#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <poll.h>
#include <dirent.h>
#include <sys/ioctl.h>

static const unsigned int VERSION_MAJOR = 0;
static const unsigned int VERSION_MINOR = 1;

/* known (supported) chip IDs */
/* 6620, 6628 and 6630 need further research */
static int chipIDs[] = {
    0x6571, 0x6572, 
    0x6580, 0x6582, 
    0x6592, 
    0x6752, 0x6735,
    0x8127, 0x8163,
    0x0321, 0x0335, 0x0337, // these are 0x6735
    0x6768 // MT6768 (A31) connsys
};

typedef struct 
{
    int id;
    const char* patch;
} PatchEntry;

static PatchEntry patchNames[] = {
    { 0x6571, "ROMv2_patch" },    { 0x6572, "ROMv1_patch" }, 
    { 0x6580, "ROMv2_lm_patch" }, { 0x6582, "ROMv1_patch" }, 
    { 0x6592, "ROMv1_patch" },
    { 0x6752, "ROMv2_lm_patch" }, { 0x6735, "ROMv2_lm_patch" },
    { 0x8127, "ROMv2_patch" },    { 0x8163, "ROMv2_lm_patch" },
    { 0x6768, "soc1_0" } // MT6768 connsys: soc1_0_ram_wifi/bt/mcu + soc1_0_patch_mcu
};
static const int patchNamesSize = sizeof( patchNames ) / sizeof( PatchEntry );

static const char* WMT_DEVICE = "/dev/stpwmt";

static const char* DEFAULT_FIRMWARE_FOLDER = "/lib/firmware";

static const int POLLING_TIMEOUT = 2000; /* ms */

static const int MAX_COMMAND_LENGTH = NAME_MAX + 1;

typedef struct 
{
    const char* name;
    int (*callback)();
} CommandEntry;

/* drivers/misc/mediatek/connectivity/common/common_main/core/wmt_ctrl.c */
int search_patch_callback();
int search_rom_patch_callback();

static CommandEntry commandLookup[] = {
    { "srh_patch", search_patch_callback },
    { "srh_rom_patch", search_rom_patch_callback }
};
static const int commandLookupSize = sizeof( commandLookup ) / sizeof( CommandEntry );

/* drivers/misc/mediatek/connectivity/common/common_main/linux/include/wmt_dev.h */
enum STP_MODE
{
    UART_FULL = 0x01,
    UART_MAND = 0x02,
    BTIF_FULL = 0x03,
    SDIO = 0x04
};

enum FM_MODE
{
    I2C     = 0x01,
    COMM_IF = 0x02
};

/* drivers/misc/mediatek/connectivity/common/common_main/core/include/wmt_lib.h */
typedef struct
{
    unsigned int downloadSeq;
    char address[ 4 ];
    char patchName[ 256 ];
} WMT_PATCH_INFO;

/* ioctl commands */
/* drivers/misc/mediatek/connectivity/common/common_main/linux/wmt_dev.c */
#define WMT_IOCTL_SET_PATCH_NAME        _IOW(0xA0, 4, char*)
#define WMT_IOCTL_SET_STP_MODE          _IOW(0xA0, 5, int)
#define WMT_IOCTL_LPBK_POWER_CTRL       _IOW(0xA0, 7, int)
#define WMT_IOCTL_GET_CHIP_INFO         _IOR(0xA0, 12, int)
#define WMT_IOCTL_SET_LAUNCHER_KILL     _IOW(0xA0, 13, int)
#define WMT_IOCTL_SET_PATCH_NUM         _IOW(0xA0, 14, int)
#define WMT_IOCTL_SET_PATCH_INFO        _IOW(0xA0, 15, char*)
#define WMT_IOCTL_WMT_QUERY_CHIPID      _IOR(0xA0, 22, int)
#define WMT_IOCTL_SET_ROM_PATCH_INFO    _IOW(0xA0, 31, char*)

/* connsys SOC rom patch (MT6768): struct wmt_rom_patch_info from wmt_lib.h
 * { UINT32 type; UINT8 addRess[4]; UINT8 patchName[256]; } = 264 bytes
 * addRess[4] = u4PatchAddr at offset 24 of the firmware header (struct wmt_rom_patch) */
struct wmt_rom_patch_info_u
{
    unsigned int type;
    unsigned char addRess[4];
    unsigned char patchName[256];
};

/* WMTDRV_TYPE enum: BT=0, FM=1, GPS=2, WIFI=3, WMT=4, ANT=5 */
typedef struct { int type; const char* file; } RomPatchEntry;
static RomPatchEntry romPatches[] = {
    { 0, "soc1_0_ram_bt_1a_1_hdr.bin" },    /* WMTDRV_TYPE_BT   */
    { 3, "soc1_0_ram_wifi_1a_1_hdr.bin" },  /* WMTDRV_TYPE_WIFI */
    { 4, "soc1_0_ram_mcu_1a_1_hdr.bin" }    /* WMTDRV_TYPE_WMT  */
    /* TODO BTIF handshake fails after EMI download — MCU not booting.
       Suspects: also need patch_mcu via the srh_patch path; or a clock/reset
       step Android userspace does; or WMT.cfg/wifi.cfg parsing (failed earlier).
       Test cleanly after a reboot (kernel caches rom patch info). */
};
static const int romPatchesSize = sizeof( romPatches ) / sizeof( RomPatchEntry );

#define LOG_TAG "[mtdaemon]"
#define LOG_DEBUG( fmt, ... )   fprintf( stdout, LOG_TAG "[dbg] " fmt "\n", ##__VA_ARGS__ )
#define LOG_INFO( fmt, ... )    fprintf( stdout, LOG_TAG "[inf] " fmt "\n", ##__VA_ARGS__ )
#define LOG_ERROR( fmt, ... )   fprintf( stderr, LOG_TAG "[err] " fmt "\n", ##__VA_ARGS__ )

/* globals are bad */
static volatile sig_atomic_t running = 1;
static int wmtFD = -1;
char firmwareFolder[ PATH_MAX + 1 ] = { 0 };

static void signalHandler( int signal )
{
    ioctl( wmtFD, WMT_IOCTL_SET_LAUNCHER_KILL, 1 );
    running = 0;
}

void setupSignalHandlers()
{
    struct sigaction sa;
    
    memset( &sa, 0x00, sizeof( sa ) );
    sa.sa_flags = SA_NOCLDSTOP;
    sa.sa_handler = SIG_IGN;
    sigaction( SIGCHLD, &sa, NULL );
    sigaction( SIGPIPE, &sa, NULL );
    sigaction( SIGHUP, &sa, NULL );

    sa.sa_handler = signalHandler;
    sigaction( SIGTERM, &sa, NULL );
    sigaction( SIGINT, &sa, NULL );    
}

/* return the apropriate chip ID or -1 if unknown */
int validateChipID( int chipID )
{
    int properChipID = -1;

    for ( int i = 0; i < sizeof( chipIDs ) / sizeof( int ); i++ )
    {
        if ( chipID == chipIDs[ i ] )
        {
            properChipID = chipID;
            break;
        }
    }

    switch( properChipID )
    {
        case 0x0321:
        case 0x0335:
        case 0x0337:
            properChipID = 0x6735;
            break;
        default:
            break;
    }

    return properChipID;
}

void* powerOn( void* arg )
{
    /* turn off the chip first in case its already on s*/
    ioctl( wmtFD, WMT_IOCTL_LPBK_POWER_CTRL, 0 );

    /* this triggers the driver to send commands to the main thread */
    LOG_INFO( "trying to power on the chip" );
    if ( ioctl( wmtFD, WMT_IOCTL_LPBK_POWER_CTRL, 1 ) )
    {
        /* if the commands fail then the chip fails to power on */
        ioctl( wmtFD, WMT_IOCTL_LPBK_POWER_CTRL, 0 );
        LOG_ERROR( "failed to power on the chip" );
    }
    else
    {
        LOG_INFO( "chip powered on!" );
    }

    return NULL;
}

int search_patch_callback()
{
    int chipID = ioctl( wmtFD, WMT_IOCTL_GET_CHIP_INFO, 0 );
    int hardwareVersion = ioctl( wmtFD, WMT_IOCTL_GET_CHIP_INFO, 1 );
    int firmwareVersion = ioctl( wmtFD, WMT_IOCTL_GET_CHIP_INFO, 2 );

    /* assume its valid at this point */
    int properChipID = validateChipID( chipID );

    LOG_INFO( "chip ID: 0x%04x (0x%04x) hardware: 0x%04x firmware: 0x%04x", properChipID, chipID, hardwareVersion, firmwareVersion );

    const char* patchName = NULL;
    for( int i = 0; i < patchNamesSize; i++ )
    {
        if ( patchNames[ i ].id == properChipID )
        {
            patchName = patchNames[ i ].patch;
        }
    }

    if ( patchName == NULL )
    {
        /* this shouldnt happen if both chip and patch arrays are in sync */
        LOG_ERROR( "patch filename for chip ID is not set", properChipID );
        return -2;
    }

    LOG_INFO( "checking folder: %s", firmwareFolder );

    /* search the specified folder and locate the firmware files */
    if ( access( firmwareFolder, F_OK ) )
    {
        LOG_ERROR( "folder not found: %s", firmwareFolder );
        return -1;
    }

    if ( access( firmwareFolder, R_OK ) )
    {
        LOG_ERROR( "insufficient read/write permission to access %s", firmwareFolder );
        return -1;
    }

    DIR* folder = opendir( firmwareFolder );
    if ( folder == NULL )
    {
        LOG_ERROR( "failed to open %s", firmwareFolder );
        LOG_ERROR( "errno: %d %s", errno, strerror( errno ) );
        return -1;
    }

    /* SET_PATCH_NUM needs to be called only once */
    int patchNumberSet = 0;
    unsigned int totalPatches = 0;
    unsigned int setPatches = 0;

    struct dirent* entry = NULL;
    while( ( entry = readdir( folder ) ) != NULL )
    {
        /* check all files starting with the expected patch name */
        if ( !strncmp( entry->d_name, patchName, strlen( patchName ) ) )
        {
            /* build full path to the file */
            char filePath[ PATH_MAX + 1 ];
            memset( filePath, 0x00, sizeof( filePath ) );

            strncpy( filePath, firmwareFolder, PATH_MAX );
            if ( filePath[ strlen( filePath ) ] != '/')
            {
                strncat( filePath, "/", PATH_MAX - strlen( filePath ) );
            }
            strncat( filePath, entry->d_name, PATH_MAX - strlen( filePath ) );

            LOG_INFO( "checking patch file: %s", filePath );

            int patchFD = open( filePath, O_RDONLY );
            if ( patchFD == -1 )
            {
                LOG_ERROR( "failed to open file: %s", filePath );
                LOG_ERROR( "errno: %d %s", errno, strerror( errno ) );
                continue;
            }

            /* read patch header and verify that the firmware version matches */
            if ( lseek( patchFD, 22, SEEK_SET ) < 0 )
            {
                LOG_ERROR( "failed to seek patch header" );
                LOG_ERROR( "errno: %d %s", errno, strerror( errno ) );
                close( patchFD );
                continue;
            }

            unsigned short patchVersion = 0;
            if ( read( patchFD, ( void* )&patchVersion, sizeof( patchVersion ) ) < sizeof( patchVersion ) )
            {
                //error
                close( patchFD );
                continue;
            }

            /* chip firmware version is big endian and patch file is little endian */
            unsigned char chipFWMajor = ( firmwareVersion >> 8 ) & 0xFF;
            unsigned char chipFWMinor = firmwareVersion & 0xFF;
            unsigned char fileFWMajor = patchVersion & 0xFF;
            unsigned char fileFWMinor = ( patchVersion >> 8 ) & 0xFF;

            patchVersion = ( ( unsigned short )fileFWMajor << 8 ) | fileFWMinor;
            LOG_INFO( "patch version: 0x%04x", patchVersion );

            /* FIXME is it required to match both major and minor versions? */
            if ( ( chipFWMajor == fileFWMajor ) && ( chipFWMinor == fileFWMinor ) )
            {
                WMT_PATCH_INFO patchInfo;
                if ( read( patchFD, patchInfo.address, sizeof( patchInfo.address ) ) < sizeof( patchInfo.address ) )
                {
                    // error
                }

                /* tell the driver how many patches there are, then push them one by one */
                if ( !patchNumberSet )
                {    
                    totalPatches = ( patchInfo.address[ 0 ] & 0xF0 ) >> 4;
                    ioctl( wmtFD, WMT_IOCTL_SET_PATCH_NUM, totalPatches );
                    LOG_INFO( "total number of patches: %u", totalPatches );
                    patchNumberSet = 1;
                }

                patchInfo.downloadSeq = patchInfo.address[ 0 ] & 0x0F;
                LOG_INFO( "patch file sequence: %u/%u", patchInfo.downloadSeq, totalPatches );

                patchInfo.address[ 0 ] = 0x00; /* remove sequence data */

                /* 
                    FIXME on Android the full path is copied, but Linux needs the relative path
                    from its defined firmware folder. So is it fine this way?
                 */
                strncpy( patchInfo.patchName, entry->d_name, sizeof( patchInfo.patchName ) );
                ioctl( wmtFD, WMT_IOCTL_SET_PATCH_INFO, &patchInfo );
                setPatches++;
            }

            close( patchFD );
        }
    }
    closedir( folder );
    
    if ( patchNumberSet )
    {
        if ( setPatches != totalPatches )
        {
            LOG_ERROR( "number of set patches differs from the expected: %u of %u", setPatches, totalPatches );
            return -3;
        }

        LOG_INFO( "patches set: %u of %u", setPatches, totalPatches );
    }
    else
    {
        LOG_ERROR( "no firmware files found" );
        return -3;
    }

    return 0;
}

/* connsys SOC rom patch: tell the kernel the firmware filename + EMI address
 * for each subsystem (BT/WIFI/WMT). Triggered by the "srh_rom_patch" command. */
int search_rom_patch_callback()
{
    LOG_INFO( "srh_rom_patch: setting connsys SOC rom patch info" );
    for ( int i = 0; i < romPatchesSize; i++ )
    {
        char filePath[ PATH_MAX + 1 ];
        memset( filePath, 0x00, sizeof( filePath ) );
        strncpy( filePath, firmwareFolder, PATH_MAX );
        if ( strlen( filePath ) > 0 && filePath[ strlen( filePath ) - 1 ] != '/' )
            strncat( filePath, "/", PATH_MAX - strlen( filePath ) );
        strncat( filePath, romPatches[ i ].file, PATH_MAX - strlen( filePath ) );

        int fd = open( filePath, O_RDONLY );
        if ( fd < 0 )
        {
            LOG_ERROR( "rom patch file not found: %s (errno %d)", filePath, errno );
            continue;
        }

        struct wmt_rom_patch_info_u info;
        memset( &info, 0x00, sizeof( info ) );
        info.type = romPatches[ i ].type;

        /* u4PatchAddr is at offset 24 of the firmware header (struct wmt_rom_patch) */
        if ( lseek( fd, 24, SEEK_SET ) < 0 || read( fd, info.addRess, 4 ) != 4 )
        {
            LOG_ERROR( "failed to read patch address from %s", filePath );
            close( fd );
            continue;
        }
        close( fd );

        strncpy( (char*)info.patchName, romPatches[ i ].file, sizeof( info.patchName ) - 1 );

        LOG_INFO( "rom patch type %d: %s addr 0x%02x%02x%02x%02x", info.type, info.patchName,
            info.addRess[3], info.addRess[2], info.addRess[1], info.addRess[0] );

        if ( ioctl( wmtFD, WMT_IOCTL_SET_ROM_PATCH_INFO, &info ) < 0 )
            LOG_ERROR( "SET_ROM_PATCH_INFO failed for type %d (errno %d)", info.type, errno );
    }
    return 0;
}

int main( int argc, char* argv[] )
{
    LOG_INFO( "Open MT Tools - mtdaemon v%u.%u", VERSION_MAJOR, VERSION_MINOR );

    strncpy( firmwareFolder, DEFAULT_FIRMWARE_FOLDER, PATH_MAX );

    int stpMode = BTIF_FULL;
    int fmMode = COMM_IF;

    /* process parameters */
    /* FIXME n, d, b and c are missing */
    const char* parameters = "m:p:n:d:b:c:?";
    opterr = 0; /* disable error messages */
    int parameter = -1;
    do {
        switch( parameter )
        {
            case -1: break;
            
            case 'p': /* custom firmware folder path */
                strncpy( firmwareFolder, optarg, PATH_MAX );
                break;
            case 'm': /* STP mode */
                stpMode = atoi( optarg );
                break;
            case '?': /* usage */
                LOG_INFO( "usage: mtdaemon [-m <stp mode>] [-p <firmware folder path>] ");
                return 0;
            break;
            default:
                LOG_ERROR( "option %c is not implemented (yet)", (char)parameter );
        }
        parameter = getopt( argc, argv, parameters );
    } while( parameter != -1 );

    LOG_INFO( "initialization started" );

    /* find and open the WMT device */
    if ( access( WMT_DEVICE, F_OK ) )
    {
        LOG_ERROR( "WMT device not found: %s", WMT_DEVICE );
        return 0;
    }

    if ( access( WMT_DEVICE, R_OK | W_OK ) )
    {
        LOG_ERROR( "insufficient read/write permission to access %s", WMT_DEVICE );
        return 0;
    }

    wmtFD = open( WMT_DEVICE, O_RDWR | O_NOCTTY );
    if ( wmtFD < 0 )
    {
        LOG_ERROR( "failed to open %s", WMT_DEVICE );
        LOG_ERROR( "errno: %d %s", errno, strerror( errno ) );
        return -1;
    }

    LOG_INFO( "WMT device open: %s", WMT_DEVICE );

    /* retrieve and validate chip ID */
    int chipID = ioctl( wmtFD, WMT_IOCTL_WMT_QUERY_CHIPID, NULL );
    int properChipID = validateChipID( chipID );

    if ( properChipID == -1 )
    {
        LOG_ERROR( "unknown chip ID. this code may need some updates" );
        close( wmtFD );
        return 0;
    }

    LOG_INFO( "the chip ID is 0x%04x (0x%04x)", properChipID, chipID );

    /* reset patch name */
    char patchName[ NAME_MAX + 1 ];
    memset( patchName, 0x00, sizeof( patchName ) );
    ioctl( wmtFD, WMT_IOCTL_SET_PATCH_NAME, patchName );
    
    /* TODO: UART mode is not supported yet */
    if ( stpMode != BTIF_FULL )
    {
        LOG_ERROR( "mode %d is not implemented (yet)", stpMode );
        close( wmtFD );
        return 0;
    }

    int mode = ( ( fmMode & 0x0F ) << 4 ) | ( stpMode & 0x0F );
    ioctl( wmtFD, WMT_IOCTL_SET_STP_MODE, mode );
    
    ioctl( wmtFD, WMT_IOCTL_SET_LAUNCHER_KILL, 0);

    setupSignalHandlers();

    LOG_INFO( "initialization finished" );

    /* 
        command loop needs to execute while the power ioctl blocks,
        so they cannot run synchronously
    */
    pthread_t powerThread;
    if ( pthread_create( &powerThread, NULL, powerOn, NULL ) )
    {
        LOG_ERROR( "failed to create secondary thread" );
    }
    
    struct pollfd fd;
    fd.fd = wmtFD;
    fd.events = POLLIN | POLLRDNORM;

    LOG_INFO( "command loop started" );
    while ( running )
    {
        fd.revents = 0;
        int pollRet = poll( &fd, 1, POLLING_TIMEOUT );
        if ( pollRet < 0 )
        {
            if ( errno == EINTR)
            {
                continue;
            }
            else
            {
                LOG_ERROR( "error polling %s: %d", WMT_DEVICE, pollRet );
                LOG_ERROR( "errno: %d %s", errno, strerror( errno ) );
                break;
            }
        }
        
        if ( fd.revents & POLLIN )
        {
            /* receive the command from the driver */
            char request[ MAX_COMMAND_LENGTH ];
            int requestLength = read( fd.fd, request, sizeof( request ) - 1 );
            if ( requestLength < 0 || requestLength >= ( sizeof( request ) - 1 ) )
            {
                LOG_ERROR( "error reading %s: %d", WMT_DEVICE, requestLength );
                continue;
            }
            request[ requestLength ] = '\0';

            LOG_INFO( "requested command: %s", request );

            /* find and execute the associated callback */
            int commandResult = 1;
            for( int i = 0; i < commandLookupSize; i++ )
            {
                if ( !strcmp(  commandLookup[ i ].name, request ) )
                {
                    commandResult = (*commandLookup[ i ].callback)();
                    break;
                }
            }

            /* send back an apropriate response */
            char response[ MAX_COMMAND_LENGTH ];
            switch( commandResult )
            {
                case 0:
                    snprintf( response, sizeof( response ), "ok" );
                    break;
                case 1:
                    snprintf( response, sizeof( response ), "cmd not found" );
                    break;
                default:
                    snprintf( response, sizeof( response ), "resp_%d", commandResult );
            }

            LOG_INFO( "command result: %s", response );

            int responseLength = strlen( response );
            int written = write( fd.fd, response, responseLength );
            if ( written != responseLength )
            {
                LOG_ERROR( "error writing %s: %d/%d", WMT_DEVICE, written, responseLength );
                if ( written < 0 )
                {
                    LOG_ERROR( "errno: %d %s", errno, strerror( errno ) );
                }
                continue;
            }
        }
    }

    LOG_INFO( "command loop finished" );

    if ( wmtFD > -1 )
    {
        close( wmtFD );
        wmtFD = -1;
    }

    pthread_join( powerThread, NULL );
    
    return 0;
}
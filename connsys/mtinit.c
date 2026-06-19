#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

static const unsigned int VERSION_MAJOR = 0;
static const unsigned int VERSION_MINOR = 9;

static const char* LOADER_DEVICE = "/dev/wmtdetect";

/* standard mediatek devices */
static const char* WMT_DEVICE = "/dev/stpwmt";
static const char* WIFI_DEVICE = "/dev/wmtWifi";
static const char* BLUETOOTH_DEVICE = "/dev/stpbt";
static const char* GPS_DEVICE = "/dev/stpgps";
static const char* ANT_DEVICE = "/dev/stpant";

/* known chip IDs */
static int chipIDs[] = {
    0x6620, 0x6628, 0x6630, 
    0x6571, 0x6572, 
    0x6580, 0x6582, 0x6592, 
    0x8127, 0x8163,
    0x6752, 0x6735,
    0x0321, 0x0335, 0x0337, // these are 0x6735
    0x6768 // MT6768 (A31) connsys CONNAC1.x
};

/* ioctl commands */
/* drivers/misc/mediatek/connectivity/common/common_detect/wmt_detect.c */
#define COMBO_IOCTL_GET_CHIP_ID       _IOR('w', 0, int)
#define COMBO_IOCTL_SET_CHIP_ID       _IOW('w', 1, int)
#define COMBO_IOCTL_EXT_CHIP_DETECT   _IOR('w', 2, int)
#define COMBO_IOCTL_GET_SOC_CHIP_ID   _IOR('w', 3, int)
#define COMBO_IOCTL_DO_MODULE_INIT    _IOR('w', 4, int)
#define COMBO_IOCTL_MODULE_CLEANUP    _IOR('w', 5, int)
#define COMBO_IOCTL_EXT_CHIP_PWR_ON   _IOR('w', 6, int)
#define COMBO_IOCTL_EXT_CHIP_PWR_OFF  _IOR('w', 7, int)
#define COMBO_IOCTL_DO_SDIO_AUTOK     _IOR('w', 8, int)

#define LOG_TAG "[mtinit]"
#define LOG_DEBUG( fmt, ... )   fprintf( stdout, LOG_TAG "[dbg] " fmt "\n", ##__VA_ARGS__ )
#define LOG_INFO( fmt, ... )    fprintf( stdout, LOG_TAG "[inf] " fmt "\n", ##__VA_ARGS__ )
#define LOG_ERROR( fmt, ... )   fprintf( stderr, LOG_TAG "[err] " fmt "\n", ##__VA_ARGS__ )

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

int main( int argc, char* argv[] )
{
    LOG_INFO( "Open MT Tools - mtinit v%u.%u", VERSION_MAJOR, VERSION_MINOR );

    int avoidReinitialization = 0;
    if ( argc == 2 && !strcmp( argv[ 1 ], "-safe" ) )
    {
        avoidReinitialization = 1;
    }

    if ( avoidReinitialization )
    {
        /* try to access the standard devices to hopefully avoid re-initialization */
        /* TODO maybe the WMT one is enough in all cases? */
        int found = 0;
        if ( !access( WMT_DEVICE, F_OK ) )
        {
            LOG_INFO("WMT device is already present");
            found = 1;
        }
        if ( !access( WIFI_DEVICE, F_OK ) )
        {
            LOG_INFO("WiFi device is already present");
            found = 1;
        }
        if ( !access( BLUETOOTH_DEVICE, F_OK ) )
        { 
            LOG_INFO("Bluetooth device is already present"); 
            found = 1;
        }
        if ( !access( GPS_DEVICE, F_OK ) )
        { 
            LOG_INFO("GPS device is already present"); 
            found = 1;
        }
        if ( !access( ANT_DEVICE, F_OK ) )
        { 
            LOG_INFO("ANT device is already present"); 
            found = 1;
        }

        if ( found )
        {
            LOG_INFO( "Skipping initialization" );
            return 0;
        }
    }

    LOG_INFO( "initialization started" );

    /* find and open the loader device */
    if ( access( LOADER_DEVICE, F_OK ) )
    {
        LOG_ERROR( "loader device not found: %s", LOADER_DEVICE );
        return 0;
    }

    if ( access( LOADER_DEVICE, R_OK | W_OK ) )
    {
        LOG_ERROR( "insufficient read/write permission to access %s", LOADER_DEVICE );
        return 0;
    }

    int loaderFD = open(LOADER_DEVICE, O_RDWR | O_NOCTTY );
    if ( loaderFD < 0 )
    {
        LOG_ERROR( "failed to open %s", LOADER_DEVICE );
        return -1;
    }

    LOG_INFO( "loader device open: %s", LOADER_DEVICE );

    /* chip ID detection */
    int chipID = -1;
    int internalChip = -1;

    if ( ioctl( loaderFD, COMBO_IOCTL_EXT_CHIP_PWR_ON ) < 0 )
    {
        LOG_INFO( "failed to power on external chip" );
        internalChip = 1;
    }
    else
    {
        LOG_INFO("detecting if the chip is internal or external");
        internalChip = ioctl( loaderFD, COMBO_IOCTL_EXT_CHIP_DETECT, NULL );
    }
    
    if ( internalChip )
    {
        chipID = ioctl( loaderFD, COMBO_IOCTL_GET_SOC_CHIP_ID, NULL );
        LOG_INFO( "SOC chip detected with ID 0x%04x", chipID );
    }
    else
    {
        chipID = ioctl( loaderFD, COMBO_IOCTL_GET_CHIP_ID, NULL );
        LOG_INFO( "external chip detected with ID 0x%04x", chipID );
        
        int autok = ioctl( loaderFD, COMBO_IOCTL_DO_SDIO_AUTOK, chipID );
        LOG_INFO( "executing SDIO AUTOK %s", autok == 0 ? "succeeded" :  "failed" );
    }

    /* turn off the chip if its external */
    if ( !internalChip )
    {
        if ( ioctl( loaderFD, COMBO_IOCTL_EXT_CHIP_PWR_OFF ) < 0 )
        {
            LOG_ERROR( "failed to power off external chip" );
            close( loaderFD );
            return -1;
        }
    }

    if ( internalChip && chipID == -1 )
    {
        LOG_ERROR( "failed to retrieve the chip ID. Try again" );
        close( loaderFD );
        return -1;
    }

    /* check if the chip is known and get the proper ID */
    int properChipID = validateChipID( chipID );
    if ( properChipID < 0 )
    {
        /* unknown chip ID */
        LOG_ERROR( "unknown chip ID. this code may need some updates" );
        close( loaderFD );
        return -1;
    }

    LOG_INFO( "the chip ID is 0x%04x (0x%04x)", properChipID, chipID );

    /* set chip ID in the kernel */
    if ( ioctl( loaderFD, COMBO_IOCTL_SET_CHIP_ID, chipID ) < 0 )
    {
        LOG_ERROR( "failed to set the chip ID in the kernel" );
    }

    /* initialize kernel modules (drivers) */
    LOG_INFO( "initializing kernel modules" );

    /* unregister SDIO driver. only works once */
    if ( ioctl(  loaderFD, COMBO_IOCTL_MODULE_CLEANUP, properChipID ) < 0 )
    {
        LOG_ERROR( "failed to cleanup modules" );
        close( loaderFD );
        return -1;
    }

    if (ioctl( loaderFD, COMBO_IOCTL_DO_MODULE_INIT, properChipID ) < 0 )
    {
        LOG_ERROR( "failed to initialize modules" );
        close( loaderFD );
        return -1;
    }

    LOG_INFO( "kernel modules initialized successfully" );

    if ( loaderFD > -1 )
    {
        close( loaderFD );
        loaderFD = -1;
    }

    /* TODO set proper permissions on the new devices */
    
    LOG_INFO( "initialization finished" );
    return 0;
}
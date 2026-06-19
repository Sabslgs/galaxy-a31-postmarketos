#!/bin/sh
# speaker.sh - route audio out the loudspeaker.
# Run on the phone as root. The path is DL (playback) -> I2S1 -> the external
# SMA1303 smart amp. None of this comes up by default; you set it by hand with
# amixer. See docs/audio.md.
set +e
C=0   # card 0 = mt6768-mt6358

# connect the playback streams into I2S1
for ctl in "I2S1_CH1 DL1_CH1" "I2S1_CH2 DL1_CH2" "I2S1_CH1 DL2_CH1" "I2S1_CH2 DL2_CH2"; do
    amixer -c$C cset name="$ctl" on >/dev/null 2>&1
done

# power up the external amp and unmute
amixer -c$C cset name="Ext_Speaker_Amp" 1 >/dev/null 2>&1
amixer -c$C cset name="Power Up(1:Up_0:Down)" 1 >/dev/null 2>&1
amixer -c$C cset name="Force AMP Power Down" 0 >/dev/null 2>&1
amixer -c$C cset name="Speaker Mute Switch(1:muted_0:un)" 0 >/dev/null 2>&1

# keep the gain moderate - past ~60% this amp clips hard
amixer -c$C sset "Speaker Volume" 45% >/dev/null 2>&1
amixer -c$C sset "Speaker" 95      >/dev/null 2>&1

echo "speaker routed. test with: aplay /usr/share/sounds/alsa/Front_Center.wav"

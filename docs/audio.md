# Audio (speaker)

Sound was one of the nicer surprises in this port. It works, and it didn't even take that long once I figured out how the routing is wired. This page is just the speaker path. Headphone and mic live on the same codec and I got those going too, but the loudspeaker is the one I'll walk through here because it's the one most people want first.

The card is `card0`, name `mt6768-mt6358`. That's the MT6358 PMIC codec on the MediaTek side. The catch on the A31 is that the loudspeaker isn't driven by the codec's analog LINEOUT. It goes out **digitally over I2S1** into an external class-D smart amp, an **SMA1303**, sitting on I2C at `0x1e`. So getting audio to the speaker is really two jobs: route playback to I2S1, and power up the SMA1303 yourself.

That second part is the gotcha. The machine driver has an `spk_amp_event` callback that's supposed to bring the amp up when DAPM activates the speaker widget, but on this codebase it's an **empty stub**. It does nothing. So no matter how you poke DAPM, the amp stays asleep unless you flip its controls by hand. Took me a while to accept that the driver wasn't going to do it for me.

## The routing, by hand

Everything is `amixer -c0`. The path is DL (download, i.e. playback) -> I2S1 -> SMA1303 -> speaker.

First, route the two playback streams (DL1 and DL2) into both I2S1 channels:

```sh
amixer -c0 cset name='I2S1_CH1 DL1_CH1' on
amixer -c0 cset name='I2S1_CH2 DL1_CH2' on
amixer -c0 cset name='I2S1_CH1 DL2_CH1' on
amixer -c0 cset name='I2S1_CH2 DL2_CH2' on
```

Then enable the external amp widget and bring the SMA1303 itself up:

```sh
amixer -c0 cset name='Ext_Speaker_Amp' 1
amixer -c0 cset name='Power Up(1:Up_0:Down)' 1
amixer -c0 cset name='Force AMP Power Down' 0
amixer -c0 cset name='Speaker Mute Switch(1:muted_0:un)' 0
amixer -c0 sset 'Speaker Volume' 45%
amixer -c0 sset 'Speaker' 150
```

Those control names are verbatim, parentheses and all. The SMA1303 driver names them that way and you have to match exactly or `amixer` just errors out.

A note on levels. I had `Speaker Volume` at 80% while debugging and it clipped horribly, sounded like a blown driver. Drop it to around **45%** and it's clean. The `Speaker` gain at 150 is the analog gain step on the amp. You can nudge both, but past roughly half the volume control the class-D just distorts, so keep it moderate.

## Playing something

Once routed, any raw ALSA playback comes out the speaker:

```sh
aplay -D hw:0,0 something.wav
# or
ffmpeg -f alsa default -i ...    # ffmpeg -f alsa for capture; for playback aplay is simplest
speaker-test -D hw:0,0 -c2 -twav
```

I confirmed it by watching the PCM state go to **RUNNING** in `/proc/asound/card0/pcm0p/sub0/status` and, you know, actually hearing it. That part's hard to fake.

One warning I'll repeat from the kernel notes: don't point PipeWire or PulseAudio at this card yet. The MediaTek legacy PCM driver (`mtk-soc-pcm-dl1.c`) has a dummy `mmap`/page path that **panics the kernel** when a sound server mmaps the device. Raw `aplay` doesn't mmap, so it's fine. There's a driver patch to drop those ops (pmaports has a similar fix for amazon-biscuit, MR!6233) but until that's in, stick to raw ALSA.

## Making it stick

Two ways I persist this. `alsactl store` writes the whole mixer state to `/var/lib/alsa/asound.state` so it reloads on boot. And the full amixer sequence lives in [`scripts/speaker.sh`](../scripts/speaker.sh), which I run at startup. The script is just the commands above wrapped up so I don't have to remember them.

If you're tethered and poking at this over SSH, open the audio device detached. Touching the card can momentarily drop the usb0 link mid-command, which is annoying but harmless.

## What's not here

Headphones (MT6358 internal analog via the ADDA path and the HPL/HPR muxes) and the mic both work on the same `mt6768-mt6358` card, and I even have automatic speaker/headphone switching off the ACCDET jack-detect input. I kept this page to the speaker on purpose. Graphics on this device are software-rendered on plain fbdev, so none of this touches anything fancy on the display side.

If someone finds a cleaner way to bring up the SMA1303, or knows why that `spk_amp_event` stub is empty upstream, open an issue. I'd genuinely like to know.

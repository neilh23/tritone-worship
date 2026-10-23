TritoneWorship is a multiplatform audio plugin, written in C, with simple UI.
The initial drop targets Linux (VST3, CLAP, LV2 and native LMMS) and Windows (VST3, CLAP and LV2, cross-compiled from Linux). macOS builds are possible from source but untested.

The plugin replicates the sound at intervals of minor thirds, each step reducing in volume by a percentage.
There is a cut-off in replacation once a frequency of approx 18khz is reached

The following controls are available:
* Amount: controles the percentage by which the volume - the scale is 0-100, with 0 amount reducing by 70% at each step, and 100 not reducing. The curve is linear in dB between the two.
* Death: scale 0-100. This recreates the tone an octave lower, and stacks 3 minor thirds on top of that - the octave below will be set to the same volume as the normal tone, multiplied by the percentage value of 'Death' (logarithm scale: 0 is off, 1-100 runs linearly in dB from -28 dB to 0 dB), then  each 3rd on top will be reduced from that by the 'amount' percentage.
* Blacken: on/off. Applies an EQ specifically designed to mimic badly-recorded black metal - do some research on frequencies that are known to be unpleasant to the listener. This is a flip switch with a light above it that shows when Blacken is in operation, no amounts or tweaking. The 20-band EQ controls (with frequency values) are only shown in tuning builds (make EQ_TUNING=1). Blacken also applies a soft-clip distortion before the EQ, and a fixed output trim (currently -0.6 dB) set so that switching it on is about 3 dB louder than switching it off. While Death is above 0, the bass bands (250 Hz and below) are left flat so the octave below is not cut.
* Tremelo amount: scale 0-100. Amount to apply tremolo effect. The effect should be designed to emulate the sound of tremolo picking on a guitar. 0 is off; 1-100 maps to 30-100% depth, since lower depths are inaudible.
* Tremelo speed: BPM, 666-1666 . Speed of tremelo in bpm
* Bypass: footswitch at the bottom of the pedal that switches the effect off entirely (input passes through unchanged). A status light at the top of the pedal is lit while the effect is engaged.

The look of UI should be designed to mimic a black boutique guitar pedal with a fairly simple design

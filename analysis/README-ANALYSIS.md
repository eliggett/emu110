# Roland U-110 Emulator Analysis TOC

## PDFs:

1. sample212_trapezoid.pdf: Shows the hidden test waveform, which is a sine wave (claude thought it might be a trapezoid initially). This sine wave was the key to understanding how to unpack the 8 bit values to their 16 bit "expanded" values. This waveform is played back when the system is in one of the test modes. The ultimate decoder curve is shown in decoder_curve.pdf

2. deemphasis.pdf: Initially, it seemed like the wave rom audio had some pre-emphasis applied. This was both correct and incorrect: What was happening, is the data were recorded as differential values, effectively the dn/dt of the audio data. This has the effect of appearing to be a high-pass filter on raw playback. However, if you implement an integrator (accumulator), the HPF effect is counteracted perfectly, and the audio is flat. This PDF shows the frequency response of audio recorded from the hardware synth compared with the raw wave rom. It's not really a valid comparison anymore but it is kept here because it's neat to examine. 

3. hf_excess.pdf: This shows the frequency-domain (spectral) error between our emulated output and the hardawre output. Initially, the LPF filters were implemented with the wrong sample rate (48k vs 32k), and this caused an excess of high frequency energy in the output. This is now corrected, although there is a minor discrepancy around 10 KHz which I can live with. The PDF shows the effect of the correct filter, which is implemented in the code. 

4. pingpong.pdf: This PDF shows some of the pain encountered trying to seamlessly stitch waves together which were using the "ping pong" method of stitching. There were initially all sorts of clicks, pops, and thumps. The ping pong loop is especially tricky as it reverses the direction and phase of the loop region each time, thus making a "seamless" transition at the region of transition, but also making an enormous transient on any storage elements in any filtering code. Once we understood that an accumulator was needed, and once we stopped screwing with the wave data ("...maybe we should add an interpolated set of points..." etc), the wavwe played back perfectly in loop. 

5. decoder_curve.pdf: Shows the result of analyzing the embedded test sine wave in the wave rom, which showed how to expand the 8 bit data to 16 bit. This reflects what we're doing in the code. 

## Markdown: 

0. README-ANALYSIS.md: this file...

1. SYSTEM-DESIGN.md: Explains the design of the system, largly based on looking through the service manual (which is in the reference directory). Each time we discover something about the design (especially the mystery chips), this document is updated. 

2. ROM-ANALYSIS.md: Explains how the ROM is organized. Because this is a "working document", there is a massive "9. Corrections log" section. I should probably re-write this at some point. 


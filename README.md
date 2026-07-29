# Extreme Stretch

Paul's Extreme Sound Stretch, rebuilt as a JUCE 8 plugin, standalone app and
command-line renderer — with four sibling algorithms and a destructive half the
original never had.

## Credit where it is owed

The algorithm is **Nasca Octavian Paul's**. He wrote *Paul's Extreme Sound
Stretch* (and ZynAddSubFX), and the idea at the heart of this — throw the phases
away, replace them with noise, overlap enormous windows — is entirely his. It is
one of those rare techniques that is both trivially simple to state and sounds
like nothing else. Everything here exists because he thought of it first.

This is an **independent reimplementation written from the algorithm**, not a
port or a translation of his code: the DSP was built from the method and the
window maths, and none of his source is included or derived from. Credit for the
invention is his; any bugs in this version are ours.

If you want the original, look up *Paul's Extreme Sound Stretch* and his
`paulstretch_python` reference scripts.

| target | what it is |
| --- | --- |
| `Extreme Stretch.vst3` | VST3 plugin (file player + live-input smearer) |
| `Extreme Stretch` | standalone app |
| `extremestretch-render` | headless CLI renderer |
| `extremestretch-selftest` | 97 assertions over the shipping DSP |

## Build

Needs JUCE 8 installed (this machine has 8.0.13 at `/usr/local/lib/cmake/JUCE-8.0.13`).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/ExtremeStretchSelfTest_artefacts/Release/extremestretch-selftest   # must print ALL PASS
```

The VST3 is copied into `~/.vst3/` after a successful build. Turn that off with
`-DES_COPY_PLUGIN=OFF` if you want everything to stay inside `build/`.

## CLI

```sh
extremestretch-render in.wav out.wav --stretch 50 --window 0.5
extremestretch-render in.wav out.wav --length "2h"              # ratio worked out for you
extremestretch-render in.wav out.wav --algorithm vocoder --shape 0.7
extremestretch-render in.wav out.wav --stretch 1e14 --max-seconds 60
```

`--stretch --length --algorithm --shape --window --pitch --blur --rot
--scramble --lowcut --highcut --width --max-seconds`

It prints the full output length before it starts, so you can see what you are
asking for before it writes anything.

## How long will it take to play?

The **LENGTH** field is both readout and input. It shows what the current ratio
produces; type a target into it and press Enter and the ratio is set to match:

```
3h            45 min          2:30            1:02:05
4 days        1000 years      9.5 billion years
```

STRETCH runs to **10^15×**. That is genuinely absurd on purpose — a five-minute
track at the top of the range lasts about 9.5 billion years, roughly two thirds
of the age of the universe, and the readout says so. The knob is log-mapped with
a bias so that 1×–100× still occupies the first ~40% of the travel; the top of
the sweep is where the geology happens.

At ratios that extreme the read head advances by less than a sample per frame,
so it stops being a stretch and becomes a freeze. That is honest behaviour, not
a failure — there is nothing else it could mean.

## Navigation

Click or drag anywhere on the waveform to jump there. The playhead is derived
from the render thread's read position with the FIFO backlog subtracted, so it
tracks what you are actually hearing rather than what has been rendered. A seek
keeps the overlap-add tail, so the splice crossfades over half a window instead
of clicking.

## Algorithms

| | |
| --- | --- |
| **Smooth** | paulstretch proper. Random phase per bin per frame. A cloud. |
| **Onset** | Smooth, except attacks detected in the source are re-articulated with their original phases, so drums keep their hit instead of turning to fog. This is the "new method" the original shipped as its second algorithm. |
| **Vocoder** | Phase-coherent. Tracks each bin's true frequency and advances the synthesis phase accordingly — tape slowed down rather than a cloud. At ratio 1 it is a *perfect* reconstructor (the self-test asserts correlation > 0.99); at extreme ratios it goes metallic and static. |
| **Grain** | No FFT at all. Three jittered time-domain grains per frame, Hann-windowed and overlap-added. Grittier and dirtier than any of the spectral modes. |
| **Hold** | Random phase over a time-averaged magnitude spectrum. Glassy and static — a drone-maker rather than a stretcher. |

**SHAPE** is one knob wearing five hats, and it re-labels itself:

| algorithm | SHAPE means |
| --- | --- |
| Smooth | (unused) |
| Onset | **SENSITIVITY** — how weak an attack still counts. At 0 it is bit-identical to Smooth. |
| Vocoder | **COHERENCE** |
| Grain | **SPRAY** — how far each grain wanders from the read head |
| Hold | **HOLD** — how many frames the magnitude spectrum averages over |

**Grain works in the time domain**, so BLUR / ROT / SCRAMBLE / LOW CUT / HIGH CUT
do nothing in that mode. They grey out rather than lying to you.

## The rest of the controls

**Paulstretch proper**

| | |
| --- | --- |
| `STRETCH` | 1× – 10^15× |
| `WINDOW` | 5 ms – 5 s, snapped to a power of two. Short = grainy, long = glassy |
| `FREEZE` | pins the read head; the drone hangs on one moment forever |
| `WIDTH` | 0 = channels share a phase per bin, 1 = fully decorrelated (the classic huge paulstretch stereo) |

**Destruction**

| | |
| --- | --- |
| `PITCH` | ±24 semitones by resampling the magnitude spectrum. Artefact-free, because there are no phases left to smear |
| `BLUR` | smears magnitudes across neighbouring bins. The radius grows with bin index, so the smear is constant in octaves rather than in hertz; energy is compensated so it doesn't quietly drop the level |
| `ROT` | kills bins, and the damage **persists between frames**. The knob sets an equilibrium: at 1.0 nothing heals and the drone collapses progressively to whichever partials survive; below 1.0 damage and healing balance out at a steady level. Back it off and it grows back over a few seconds |
| `SCRAMBLE` | displaces bins locally. The same swap map is applied to every channel so the stereo image survives |
| `LOW CUT` / `HIGH CUT` | 24 dB/oct spectral gating, done in the magnitude domain |

## The algorithm

Take a long window of input, window it, FFT, **throw the phases away** and
replace them with noise, IFFT, window again, overlap-add at 50%. The output hop
is `windowSize/2`; the *input* hop is `(windowSize/2) / stretch`, and that is the
only place the stretch ratio appears anywhere in the code.

**Windowing depends on how frames add**, and getting this wrong is the classic
way to build a paulstretch that tremolos:

| addition | condition | window used |
| --- | --- | --- |
| random phase (Smooth/Onset/Hold) | frames add in **power**, so `sum(w²)` must be flat | Paul's `sqrt(1 - \|x\|^2.5)` — ripples 2.16 dB, and `mean(w²) × mean(overlap) = 1.02`, which is why paulstretch is unity-gain with no normalisation anywhere |
| coherent (Vocoder) | frames add in **amplitude**, so `sum(w_a · w_s)` must be flat | `sqrt(Hann)` twice = Hann = exactly 1.0 at 50% |
| grains | windowed once only, so `sum(w)` must be flat | Hann |

A Hann applied twice with random phase swings 6 dB and tremolos audibly. The
self-test asserts the ripple and the mean.

**Threading.** A window can be 262144 points; one FFT is tens of milliseconds, so
this cannot run in the audio callback. A render thread keeps a FIFO topped up and
`processBlock` only memcpys out of it. Playback starts instantly and stays
memory-bounded regardless of the ratio — a 60 s file at 10^15× is 1.9 billion
years of audio that is never materialised anywhere. The render thread stays only
~200 ms ahead, so parameter changes are heard promptly.

## Live input

`LIVE IN` stretches whatever is arriving at the plugin input instead of a file,
through a 20-second capture ring. The read head can only fall behind at
`(1 - 1/stretch)` × realtime, so at high ratios it eventually reaches the end of
the ring and gets dragged along — the output becomes a smear of "20 seconds ago"
rather than a true stretch. That is inherent, not a bug.

In the **standalone** app, JUCE mutes the input by default and says so in a
banner. Enable it under *Options → Audio Settings* before `LIVE IN` does anything.

## State of things

Built, tested and verified end to end: `ALL PASS (97/97)`, and a real 3 s source
renders to exactly 60.00 s at 20× at +0.05 dB of the input level with its chord
(220 / 277 / 329 / 440 Hz) intact.

Not done yet:

- **Presets.** Parameters persist in the plugin state (and the loaded file path
  restores with it), but there is no preset browser.
- **Suite integration.** This should speak the shared project format alongside
  the other JUCE tools rather than being standalone.
- **Onset markers on the waveform.** The detector's attacks drive the Onset
  algorithm but are not drawn.
- The **spectrum view** shows the engine's own frame, so it only moves while the
  render thread is producing.

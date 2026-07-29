# CLAUDE.md

Guidance for Claude Code working in `~/Projects/extremestrertch/`.

**Renamed 2026-07-29:** this was `extremestrertch` (typo) and is now
`extremestretch`, both locally and on GitHub. The old repo URL 301-redirects.
Any older note telling you the typo is canonical is out of date.

## What this is

Paul's Extreme Sound Stretch as a JUCE 8 plugin + standalone + CLI renderer,
extended with destructive spectral controls. See `README.md` for the algorithm
and the control list.

**The algorithm is Nasca Octavian Paul's.** This is an independent
reimplementation written from the method, not a port of his code. Keep the
credit in `README.md`, in the `PaulStretch.h` header comment, and in the plugin
header ("after Nasca Octavian Paul") — do not quietly drop it while refactoring.

## Layout

```
Source/PaulStretch.{h,cpp}     the five algorithms — no JUCE GUI dependency
Source/StretchEngine.{h,cpp}   source, onset detection, render thread, FIFO, offline render
Source/Duration.{h,cpp}        human duration formatting and parsing
Source/PluginProcessor.{h,cpp} AudioProcessor + APVTS parameters
Source/PluginEditor.{h,cpp}    editor layout, transport, file chooser, render job
Source/Ui.{h,cpp}              LookAndFeel, Knob, WaveformView, SpectrumView
Tests/SelfTest.cpp             97 assertions, console app
Tools/Render.cpp               headless CLI renderer
```

`PaulStretch.cpp` and `StretchEngine.cpp` are compiled into *all four* targets
(`ES_CORE_SOURCES` in CMakeLists.txt), so the self-test exercises the shipping
objects rather than a copy.

## Build and verify

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
./build/ExtremeStretchSelfTest_artefacts/Release/extremestretch-selftest
```

**The self-test must print `ALL PASS`.** It is the gate — treat a single failed
assertion as a broken build. It covers the FFT contract, the window power math,
unity gain, pitch preservation, the time base, rot/blur/scramble, the spectral
filter, stereo phase behaviour, declick, clamping at both window extremes, and a
full engine round trip through a real WAV.

For an end-to-end check with real audio, use the CLI renderer rather than
driving the GUI (GUI automation on this box is not reliable — don't spend
attempts on it):

```sh
./build/ExtremeStretchRender_artefacts/Release/extremestretch-render in.wav out.wav --stretch 20
```

Note that it writes **24-bit** WAVs — analysis scripts that assume 16-bit will
report lengths inflated by exactly 1.5×.

## Things that will bite you

- **There are three windows and they are not interchangeable.** Which one a
  frame uses depends on how frames add:
  `windowPaul` for random phase (`sum(w²)` flat), `windowSqrtHann` for the
  coherent vocoder (`sum(w_a·w_s)` flat), `windowHann` for grains (windowed once,
  `sum(w)` flat). Putting a Hann on the random-phase path gives an audible ~6 dB
  tremolo at the hop rate. Self-test assertions cover the exact ripple and mean.
- **Vocoder state must be seeded, not zeroed.** `prevPhase`/`sumPhase`/`binFreq`
  are meaningless on the first frame after a reset, seek or algorithm change —
  `stateIsStale` makes that frame adopt the analysis phase instead of
  accumulating from zero. Same flag seeds `heldMags` for Hold. Clearing it early
  produces a frame of noise.
- **At extreme ratios the analysis hop rounds to zero**, and the phase vocoder's
  `synthHop / analysisHop` would divide by it. `binFreq` caches the last good
  per-sample frequency estimate for exactly this case; do not "simplify" it back
  into the delta calculation.
- **Onset frames do not consume RNG.** That is what makes
  `Algorithm::onset` at sensitivity 0 bit-identical to `Algorithm::smooth` — a
  self-test asserts it. If you add an rng call to the coherent branch, that test
  will fail, and correctly so.
- **Never render on the audio thread.** A 262144-point FFT is tens of ms.
  `processBlock` may only memcpy out of the FIFO.
- **The FIFO is a lock-free SPSC `AbstractFifo`.** Writes take no lock. The
  `SpinLock` exists *only* to keep `fifo.reset()` away from a read in progress;
  the audio thread takes it with a **try**-lock and emits silence on a miss.
  Do not add a blocking lock to the audio path.
- **Don't let the render thread fill the whole FIFO.** It is sized for the
  largest possible hop; filling it at a small window size would buffer seconds
  of audio and every knob turn would arrive that late. `targetBufferedSamples()`
  caps it at ~200 ms.
- **A window-size change flushes the overlap-add tail first** (`flushTail`).
  That tail is already windowed so it decays to exactly zero, which is what
  makes the switch silent instead of a step. Don't reorder that.
- **`setOrder()` reallocates and resets the rot `health` array**, so changing the
  window size clears accumulated rot damage.
- **DC and Nyquist bins must stay real** before the inverse transform; they are
  zeroed rather than given a random phase.
- **JUCE 8.0.13 deprecates the old `createWriterFor`** — use the
  `AudioFormatWriterOptions` overload taking `std::unique_ptr<OutputStream>&`.
- **`juce::String (someDouble, 0)` does not mean "no decimals"** — it means
  shortest round-trip, and will happily print `1000.08`. Round to an integer
  type first. This was a real bug in `Duration.cpp`.
- **Keep runtime string literals ASCII.** An em-dash in a `juce::String`
  literal renders as mojibake in the editor (it was showing as `å`). Comments
  and `std::puts` output are fine — it is only strings that reach JUCE's text
  layout.
- **The playhead comes from `readPosSnapshot` minus the FIFO backlog**, not from
  samples consumed, so it stays correct across a seek. If you change the
  buffering, `sourcePositionInSamples()` needs to change with it.

## Conventions

Parameter IDs live in `es::pid` in `PluginProcessor.h`; value formatting is on
the parameters themselves (`AudioParameterFloatAttributes`), not on the sliders —
`SliderParameterAttachment` overwrites `textFromValueFunction`, so setting it on
the slider silently does nothing.

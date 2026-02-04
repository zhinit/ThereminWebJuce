# POC Plan: Expand ThereminWebJuce

Expand the existing proof of concept at `ThereminWebJuce` to prove the three risky pieces before migrating KickWithReverb.

## Goal

A single React button that starts/stops a 140 BPM loop playing a kick sample through convolution reverb, all running in JUCE compiled to WASM.

## Milestones

### 1. Sample Playback ✅ COMPLETE

**What**: Button click triggers a single kick sample through the JUCE WASM engine.

**How**:
- Add one kick WAV file to `frontend/public/`
- In JS: fetch the WAV, decode with `AudioContext.decodeAudioData()`, copy the resulting `Float32Array` into WASM heap via `_malloc`
- In C++ engine: store a `float*` buffer + length. Add a `trigger()` method that resets a playback position to 0. In `process()`, if position is active, copy samples from buffer to output and advance position
- Expose `trigger()` and a `loadSample(uintptr_t bufferPtr, int numSamples)` method via Embind
- AudioWorklet forwards "trigger" and "loadSample" messages from React to the engine

**Implementation notes**:
- Created `dsp/sampler.cpp` with `Sampler` class (replaced `oscillator.cpp`)
- Sample loading flow: JS decodes WAV → allocates WASM heap with `_malloc` → copies with `HEAPF32.set()` → calls `loadSample(ptr, len)`
- `trigger()` resets `playbackPosition_` to 0; `process()` copies samples until position exceeds length, then outputs silence
- Known issue: first click has slightly soft transient (timing issue between init and first trigger)

**Done when**: Clicking the button plays the kick sample through speakers. ✅

### 2. Loop ✅ COMPLETE

**What**: Button toggles a 140 BPM loop that retriggers the kick every quarter note.

**How**:
- In C++ engine: add a sample counter that increments each `process()` call
- Calculate `samplesPerBeat = 60.0 / bpm * sampleRate`
- When counter crosses the beat boundary, call `trigger()` internally
- Add `setLooping(bool)` method exposed via Embind
- React button sends "loop" message to AudioWorklet

**Implementation notes**:
- Added loop variables to `Sampler` class: `bpm_` (float), `samplesPerBeat_` (size_t), `loopPosition_` (size_t), `inLoop_` (bool)
- `samplesPerBeat_` calculated in `prepare()` as `sampleRate / bpm_ * 60`
- Loop logic in `process()`: when `loopPosition_ > samplesPerBeat_`, reset to 0 and call `trigger()`
- `setLooping(true)` immediately calls `trigger()` and resets `loopPosition_` so first kick plays instantly
- Important fix: `loadSample()` sets `samplePosition_ = sampleLength_` to prevent auto-play when sample loads
- UI has two buttons: "Cue" (single trigger) and "Play/Pause" (toggle loop)
- React tracks `playbackReady` state to ensure sample is loaded before allowing loop to start

**Known issue**: First click on Play/Pause only initializes audio (no sound). Second click starts the loop. This is because browser requires user gesture to start audio, and sample loading happens asynchronously after init.

**Done when**: Clicking the button starts a looping kick at 140 BPM. Clicking again stops it. ✅

### 3. Reverb ✅ COMPLETE (with fallback)

**What**: Add reverb to the kick sound.

**Attempted approach**: `juce::dsp::Convolution` with impulse response file.

**Why it failed**: `juce::dsp::Convolution` uses background threading for IR loading. Enabling WASM pthreads (`-pthread`, `USE_PTHREADS=1`) got past the atomics errors, but JUCE's thread implementation for WASM is incomplete — `juce::Thread::createNativeThread()` and `juce::Thread::killThread()` are undefined symbols.

**Fallback**: Used `juce::Reverb` (Freeverb-based algorithmic reverb). No threading required.

**Implementation notes**:
- `reverb_.setSampleRate()` in `prepare()`
- `reverb_.setParameters()` to configure roomSize, damping, wetLevel, dryLevel, width
- `reverb_.processStereo(left, right, numSamples)` at end of `process()`
- Updated to stereo output: `process()` takes left/right buffer pointers, AudioWorklet allocates two heap buffers

**Limitation**: Algorithmic reverb sounds more "digital" than convolution. No variety from loading different IR files.

**Done when**: Kick loop has audible reverb tail. ✅

### 4. Custom Convolution Reverb ✅ COMPLETE

**What**: Build a simple convolution reverb that works in WASM without threading.

**Why**: `juce::dsp::Convolution` requires threading. A custom implementation can run entirely in the audio thread.

**Approach chosen**: FFT-based convolution using uniform partitioned overlap-add algorithm.

**Implementation notes**:
- Created `ConvolutionEngine` class in `sampler.cpp` (~200 lines)
- Algorithm based on JUCE's `juce::dsp::Convolution` internals, rewritten to run synchronously
- Uses `juce::dsp::FFT` (order 9 = 512 samples) for frequency-domain processing
- IR partitioned into 384-sample segments, each pre-FFT'd at load time
- Ring buffer of FFT'd input segments for efficient convolution with all IR segments
- Overlap-add handles reverb tail accumulation between 128-sample blocks
- `StereoConvolutionReverb` wrapper handles stereo IR files (deinterleaves channels)
- Wet/dry mix control via `setReverbMix(wetLevel, dryLevel)`

**IR loading flow**:
1. React fetches `ir.wav`, decodes with Web Audio API
2. Interleaves stereo channels into single `Float32Array`
3. Sends to AudioWorklet via `postMessage({ type: "loadIR", irSamples, irLength, numChannels })`
4. Worklet allocates WASM heap, copies data, calls `engine.loadImpulseResponse()`
5. C++ partitions IR, FFTs each segment, stores for real-time convolution

**Memory usage**: ~5.5 MB for a 1.46s stereo IR (ir.wav)

**Done when**: Can load an IR file and hear convolution reverb on the kick loop. ✅

## Build Notes

- Extend existing `CMakeLists.txt` -- may need to add `juce_audio_formats` module if using JUCE to decode audio (or just decode in JS and pass raw floats)
- The existing JUCE patches for WASM should still apply
- Keep the `SINGLE_FILE=1` Emscripten flag so the WASM binary stays embedded in the JS file

## What This Proves

All four milestones confirm the following work in WASM:
- Loading external audio buffers across the JS/WASM boundary
- Sample-accurate playback and triggering from C++
- BPM-based sequencing inside the audio process loop
- Stereo audio output
- Algorithmic reverb (`juce::Reverb`)
- **FFT-based convolution reverb** using `juce::dsp::FFT` with custom overlap-add implementation

Everything needed for KickWithReverb (filters, distortion, compression, limiter, convolution reverb) uses JUCE DSP classes that work in WASM without threading.

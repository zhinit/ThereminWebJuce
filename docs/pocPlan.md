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

### 2. Loop

**What**: Button toggles a 140 BPM loop that retriggers the kick every quarter note.

**How**:
- In C++ engine: add a sample counter that increments each `process()` call
- Calculate `samplesPerBeat = 60.0 / bpm * sampleRate`
- When counter crosses the beat boundary, call `trigger()` internally
- Add `setPlaying(bool)` and `setBpm(float)` methods exposed via Embind
- React button sends "play"/"stop" message to AudioWorklet

**Done when**: Clicking the button starts a looping kick at 140 BPM. Clicking again stops it.

### 3. Reverb

**What**: Add reverb to the kick sound using an impulse response.

**How**:
- First attempt: `juce::dsp::Convolution`
  - Add one IR WAV file to `frontend/public/`
  - Load IR the same way as the kick sample (decode in JS, copy to WASM heap)
  - In C++ engine: create a `juce::dsp::Convolution` instance, load the IR buffer, process kick output through it, mix wet + dry to final output
- If convolution doesn't work in WASM: fall back to `juce::Reverb` (algorithmic, no IR needed)

**Done when**: Kick loop has audible reverb tail.

## Build Notes

- Extend existing `CMakeLists.txt` -- may need to add `juce_audio_formats` module if using JUCE to decode audio (or just decode in JS and pass raw floats)
- The existing JUCE patches for WASM should still apply
- Keep the `SINGLE_FILE=1` Emscripten flag so the WASM binary stays embedded in the JS file

## What This Proves

If all three milestones work, the following are confirmed for WASM:
- Loading external audio buffers across the JS/WASM boundary
- Sample-accurate playback and triggering from C++
- BPM-based sequencing inside the audio process loop
- Convolution reverb (or that a fallback is needed)

Everything else needed for KickWithReverb (filters, distortion, compression, limiter) uses basic JUCE DSP classes already proven by the POC's `StateVariableTPTFilter`.

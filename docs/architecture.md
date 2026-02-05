# Architecture

This project compiles JUCE's C++ DSP code to WebAssembly and runs it in the browser via an AudioWorklet, with a React frontend for the UI.

## High-Level Data Flow

```
React UI (App.tsx)
    |
    | 1. fetch ir.wav, decode to Float32Array, interleave stereo channels
    | 2. postMessage("loadIR", irSamples) → copy to WASM heap
    | 3. fetch kick.wav, decode to Float32Array
    | 4. postMessage("loadSample", samples) → copy to WASM heap
    | 5. postMessage("loop", true/false) → start/stop looping
    | 6. postMessage("distortionAmount", drive) → set waveshaper drive
    | 7. postMessage("ottAmount", amount) → set OTT compression amount
    | 8. postMessage("reverbMix", wet, dry) → set reverb dry/wet
    v
AudioWorklet (dsp-processor.js)
    |
    | calls process() every ~2.9ms (128 samples at 44.1kHz)
    v
WASM Module (dsp/*.cpp compiled by Emscripten)
    |
    | 1. Copies samples from loaded buffer to stereo output
    | 2. Auto-retriggers at BPM interval if looping
    | 3. Applies waveshaper distortion (JUCE WaveShaper)
    | 4. Applies OTT multiband compression (custom implementation)
    | 5. Applies FFT-based convolution reverb (custom implementation)
    v
AudioWorklet copies stereo buffers to browser audio output
    |
    v
Speakers (stereo)
```

## The Three Layers

### 1. C++ DSP Layer (`dsp/`)

The audio engine is split across four source files, each with a single responsibility. Header files (`.h`) declare the classes so they can be shared across files; implementation files (`.cpp`) contain the logic.

```
dsp/
  sampler.cpp          — Sampler class (playback, looping, effects chain) + EMSCRIPTEN_BINDINGS
  distortion.h/.cpp    — Distortion class (JUCE WaveShaper wrapper)
  ott.h/.cpp           — BandCompressor + OTTCompressor (multiband compression)
  convolution.h/.cpp   — ConvolutionEngine + StereoConvolutionReverb (FFT convolution)
  oscillator.cpp       — SineOscillator (standalone, not used by sampler)
```

**Classes:**
- `ConvolutionEngine` (`convolution.h`) — Single-channel FFT-based convolution using uniform partitioned overlap-add
- `StereoConvolutionReverb` (`convolution.h`) — Wrapper that runs two `ConvolutionEngine` instances (one per channel) with wet/dry mix
- `BandCompressor` (`ott.h`) — Single-band compressor with envelope follower, upward and downward compression, and ratio interpolation
- `OTTCompressor` (`ott.h`) — 3-band "Over The Top" multiband compressor using Linkwitz-Riley crossovers and three `BandCompressor` instances
- `Distortion` (`distortion.h`) — Wraps a `juce::dsp::WaveShaper` with a drive parameter and asymmetric saturation transfer function
- `Sampler` (`sampler.cpp`) — Top-level orchestrator that handles sample playback, looping, and runs the full effects chain. Owns a `Distortion`, `OTTCompressor`, and `StereoConvolutionReverb` as members.

**How the sampler works:**
- Stores a pointer to sample data (`sampleData_`) and its length (`sampleLength_`), loaded via `loadSample()`
- Maintains a `samplePosition_` that advances through the sample each `process()` call
- `trigger()` resets `samplePosition_` to 0 to restart playback
- When `samplePosition_ >= sampleLength_`, outputs silence (0.0f)
- Loop mode: tracks `loopPosition_` and auto-triggers when it exceeds `samplesPerBeat_`
- Delegates to `Distortion`, `OTTCompressor`, and `StereoConvolutionReverb` in sequence during `process()`

**How the distortion works** (`distortion.cpp`)**:**
- Uses `juce::dsp::WaveShaper<float, std::function<float(float)>>` — a JUCE DSP processor that applies a transfer function sample-by-sample
- Currently uses `tanh(x * drive) + 0.1 * x²` — asymmetric saturation that adds both odd harmonics (from tanh) and even harmonics (from the x² term), giving a warmer tube-like character
- Drive parameter controlled via `setDrive(drive)` — higher values push the signal harder into the nonlinear region, generating more harmonics
- Alternative transfer functions are commented out in the source for easy swapping (tanh, atan, soft/hard clip, wavefold)
- Signal chain position: after kick sample playback, before OTT compressor

**How the OTT compressor works** (`ott.cpp`)**:**
- Splits the stereo signal into 3 frequency bands using `juce::dsp::LinkwitzRileyFilter` (LR4, 24dB/oct) crossovers at 100 Hz and 2500 Hz
- Each band has its own `BandCompressor` with independent settings:

| | Low | Mid | High |
|---|---|---|---|
| Attack | 10ms | 5ms | 1ms |
| Release | 100ms | 75ms | 50ms |
| Downward threshold | -20dB | -20dB | -20dB |
| Downward ratio | 10:1 | 15:1 | 20:1 |
| Upward threshold | -40dB | -40dB | -40dB |
| Upward ratio | 3:1 | 4:1 | 5:1 |

- **Envelope follower**: one-pole filter with separate attack/release coefficients, tracking peak level per sample. Separate envelope state per stereo channel.
- **Gain computation** (in dB): `gainDb = (1 - 1/ratio) * (threshold - envelopeDb)` — the same formula handles both upward (envelope below threshold → positive gain) and downward (envelope above threshold → negative gain) compression
- **Amount control**: interpolates each ratio toward 1:1 — `effectiveRatio = 1.0 + amount * (targetRatio - 1.0)`. At amount=0, all ratios are 1:1 (no compression, no gain change). At amount=1, ratios hit their full values.
- **Makeup gain**: 18dB of makeup gain scaled by amount to compensate for level reduction from heavy downward compression. `makeupGain = 10^(amount * 18 / 20)`
- **Band splitting**: cascade approach — LR lowpass/highpass at 100 Hz splits into low and mid+high, then LR lowpass/highpass at 2500 Hz splits mid+high into mid and high. Linkwitz-Riley filters provide perfect reconstruction when bands are summed.
- Signal chain position: after waveshaper, before convolution reverb

**How convolution reverb works** (`convolution.cpp`)**:**
- IR loaded via `loadImpulseResponse(ptr, length, numChannels)` — partitions IR into 384-sample segments, FFTs each
- Uses `juce::dsp::FFT` with order 9 (512 samples) for frequency-domain processing
- Each `process()` call: FFT input → complex multiply-accumulate with all IR segments → inverse FFT → overlap-add
- Ring buffer of FFT'd input segments allows efficient convolution with long IRs
- Stereo IR: left channel IR convolves with left input, right with right
- Wet/dry mix controlled via `setReverbMix(wetLevel, dryLevel)`

**Stereo output:**
The `Sampler::process()` method takes two buffer pointers (left and right channels). The mono sample is written to both channels, then the signal passes through the effects chain: `distortion_.process()` → `ottCompressor_.process()` → `convolutionReverb_.process()`.

**How it's exposed to JavaScript:**
The class is exposed via Emscripten's `embind` system (`EMSCRIPTEN_BINDINGS` macro). This generates JavaScript bindings so the AudioWorklet can call C++ methods like `engine.loadSample(ptr, len)`, `engine.loadImpulseResponse(ptr, len, channels)`, `engine.trigger()`, `engine.process(leftPtr, rightPtr, 128)`, `engine.setWaveshaperDrive(drive)`, `engine.setOTTAmount(amount)`, or `engine.setReverbMix(wet, dry)` directly.

### 2. AudioWorklet Layer (`frontend/public/dsp-processor.js`)

The AudioWorklet is a browser API for real-time audio processing. It runs on a dedicated audio thread, separate from the main UI thread.

**Initialization flow:**
1. Main thread fetches `audio-engine.js` (the Emscripten glue code) as text
2. Main thread sends the script text to the worklet via `postMessage`
3. Worklet evaluates the script using `new Function()`, calls `createAudioEngine()` to instantiate the WASM module
4. Worklet creates a `Sampler` instance and calls `prepare()`
5. Worklet sends `"ready"` message back to main thread

**IR loading flow:**
1. Main thread fetches and decodes `ir.wav` into an `AudioBuffer`
2. Main thread interleaves stereo channels into a single `Float32Array`
3. Main thread sends `{ type: "loadIR", irSamples, irLength, numChannels }` to worklet
4. Worklet allocates WASM heap memory via `module._malloc(irSamples.length * 4)`
5. Worklet copies samples into heap via `HEAPF32.set(irSamples, ptr / 4)`
6. Worklet calls `engine.loadImpulseResponse(ptr, irLength, numChannels)`

**Sample loading flow:**
1. Main thread fetches and decodes `kick.wav` into a `Float32Array`
2. Main thread sends `{ type: "loadSample", samples }` to worklet
3. Worklet allocates WASM heap memory via `module._malloc(samples.length * 4)`
4. Worklet copies samples into heap via `HEAPF32.set(samples, ptr / 4)`
5. Worklet calls `engine.loadSample(ptr, samples.length)`

**Audio processing flow (called ~344 times/second at 44.1kHz):**
1. Browser calls `process(inputs, outputs)` with stereo 128-sample output buffers
2. Worklet allocates two WASM heap buffers (left/right) via `module._malloc()` (reuses if already allocated)
3. Worklet calls `engine.process(leftPtr, rightPtr, 128)` — C++ writes stereo samples into WASM heap
4. Worklet creates `Float32Array` views into the WASM heap for each channel
5. Worklet copies the samples into the browser's stereo output buffers

**Why the memory dance?**
JavaScript's `Float32Array` output buffer lives in JS memory. C++ writes into WASM linear memory (a separate `ArrayBuffer`). You can't pass the JS buffer directly to WASM — you have to allocate space in the WASM heap, let C++ write there, then copy back to JS.

### 3. React UI Layer (`frontend/src/App.tsx`)

A minimal React app with two buttons (Cue and Play/Pause) and three parameter sliders. It:
1. Creates an `AudioContext` on first click (browsers require user gesture)
2. Loads the AudioWorklet processor module
3. Creates an `AudioWorkletNode` with stereo output (`outputChannelCount: [2]`) and connects it to `ctx.destination`
4. Waits for `"ready"` message, then:
   - Fetches `ir.wav`, decodes it, interleaves stereo channels, and sends to worklet for convolution reverb
   - Fetches `kick.wav`, decodes it with `decodeAudioData()`, and sends the samples to the worklet
5. "Cue" button sends `play` message for single trigger; "Play/Pause" toggles `loop` message for 140 BPM looping
6. Three range sliders control the effects chain in real time:
   - **Distortion** (0–1): maps to waveshaper drive 1–20 via `drive = 1.0 + amount * 19.0`. At 0, the waveshaper is nearly linear.
   - **OTT Amount** (0–1): scales compression ratios from 1:1 (transparent) to full OTT values
   - **Reverb** (0–1): dry/wet mix. 0 = fully dry, 1 = fully wet (defaults to 0.3)

## Build System

### CMake + CPM + Emscripten

The build uses three tools together:

**CPM (CMake Package Manager)** fetches JUCE from GitHub at configure time. It's a single-file CMake script that wraps `FetchContent`. Pinned to JUCE 8.0.12 for reproducibility.

**Emscripten** is a C++ to WebAssembly compiler. The `emcmake` wrapper sets CMake's toolchain file so that `em++` is used instead of `clang++`/`g++`. The output is a `.js` file (Emscripten glue code) with WASM embedded inline (`SINGLE_FILE=1`).

**CMake** ties it together. The target is a plain `add_executable` compiling four source files (`dsp/sampler.cpp`, `dsp/convolution.cpp`, `dsp/ott.cpp`, `dsp/distortion.cpp`) and linking headless JUCE modules: `juce_core`, `juce_audio_basics`, and `juce_dsp`. No GUI, no audio device I/O.

### Key Emscripten Flags

| Flag | Purpose |
|------|---------|
| `--bind` | Enables Embind so C++ classes can be called from JS |
| `MODULARIZE=1` | Wraps output in a factory function instead of executing immediately |
| `EXPORT_NAME=createAudioEngine` | Names the factory function |
| `ENVIRONMENT=web,worker,shell` | Declares valid runtime environments. `shell` is needed because AudioWorklets are detected as shell context by Emscripten |
| `SINGLE_FILE=1` | Embeds the `.wasm` binary as base64 inside the `.js` file. Avoids CORS issues with separate `.wasm` fetch |
| `EXPORTED_FUNCTIONS` | Exposes `_malloc` and `_free` so JS can allocate/free WASM heap memory |
| `EXPORTED_RUNTIME_METHODS` | Exposes `HEAPF32` so JS can create Float32Array views into WASM memory |

### Why `SHELL:` prefix?

CMake's `target_link_options` splits arguments on spaces. Without `SHELL:`, the flag `-s MODULARIZE=1` gets split into `-s` and `MODULARIZE=1` as separate arguments, and Emscripten interprets `MODULARIZE=1` as a filename. The `SHELL:` prefix tells CMake to pass the string as-is to the linker.

### Compile Definitions

| Definition | Purpose |
|------------|---------|
| `JUCE_USE_CURL=0` | Disables libcurl networking (not available in WASM) |
| `JUCE_WEB_BROWSER=0` | Disables embedded browser component (not relevant) |

## JUCE Patches

JUCE 8.0.12 has two bugs when compiling for Emscripten/WASM. These are patched at CMake configure time using `file(READ)` / `string(REPLACE)` / `file(WRITE)` inside an `if(EMSCRIPTEN)` block.

### Patch 1: Thread Priorities Table

**File:** `juce_core/native/juce_ThreadPriorities_native.h`

**Problem:** JUCE defines a static lookup table mapping `Thread::Priority` enums to native OS thread priority values. The table has `#if` branches for Linux, BSD, Mac, and Windows — but not WASM. When compiling for Emscripten, no branch matches, so the table is zero-length. This breaks `static_assert(std::size(table) == 5)` and all calls to `std::begin()`/`std::end()` on the table.

**Fix:** Add `|| JUCE_WASM` to the `JUCE_LINUX || JUCE_BSD` branch. WASM gets the same all-zeros entries as Linux (thread priorities are meaningless in a browser anyway).

### Patch 2: Missing Emscripten Include

**File:** `juce_core/native/juce_SystemStats_wasm.cpp`

**Problem:** This file calls `emscripten_get_now()` (for high-resolution timing) but doesn't include `<emscripten.h>` where the function is declared.

**Fix:** Prepend `#include <emscripten.h>` to the file.

### Why not fake `JUCE_LINUX=1`?

We tried this first. It fixes the thread priorities issue but pulls in `juce_BasicNativeHeaders.h`'s Linux block, which includes `<sys/prctl.h>`, `<sys/sysinfo.h>`, `<sys/timerfd.h>`, and other Linux-only headers that don't exist in Emscripten's sysroot.

### Why not use `PATCH_COMMAND`?

CPM supports `PATCH_COMMAND` in `CPMAddPackage`, but:
- Inline shell commands with `&&` break CMake's Makefile generation ("missing separator" errors)
- Shell script files via `PATCH_COMMAND` also hit escaping issues
- The `file(READ)`/`file(WRITE)` approach runs purely in CMake with no shell involvement

## Vite Dev Server

The frontend uses Vite with two special response headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

These enable `SharedArrayBuffer`, which is required for WASM threading support (even though this project doesn't currently use threads). Some Emscripten configurations expect it.

## Build & Run Commands

```bash
# One-time: build WASM
emcmake cmake -B build    # configure with Emscripten toolchain
cmake --build build        # compile C++ to WASM, output to frontend/public/

# Run frontend
cd frontend
npm install
npm run dev
```

The WASM build outputs `frontend/public/audio-engine.js`, which Vite serves as a static file.

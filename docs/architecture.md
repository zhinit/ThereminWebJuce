# Architecture

This project compiles JUCE's C++ DSP code to WebAssembly and runs it in the browser via an AudioWorklet, with a React frontend for the UI.

## High-Level Data Flow

```
React UI (App.tsx)
    |
    | postMessage("play"/"stop")
    v
AudioWorklet (dsp-processor.js)
    |
    | calls process() every ~2.9ms (128 samples at 44.1kHz)
    v
WASM Module (oscillator.cpp compiled by Emscripten)
    |
    | Manual sine wave with phase accumulation fills a float buffer
    v
AudioWorklet copies buffer to browser audio output
    |
    v
Speakers
```

## The Three Layers

### 1. C++ DSP Layer (`dsp/oscillator.cpp`)

This is the audio engine. It generates audio samples using manual phase-accumulation sine wave synthesis (no JUCE DSP module).

**How the oscillator works:**
- Maintains a `phase_` accumulator that increments by `2π * frequency / sampleRate` each sample
- Outputs `std::sin(phase_)` per sample
- Wraps phase at `2π` to prevent numerical overflow
- This approach was ported from the Theremin VST's `SineWave` class after `juce::dsp::Oscillator` produced distorted output in the WASM environment (likely due to `AudioBlock` mishandling raw pointer memory layout under Emscripten)

**How it's exposed to JavaScript:**
The class is exposed via Emscripten's `embind` system (`EMSCRIPTEN_BINDINGS` macro). This generates JavaScript bindings so the AudioWorklet can call C++ methods like `engine.setFreq(440)` or `engine.process(ptr, 128)` directly.

The `process()` method takes a `uintptr_t` (a raw memory address in the WASM heap) and a sample count. It writes float samples directly into WASM heap memory at that address.

### 2. AudioWorklet Layer (`frontend/public/dsp-processor.js`)

The AudioWorklet is a browser API for real-time audio processing. It runs on a dedicated audio thread, separate from the main UI thread.

**Initialization flow:**
1. Main thread fetches `audio-engine.js` (the Emscripten glue code) as text
2. Main thread sends the script text to the worklet via `postMessage`
3. Worklet evaluates the script using `new Function()`, calls `createAudioEngine()` to instantiate the WASM module
4. Worklet creates a `SineOscillator` instance and calls `setSampleRate()`

**Audio processing flow (called ~344 times/second at 44.1kHz):**
1. Browser calls `process(inputs, outputs)` with a 128-sample output buffer
2. Worklet allocates WASM heap memory via `module._malloc()` (reuses if already allocated)
3. Worklet calls `engine.process(heapPtr, 128)` — C++ writes samples into WASM heap
4. Worklet creates a `Float32Array` view into the WASM heap at that address
5. Worklet copies the samples into the browser's output buffer via `output.set(wasmOutput)`

**Why the memory dance?**
JavaScript's `Float32Array` output buffer lives in JS memory. C++ writes into WASM linear memory (a separate `ArrayBuffer`). You can't pass the JS buffer directly to WASM — you have to allocate space in the WASM heap, let C++ write there, then copy back to JS.

### 3. React UI Layer (`frontend/src/App.tsx`)

A minimal React app with a single play/stop button. It:
1. Creates an `AudioContext` on first click (browsers require user gesture)
2. Loads the AudioWorklet processor module
3. Creates an `AudioWorkletNode` and connects it to `ctx.destination` (speakers)
4. Sends `play`/`stop` messages to the worklet via `postMessage`

## Build System

### CMake + CPM + Emscripten

The build uses three tools together:

**CPM (CMake Package Manager)** fetches JUCE from GitHub at configure time. It's a single-file CMake script that wraps `FetchContent`. Pinned to JUCE 8.0.12 for reproducibility.

**Emscripten** is a C++ to WebAssembly compiler. The `emcmake` wrapper sets CMake's toolchain file so that `em++` is used instead of `clang++`/`g++`. The output is a `.js` file (Emscripten glue code) with WASM embedded inline (`SINGLE_FILE=1`).

**CMake** ties it together. The target is a plain `add_executable` (not `juce_add_plugin`) linking only headless JUCE modules: `juce_core` and `juce_audio_basics`. No GUI, no audio device I/O. The `juce_dsp` module was removed after switching to manual sine wave generation.

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

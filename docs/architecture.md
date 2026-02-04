# Architecture

This project compiles JUCE's C++ DSP code to WebAssembly and runs it in the browser via an AudioWorklet, with a React frontend for the UI.

## High-Level Data Flow

```
React UI (App.tsx)
    |
    | 1. fetch kick.wav, decode to Float32Array
    | 2. postMessage("loadSample", samples) → copy to WASM heap
    | 3. postMessage("play") → trigger playback
    v
AudioWorklet (dsp-processor.js)
    |
    | calls process() every ~2.9ms (128 samples at 44.1kHz)
    v
WASM Module (sampler.cpp compiled by Emscripten)
    |
    | Copies samples from loaded buffer to output, advances playback position
    v
AudioWorklet copies buffer to browser audio output
    |
    v
Speakers
```

## The Three Layers

### 1. C++ DSP Layer (`dsp/sampler.cpp`)

This is the audio engine. It plays back a sample that was loaded from JavaScript.

**How the sampler works:**
- Stores a pointer to sample data (`sampleData_`) and its length (`sampleLength_`), loaded via `loadSample()`
- Maintains a `playbackPosition_` that advances through the sample each `process()` call
- `trigger()` resets `playbackPosition_` to 0 to restart playback
- When `playbackPosition_ >= sampleLength_`, outputs silence (0.0f)

**How it's exposed to JavaScript:**
The class is exposed via Emscripten's `embind` system (`EMSCRIPTEN_BINDINGS` macro). This generates JavaScript bindings so the AudioWorklet can call C++ methods like `engine.loadSample(ptr, len)`, `engine.trigger()`, or `engine.process(ptr, 128)` directly.

The `process()` method takes a `uintptr_t` (a raw memory address in the WASM heap) and a sample count. It writes float samples directly into WASM heap memory at that address.

### 2. AudioWorklet Layer (`frontend/public/dsp-processor.js`)

The AudioWorklet is a browser API for real-time audio processing. It runs on a dedicated audio thread, separate from the main UI thread.

**Initialization flow:**
1. Main thread fetches `audio-engine.js` (the Emscripten glue code) as text
2. Main thread sends the script text to the worklet via `postMessage`
3. Worklet evaluates the script using `new Function()`, calls `createAudioEngine()` to instantiate the WASM module
4. Worklet creates a `Sampler` instance and calls `prepare()`
5. Worklet sends `"ready"` message back to main thread

**Sample loading flow:**
1. Main thread fetches and decodes `kick.wav` into a `Float32Array`
2. Main thread sends `{ type: "loadSample", samples }` to worklet
3. Worklet allocates WASM heap memory via `module._malloc(samples.length * 4)`
4. Worklet copies samples into heap via `HEAPF32.set(samples, ptr / 4)`
5. Worklet calls `engine.loadSample(ptr, samples.length)`

**Audio processing flow (called ~344 times/second at 44.1kHz):**
1. Browser calls `process(inputs, outputs)` with a 128-sample output buffer
2. Worklet allocates WASM heap memory via `module._malloc()` (reuses if already allocated)
3. Worklet calls `engine.process(heapPtr, 128)` — C++ writes samples into WASM heap
4. Worklet creates a `Float32Array` view into the WASM heap at that address
5. Worklet copies the samples into the browser's output buffer via `output.set(wasmOutput)`

**Why the memory dance?**
JavaScript's `Float32Array` output buffer lives in JS memory. C++ writes into WASM linear memory (a separate `ArrayBuffer`). You can't pass the JS buffer directly to WASM — you have to allocate space in the WASM heap, let C++ write there, then copy back to JS.

### 3. React UI Layer (`frontend/src/App.tsx`)

A minimal React app with a single button. It:
1. Creates an `AudioContext` on first click (browsers require user gesture)
2. Loads the AudioWorklet processor module
3. Creates an `AudioWorkletNode` and connects it to `ctx.destination` (speakers)
4. Waits for `"ready"` message, then fetches `kick.wav`, decodes it with `decodeAudioData()`, and sends the samples to the worklet
5. Sends `play` messages to trigger the sample on button clicks

## Build System

### CMake + CPM + Emscripten

The build uses three tools together:

**CPM (CMake Package Manager)** fetches JUCE from GitHub at configure time. It's a single-file CMake script that wraps `FetchContent`. Pinned to JUCE 8.0.12 for reproducibility.

**Emscripten** is a C++ to WebAssembly compiler. The `emcmake` wrapper sets CMake's toolchain file so that `em++` is used instead of `clang++`/`g++`. The output is a `.js` file (Emscripten glue code) with WASM embedded inline (`SINGLE_FILE=1`).

**CMake** ties it together. The target is a plain `add_executable` (not `juce_add_plugin`) linking headless JUCE modules: `juce_core`, `juce_audio_basics`, and `juce_dsp`. No GUI, no audio device I/O.

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

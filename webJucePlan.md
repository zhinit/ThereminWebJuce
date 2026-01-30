# JUCE DSP + WebAssembly Proof of Concept Plan

Replace the raw C++ sine oscillator with JUCE's DSP module, compiled to WASM via Emscripten + CMake. The React frontend and AudioWorklet architecture stay the same.

## Project Structure

```
ThereminWebJuce/
├── CMakeLists.txt
├── dsp/
│   └── oscillator.cpp
├── frontend/
│   ├── src/
│   │   ├── App.tsx
│   │   └── main.tsx
│   ├── public/
│   │   └── dsp-processor.js
│   ├── vite.config.ts
│   └── package.json
```

## Step 1: CMake Build Configuration ✅

Use CPM to fetch JUCE (pinned to 8.0.12). Create a plain executable target instead of `juce_add_plugin`, linking only the headless DSP modules. Patch JUCE source at configure time to fix Emscripten compatibility issues.

```cmake
cmake_minimum_required(VERSION 3.24)
project(ThereminWebJuce VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 20)

# CPM package manager
set(CPM_DOWNLOAD_VERSION 0.42.0)
set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
if (NOT EXISTS ${CPM_DOWNLOAD_LOCATION})
    file(DOWNLOAD https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake ${CPM_DOWNLOAD_LOCATION})
endif()
include(${CPM_DOWNLOAD_LOCATION})

CPMAddPackage(
  NAME juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.12
)

# Patch JUCE for Emscripten/WASM compatibility
if(EMSCRIPTEN)
  set(_juce_src "${juce_SOURCE_DIR}/modules/juce_core/native")

  # Fix: thread priorities table has no WASM entries (zero-length array)
  file(READ "${_juce_src}/juce_ThreadPriorities_native.h" _tp_content)
  string(REPLACE "JUCE_LINUX || JUCE_BSD" "JUCE_LINUX || JUCE_BSD || JUCE_WASM" _tp_content "${_tp_content}")
  file(WRITE "${_juce_src}/juce_ThreadPriorities_native.h" "${_tp_content}")

  # Fix: missing emscripten.h include for emscripten_get_now()
  file(READ "${_juce_src}/juce_SystemStats_wasm.cpp" _ss_content)
  string(FIND "${_ss_content}" "#include <emscripten.h>" _found)
  if(_found EQUAL -1)
    string(PREPEND _ss_content "#include <emscripten.h>\n")
    file(WRITE "${_juce_src}/juce_SystemStats_wasm.cpp" "${_ss_content}")
  endif()
endif()

add_executable(audio-engine dsp/oscillator.cpp)

target_compile_definitions(audio-engine PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
)

target_link_libraries(audio-engine PRIVATE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_dsp
)

# Emscripten linker flags (SHELL: prefix prevents CMake from splitting on spaces)
target_link_options(audio-engine PRIVATE
    --bind
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_NAME=createAudioEngine"
    "SHELL:-s ENVIRONMENT=web,worker"
    "SHELL:-s SINGLE_FILE=1"
    "SHELL:-s EXPORTED_FUNCTIONS=['_malloc','_free']"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPF32']"
)

# Output into frontend/public/
set_target_properties(audio-engine PROPERTIES
    OUTPUT_NAME "audio-engine"
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/frontend/public"
)
```

Build with:

```bash
emcmake cmake -B build
cmake --build build
```

`emcmake` wraps CMake with Emscripten's toolchain so `em++` is used automatically.

## Step 2: Validate JUCE Compiles to WASM ✅

Before writing real DSP code, verify that JUCE's modules compile under Emscripten. Create a minimal `oscillator.cpp` that just includes the header:

```cpp
#include <juce_dsp/juce_dsp.h>
#include <emscripten/bind.h>

int main() { return 0; }
```

Run the build. Two JUCE bugs surfaced and are fixed by the CMake patches above:

1. **`juce_ThreadPriorities_native.h`** — The thread priority lookup table has `#if` branches for Linux, BSD, Mac, Windows but not WASM, resulting in a zero-length array that fails `static_assert` and `std::size`/`std::begin`/`std::end`. Fixed by adding `|| JUCE_WASM` to the Linux/BSD branch (all-zeros, same as Linux).

2. **`juce_SystemStats_wasm.cpp`** — Uses `emscripten_get_now()` without including `<emscripten.h>`. Fixed by prepending the include.

Additional lessons learned:
- **Pin JUCE to a release tag** (8.0.12) instead of `origin/master` for reproducible builds.
- **Don't fake `JUCE_LINUX=1`** — it pulls in Linux-only headers like `sys/prctl.h` that Emscripten doesn't provide.
- **Use `SHELL:` prefix** for Emscripten `-s` linker flags to prevent CMake from splitting `-s KEY=VALUE` into separate arguments.
- **Use CMake `file(READ)`/`file(WRITE)`** for patching instead of `PATCH_COMMAND` with shell scripts, which has escaping issues.

## Step 3: Write the JUCE Oscillator ✅

Replace raw `std::sin` phase accumulation with `juce::dsp::Oscillator`. Keep the Embind interface identical so the frontend doesn't change.

```cpp
#include <juce_dsp/juce_dsp.h>
#include <emscripten/bind.h>

class SineOscillator {
public:
    SineOscillator() {
        oscillator.initialise([](float x) { return std::sin(x); });
        oscillator.setFrequency(440.0f);
    }

    void setPlaying(bool playing) { playing_ = playing; }
    void setFreq(float freq) { oscillator.setFrequency(freq); }

    void setSampleRate(float sampleRate) {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = 512;
        spec.numChannels = 1;
        oscillator.prepare(spec);
    }

    void process(uintptr_t outputPtr, int numSamples) {
        float* output = reinterpret_cast<float*>(outputPtr);

        if (!playing_) {
            std::fill(output, output + numSamples, 0.0f);
            return;
        }

        juce::dsp::AudioBlock<float> block(&output, 1, numSamples);
        juce::dsp::ProcessContextReplacing<float> context(block);
        oscillator.process(context);
    }

private:
    juce::dsp::Oscillator<float> oscillator;
    bool playing_ = false;
};

EMSCRIPTEN_BINDINGS(audio_module) {
    emscripten::class_<SineOscillator>("SineOscillator")
        .constructor()
        .function("setPlaying", &SineOscillator::setPlaying)
        .function("setFreq", &SineOscillator::setFreq)
        .function("setSampleRate", &SineOscillator::setSampleRate)
        .function("process", &SineOscillator::process);
}
```

Key JUCE concepts used:
- `juce::dsp::Oscillator<float>` — handles phase accumulation internally
- `juce::dsp::ProcessSpec` — configures sample rate and buffer size
- `juce::dsp::AudioBlock` / `ProcessContextReplacing` — wraps raw float buffers for JUCE's processing pipeline

## Step 4: Frontend (Copy from TherminWeb) ✅

Copied these files from the TherminWeb project with no modifications:

- `frontend/src/App.tsx` — React UI with play/stop button
- `frontend/src/main.tsx` — React entry point
- `frontend/public/dsp-processor.js` — AudioWorklet processor
- `frontend/vite.config.ts` — Vite config with COEP/COOP headers
- `frontend/package.json` — React + Vite dependencies

No frontend changes needed because the Embind API (class name, method names, signatures) is identical.

## Step 5: Run ✅

```bash
# Build WASM
emcmake cmake -B build
cmake --build build

# Start frontend
cd frontend
npm install
npm run dev
```

Open browser, click play — sound is produced from JUCE DSP compiled to WASM.

## Known Issues

- **Audio output sounds distorted** — the oscillator produces sound but it doesn't sound like a clean sine wave. Likely a bug in how the JUCE oscillator interacts with the AudioWorklet buffer (possibly related to `prepare()` being called before `setFrequency()`, or `AudioBlock` wrapping). Needs debugging in a follow-up session.
- **ENVIRONMENT flag** — AudioWorklets run in a context that Emscripten detects as "shell", so `shell` must be included in `-s ENVIRONMENT=web,worker,shell`.

## Risks (Resolved)

**JUCE + Emscripten compatibility** was the main risk. Two bugs were found and patched at CMake configure time:
1. Zero-length thread priority table (no `JUCE_WASM` branch)
2. Missing `<emscripten.h>` include in the WASM system stats file

These are bugs in JUCE 8.0.12's Emscripten support. The patches are minimal and applied automatically via CMake `file(READ)`/`file(WRITE)` in an `if(EMSCRIPTEN)` block.

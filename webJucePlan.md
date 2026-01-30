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

## Step 1: CMake Build Configuration

Use CPM to fetch JUCE (same pattern as the Theremin plugin project). Create a plain executable target instead of `juce_add_plugin`, linking only the headless DSP modules.

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
    GIT_TAG origin/master
)

add_executable(audio-engine dsp/oscillator.cpp)

target_link_libraries(audio-engine PRIVATE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_dsp
)

# Emscripten linker flags
target_link_options(audio-engine PRIVATE
    --bind
    -s MODULARIZE=1
    -s EXPORT_NAME="createAudioEngine"
    -s ENVIRONMENT='web,worker'
    -s SINGLE_FILE=1
    -s EXPORTED_FUNCTIONS=["_malloc","_free"]
    -s EXPORTED_RUNTIME_METHODS=["ccall","cwrap","HEAPF32"]
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

## Step 2: Validate JUCE Compiles to WASM

Before writing real DSP code, verify that JUCE's modules compile under Emscripten. Create a minimal `oscillator.cpp` that just includes the header:

```cpp
#include <juce_dsp/juce_dsp.h>
#include <emscripten/bind.h>

int main() { return 0; }
```

Run the build. If this fails, you'll need to stub out platform-specific code or add compile definitions to disable unsupported JUCE features. This is the highest-risk step.

## Step 3: Write the JUCE Oscillator

Replace raw `std::sin` phase accumulation with `juce::dsp::Oscillator`. Keep the Embind interface identical so the frontend doesn't change.

```cpp
#include <juce_dsp/juce_dsp.h>
#include <emscripten/bind.h>

class SineOscillator {
public:
    SineOscillator() {
        oscillator.initialise([](float x) { return std::sin(x); });
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

## Step 4: Frontend (Copy from TherminWeb)

Copy these files from the current TherminWeb project with no modifications:

- `frontend/src/App.tsx` — React UI with play/stop button
- `frontend/src/main.tsx` — React entry point
- `frontend/public/dsp-processor.js` — AudioWorklet processor
- `frontend/vite.config.ts` — Vite config with COEP/COOP headers
- `frontend/package.json` — React + Vite dependencies

No frontend changes needed because the Embind API (class name, method names, signatures) is identical.

## Step 5: Run

```bash
# Build WASM
emcmake cmake -B build
cmake --build build

# Start frontend
cd frontend
npm install
npm run dev
```

Open browser, click play, hear 440Hz sine wave — now powered by JUCE DSP.

## Risks

**JUCE + Emscripten compatibility** is the main risk. JUCE wasn't designed for WASM. The core and DSP modules are mostly portable math/data code and should compile, but platform-specific code paths may need `#ifdef` workarounds or compile definition overrides. Step 2 exists specifically to surface these issues early.

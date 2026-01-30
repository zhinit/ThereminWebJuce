#!/bin/bash
sed -i.bak 's/JUCE_LINUX || JUCE_BSD/JUCE_LINUX || JUCE_BSD || JUCE_WASM/' modules/juce_core/native/juce_ThreadPriorities_native.h
sed -i.bak '1s/^/#include <emscripten.h>\n/' modules/juce_core/native/juce_SystemStats_wasm.cpp

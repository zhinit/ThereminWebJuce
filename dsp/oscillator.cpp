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
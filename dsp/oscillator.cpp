#include <cmath>
#include <numbers>
#include <juce_dsp/juce_dsp.h>
#include <emscripten/bind.h>

class SineOscillator {
public:
    SineOscillator() = default;

    void setPlaying(bool playing) { playing_ = playing; }
    void setFreq(float freq) { frequency_ = freq; }

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate_;
        spec.maximumBlockSize = 128;
        spec.numChannels=1;
        filter_.prepare(spec);
        filter_.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
        filter_.setCutoffFrequency(150.0f);
    }

    void process(uintptr_t outputPtr, int numSamples) {
        float* output = reinterpret_cast<float*>(outputPtr);

        if (!playing_) {
            std::fill(output, output + numSamples, 0.0f);
            return;
        }

        for (int i = 0; i < numSamples; ++i) {
            const float phaseInc = doublePi * frequency_ / sampleRate_;
            output[i] = filter_.processSample(0, phase_ / pi - 1.0f);
            phase_ += phaseInc;

            if (phase_ >= doublePi)
                phase_ -= doublePi;
        }
    }

private:
    static constexpr float doublePi = 2.0f * std::numbers::pi_v<float>;
    static constexpr float pi = std::numbers::pi_v<float>;
    juce::dsp::StateVariableTPTFilter<float> filter_;
    float phase_ = 0.0f;
    float sampleRate_ = 44100.0f;
    float frequency_ = 110.0f;
    bool playing_ = false;
};

EMSCRIPTEN_BINDINGS(audio_module) {
    emscripten::class_<SineOscillator>("SineOscillator")
        .constructor()
        .function("prepare", &SineOscillator::prepare)
        .function("setPlaying", &SineOscillator::setPlaying)
        .function("setFreq", &SineOscillator::setFreq)
        .function("process", &SineOscillator::process);
}

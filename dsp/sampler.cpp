#include <juce_dsp/juce_dsp.h>
#include <emscripten/bind.h>

class Sampler {
public:
    Sampler() = default;

    // void setPlaying(bool playing) { playing_ = playing; }

    void loadSample(uintptr_t samplePtr, size_t sampleLength) {
        sampleData_ = reinterpret_cast<float*>(samplePtr);
        sampleLength_ = sampleLength;
    }

    void prepare(float sampleRate) {
        sampleRate_ = sampleRate;
        playbackPosition_ = 0;
        // sampleIsPlaying_ = false;
    }

    void trigger() {
        playbackPosition_ = 0;
    }

    void process(uintptr_t outputPtr, int numSamples) {
        float* output = reinterpret_cast<float*>(outputPtr);

        if (!playing_) {
            std::fill(output, output + numSamples, 0.0f);
            return;
        }

        for (int i = 0; i < numSamples; ++i) {
            if (playbackPosition_ < sampleLength_) {
                output[i] = sampleData_[playbackPosition_];
                ++playbackPosition_;
            } else {
                output[i] = 0.0f;
            }
        }
    }

private:
    float sampleRate_ = 44100.0f;
    // bool playing_ = false;

    float* sampleData_;
    size_t sampleLength_;
    size_t playbackPosition_;
};

EMSCRIPTEN_BINDINGS(audio_module) {
    emscripten::class_<Sampler>("Sampler")
        .constructor()
        .function("setPlaying", &Sampler::setPlaying)
        .function("loadSample", &Sampler::loadSample)
        .function("trigger", &Sampler::trigger)
        .function("prepare", &Sampler::prepare)
        .function("process", &Sampler::process);
}

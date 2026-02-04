#include <emscripten/bind.h>
#include <juce_dsp/juce_dsp.h>

class Sampler
{
public:
  Sampler() = default;

  void setLooping(bool inLoop)
  {
    inLoop_ = inLoop;
    if (inLoop_) {
      trigger();
      loopPosition_ = 0;
    }
  }

  void loadSample(uintptr_t samplePtr, size_t sampleLength)
  {
    sampleData_ = reinterpret_cast<float*>(samplePtr);
    sampleLength_ = sampleLength;
    samplePosition_ = sampleLength_;
  }

  void prepare(float sampleRate)
  {
    sampleRate_ = sampleRate;
    samplePosition_ = 0;
    samplesPerBeat_ = sampleRate_ / bpm_ * 60;

    // prepare the reverb
    reverb_.setSampleRate(static_cast<double>(sampleRate_));

    juce::Reverb::Parameters params;
    params.roomSize = 1.0f;
    params.damping = 0.5f;
    params.wetLevel = 0.3f;
    params.dryLevel = 0.7f;
    params.width = 1.0f;
    params.freezeMode = 0.0f;
    reverb_.setParameters(params);
  }

  void trigger() { samplePosition_ = 0; }

  void process(uintptr_t outputPtr, int numSamples)
  {
    float* output = reinterpret_cast<float*>(outputPtr);

    for (int i = 0; i < numSamples; ++i) {
      if (inLoop_) {
        if (loopPosition_ > samplesPerBeat_) {
          loopPosition_ = 0;
          trigger();
        } else {
          ++loopPosition_;
        }
      }
      if (samplePosition_ < sampleLength_) {
        output[i] = sampleData_[samplePosition_];
        ++samplePosition_;
      } else {
        output[i] = 0.0f;
      }
    }

    reverb_.processMono(output, numSamples);
  }

private:
  // general vars
  float sampleRate_ = 44100.0f;

  // sample vars
  float* sampleData_ = nullptr;
  size_t sampleLength_ = 0;
  size_t samplePosition_ = 0;

  // loop vars
  float bpm_ = 140;
  size_t samplesPerBeat_ = 0;
  size_t loopPosition_ = 0;
  bool inLoop_ = false;

  // reverb
  juce::Reverb reverb_;
};

EMSCRIPTEN_BINDINGS(audio_module)
{
  emscripten::class_<Sampler>("Sampler")
    .constructor()
    .function("loadSample", &Sampler::loadSample)
    .function("trigger", &Sampler::trigger)
    .function("prepare", &Sampler::prepare)
    .function("process", &Sampler::process)
    .function("setLooping", &Sampler::setLooping);
}

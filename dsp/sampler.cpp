#include "convolution.h"
#include "distortion.h"
#include "ott.h"

#include <emscripten/bind.h>

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
  }

  void loadImpulseResponse(uintptr_t irPtr, size_t irLength, int numChannels)
  {
    const float* irData = reinterpret_cast<const float*>(irPtr);
    convolutionReverb_.loadIR(irData, irLength, numChannels);
  }

  void prepare(float sampleRate)
  {
    sampleRate_ = sampleRate;
    samplePosition_ = 0;
    samplesPerBeat_ = sampleRate_ / bpm_ * 60;

    convolutionReverb_.prepare(sampleRate);
    ottCompressor_.prepare(sampleRate);
    distortion_.prepare(sampleRate);
  }

  void trigger() { samplePosition_ = 0; }

  void process(uintptr_t leftPtr, uintptr_t rightPtr, int numSamples)
  {
    float* left = reinterpret_cast<float*>(leftPtr);
    float* right = reinterpret_cast<float*>(rightPtr);

    for (int i = 0; i < numSamples; ++i) {
      if (inLoop_) {
        if (loopPosition_ > samplesPerBeat_) {
          loopPosition_ = 0;
          trigger();
        } else {
          ++loopPosition_;
        }
      }

      float sample = 0.0f;
      if (samplePosition_ < sampleLength_) {
        sample = sampleData_[samplePosition_];
        ++samplePosition_;
      }
      left[i] = sample;
      right[i] = sample;
    }

    distortion_.process(left, right, numSamples);
    ottCompressor_.process(left, right, numSamples);
    convolutionReverb_.process(left, right, numSamples);
  }

  void setReverbMix(float wetLevel, float dryLevel)
  {
    convolutionReverb_.setMix(wetLevel, dryLevel);
  }

  void setWaveshaperDrive(float drive) { distortion_.setDrive(drive); }

  void setOTTAmount(float amount) { ottCompressor_.setAmount(amount); }

private:
  float sampleRate_ = 44100.0f;

  float* sampleData_ = nullptr;
  size_t sampleLength_ = 0;
  size_t samplePosition_ = 0;

  float bpm_ = 140;
  size_t samplesPerBeat_ = 0;
  size_t loopPosition_ = 0;
  bool inLoop_ = false;

  Distortion distortion_;
  OTTCompressor ottCompressor_;
  StereoConvolutionReverb convolutionReverb_;
};

EMSCRIPTEN_BINDINGS(audio_module)
{
  emscripten::class_<Sampler>("Sampler")
    .constructor()
    .function("loadSample", &Sampler::loadSample)
    .function("loadImpulseResponse", &Sampler::loadImpulseResponse)
    .function("trigger", &Sampler::trigger)
    .function("prepare", &Sampler::prepare)
    .function("process", &Sampler::process)
    .function("setLooping", &Sampler::setLooping)
    .function("setReverbMix", &Sampler::setReverbMix)
    .function("setWaveshaperDrive", &Sampler::setWaveshaperDrive)
    .function("setOTTAmount", &Sampler::setOTTAmount);
}

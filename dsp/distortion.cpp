#include "distortion.h"

void Distortion::prepare(float sampleRate)
{
  waveshaper_.functionToUse = [this](float x) {
    // Symmetric — odd harmonics only:
    // return std::tanh(x * drive_);                        // soft clip
    // return std::atan(x * drive_);                        // similar, different rolloff
    // return x * drive_ / (1.0f + std::abs(x * drive_));  // softer saturation
    // return std::clamp(x * drive_, -1.0f, 1.0f);         // hard clip
    // Asymmetric — adds even harmonics (warmer, tube-like):
    return std::tanh(x * drive_) + 0.1f * x * x;
    // Wavefold — complex metallic harmonics:
    // return std::sin(x * drive_);
  };
  juce::dsp::ProcessSpec spec{ sampleRate, 128u, 2u };
  waveshaper_.prepare(spec);
}

void Distortion::process(float* left, float* right, int numSamples)
{
  float* channels[] = { left, right };
  juce::dsp::AudioBlock<float> block(
    channels, 2, static_cast<size_t>(numSamples));
  juce::dsp::ProcessContextReplacing<float> context(block);
  waveshaper_.process(context);
}

void Distortion::setDrive(float drive) { drive_ = drive; }

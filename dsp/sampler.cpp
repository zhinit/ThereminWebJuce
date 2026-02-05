#include <algorithm>
#include <cmath>
#include <cstring>
#include <emscripten/bind.h>
#include <functional>
#include <juce_dsp/juce_dsp.h>
#include <vector>

// FFT-based convolution engine using uniform partitioned overlap-add
class ConvolutionEngine
{
public:
  ConvolutionEngine() = default;

  void prepare(float sampleRate)
  {
    sampleRate_ = sampleRate;
    reset();
  }

  void loadIR(const float* irData, size_t irLength)
  {
    if (irLength == 0 || irData == nullptr)
      return;

    // Calculate number of segments needed
    numSegments_ = (irLength + segmentSize_ - 1) / segmentSize_;
    numInputSegments_ = numSegments_ * 3; // Ring buffer size

    // Allocate IR segment buffers (pre-FFT'd)
    irSegmentsFFT_.resize(numSegments_);
    for (auto& segment : irSegmentsFFT_) {
      segment.resize(fftSize_ * 2, 0.0f);
    }

    // Allocate input segment ring buffer
    inputSegmentsFFT_.resize(numInputSegments_);
    for (auto& segment : inputSegmentsFFT_) {
      segment.resize(fftSize_ * 2, 0.0f);
    }

    // Allocate working buffers
    inputBuffer_.resize(fftSize_, 0.0f);
    outputBuffer_.resize(fftSize_ * 2, 0.0f);
    overlapBuffer_.resize(fftSize_, 0.0f);
    tempBuffer_.resize(fftSize_ * 2, 0.0f);

    // Partition and FFT the impulse response
    for (size_t seg = 0; seg < numSegments_; ++seg) {
      std::fill(irSegmentsFFT_[seg].begin(), irSegmentsFFT_[seg].end(), 0.0f);

      // Copy IR data into segment
      size_t srcOffset = seg * segmentSize_;
      size_t copyLen = std::min(segmentSize_, irLength - srcOffset);

      for (size_t i = 0; i < copyLen; ++i) {
        irSegmentsFFT_[seg][i] = irData[srcOffset + i];
      }

      // Forward FFT
      fft_.performRealOnlyForwardTransform(irSegmentsFFT_[seg].data());

      // Repack for efficient convolution
      prepareForConvolution(irSegmentsFFT_[seg].data());
    }

    irLoaded_ = true;
    reset();
  }

  void process(const float* input, float* output, int numSamples)
  {
    if (!irLoaded_) {
      // Pass through if no IR loaded
      std::copy(input, input + numSamples, output);
      return;
    }

    int numSamplesProcessed = 0;
    size_t indexStep = numInputSegments_ / numSegments_;

    while (numSamplesProcessed < numSamples) {
      bool inputBufferWasEmpty = (inputDataPos_ == 0);
      size_t samplesToProcess =
        std::min(static_cast<size_t>(numSamples - numSamplesProcessed),
                 blockSize_ - inputDataPos_);

      // Copy new input samples into input buffer
      for (size_t i = 0; i < samplesToProcess; ++i) {
        inputBuffer_[inputDataPos_ + i] = input[numSamplesProcessed + i];
      }

      // Get current input segment
      float* inputSegmentData = inputSegmentsFFT_[currentSegment_].data();

      // FFT the current input segment when buffer was empty
      if (inputBufferWasEmpty) {
        std::copy(inputBuffer_.begin(), inputBuffer_.end(), inputSegmentData);
        std::fill(
          inputSegmentData + fftSize_, inputSegmentData + fftSize_ * 2, 0.0f);
        fft_.performRealOnlyForwardTransform(inputSegmentData);
        prepareForConvolution(inputSegmentData);

        // Complex multiplication with all IR segments (except first)
        std::fill(tempBuffer_.begin(), tempBuffer_.end(), 0.0f);

        size_t index = currentSegment_;
        for (size_t seg = 1; seg < numSegments_; ++seg) {
          index += indexStep;
          if (index >= numInputSegments_)
            index -= numInputSegments_;

          convolutionProcessingAndAccumulate(inputSegmentsFFT_[index].data(),
                                             irSegmentsFFT_[seg].data(),
                                             tempBuffer_.data());
        }
      }

      // Copy temp to output and add first segment contribution
      std::copy(tempBuffer_.begin(), tempBuffer_.end(), outputBuffer_.begin());
      convolutionProcessingAndAccumulate(
        inputSegmentData, irSegmentsFFT_[0].data(), outputBuffer_.data());

      // Inverse FFT
      updateSymmetricFrequencyDomainData(outputBuffer_.data());
      fft_.performRealOnlyInverseTransform(outputBuffer_.data());

      // Overlap-add: combine with previous overlap
      for (size_t i = 0; i < samplesToProcess; ++i) {
        output[numSamplesProcessed + i] =
          outputBuffer_[inputDataPos_ + i] + overlapBuffer_[inputDataPos_ + i];
      }

      inputDataPos_ += samplesToProcess;

      // If block complete, prepare for next block
      if (inputDataPos_ == blockSize_) {
        std::fill(inputBuffer_.begin(), inputBuffer_.end(), 0.0f);
        inputDataPos_ = 0;

        // Accumulate overlap for next block
        for (size_t i = blockSize_; i < fftSize_; ++i) {
          outputBuffer_[i] += overlapBuffer_[i];
        }

        // Save overlap for next block
        std::copy(outputBuffer_.begin() + blockSize_,
                  outputBuffer_.begin() + fftSize_,
                  overlapBuffer_.begin());
        std::fill(overlapBuffer_.begin() + (fftSize_ - blockSize_),
                  overlapBuffer_.end(),
                  0.0f);

        // Advance ring buffer index
        currentSegment_ = (currentSegment_ > 0) ? (currentSegment_ - 1)
                                                : (numInputSegments_ - 1);
      }

      numSamplesProcessed += samplesToProcess;
    }
  }

  void reset()
  {
    currentSegment_ = 0;
    inputDataPos_ = 0;

    std::fill(inputBuffer_.begin(), inputBuffer_.end(), 0.0f);
    std::fill(outputBuffer_.begin(), outputBuffer_.end(), 0.0f);
    std::fill(overlapBuffer_.begin(), overlapBuffer_.end(), 0.0f);
    std::fill(tempBuffer_.begin(), tempBuffer_.end(), 0.0f);

    for (auto& segment : inputSegmentsFFT_) {
      std::fill(segment.begin(), segment.end(), 0.0f);
    }
  }

private:
  // Repack FFT data for efficient convolution
  void prepareForConvolution(float* samples)
  {
    size_t halfSize = fftSize_ / 2;

    // Pack real parts into first half
    for (size_t i = 0; i < halfSize; ++i)
      samples[i] = samples[i << 1];

    samples[halfSize] = 0.0f;

    // Pack imaginary parts into second half (negated)
    for (size_t i = 1; i < halfSize; ++i)
      samples[i + halfSize] = -samples[((fftSize_ - i) << 1) + 1];
  }

  // Complex multiply-accumulate
  void convolutionProcessingAndAccumulate(const float* input,
                                          const float* impulse,
                                          float* output)
  {
    size_t halfSize = fftSize_ / 2;

    // Real part: Re(A)*Re(B) - Im(A)*Im(B)
    for (size_t i = 0; i < halfSize; ++i) {
      output[i] += input[i] * impulse[i];
    }
    for (size_t i = 0; i < halfSize; ++i) {
      output[i] -= input[halfSize + i] * impulse[halfSize + i];
    }

    // Imaginary part: Re(A)*Im(B) + Im(A)*Re(B)
    for (size_t i = 0; i < halfSize; ++i) {
      output[halfSize + i] += input[i] * impulse[halfSize + i];
    }
    for (size_t i = 0; i < halfSize; ++i) {
      output[halfSize + i] += input[halfSize + i] * impulse[i];
    }

    output[fftSize_] += input[fftSize_] * impulse[fftSize_];
  }

  // Restore symmetric FFT data for inverse transform
  void updateSymmetricFrequencyDomainData(float* samples)
  {
    size_t halfSize = fftSize_ / 2;

    for (size_t i = 1; i < halfSize; ++i) {
      samples[(fftSize_ - i) << 1] = samples[i];
      samples[((fftSize_ - i) << 1) + 1] = -samples[halfSize + i];
    }

    samples[1] = 0.0f;

    for (size_t i = 1; i < halfSize; ++i) {
      samples[i << 1] = samples[(fftSize_ - i) << 1];
      samples[(i << 1) + 1] = -samples[((fftSize_ - i) << 1) + 1];
    }
  }

  static constexpr int fftOrder_ = 9; // 2^9 = 512
  static constexpr size_t fftSize_ = 512;
  static constexpr size_t blockSize_ = 128;
  static constexpr size_t segmentSize_ = fftSize_ - blockSize_; // 384

  juce::dsp::FFT fft_{ fftOrder_ };

  std::vector<std::vector<float>> irSegmentsFFT_;
  std::vector<std::vector<float>> inputSegmentsFFT_;
  std::vector<float> inputBuffer_;
  std::vector<float> outputBuffer_;
  std::vector<float> overlapBuffer_;
  std::vector<float> tempBuffer_;

  float sampleRate_ = 44100.0f;
  size_t numSegments_ = 0;
  size_t numInputSegments_ = 0;
  size_t currentSegment_ = 0;
  size_t inputDataPos_ = 0;
  bool irLoaded_ = false;
};

// Stereo convolution reverb wrapper
class StereoConvolutionReverb
{
public:
  void prepare(float sampleRate)
  {
    leftEngine_.prepare(sampleRate);
    rightEngine_.prepare(sampleRate);
    dryBuffer_.resize(128 * 2); // Max block size * 2 channels
  }

  void loadIR(const float* irData, size_t irLengthPerChannel, int numChannels)
  {
    if (numChannels == 1) {
      // Mono IR: use same IR for both channels
      leftEngine_.loadIR(irData, irLengthPerChannel);
      rightEngine_.loadIR(irData, irLengthPerChannel);
    } else {
      // Stereo IR: deinterleave and load each channel
      std::vector<float> leftIR(irLengthPerChannel);
      std::vector<float> rightIR(irLengthPerChannel);

      for (size_t i = 0; i < irLengthPerChannel; ++i) {
        leftIR[i] = irData[i * 2];
        rightIR[i] = irData[i * 2 + 1];
      }

      leftEngine_.loadIR(leftIR.data(), irLengthPerChannel);
      rightEngine_.loadIR(rightIR.data(), irLengthPerChannel);
    }
  }

  void process(float* left, float* right, int numSamples)
  {
    // Store dry signal
    if (dryBuffer_.size() < static_cast<size_t>(numSamples * 2))
      dryBuffer_.resize(numSamples * 2);

    for (int i = 0; i < numSamples; ++i) {
      dryBuffer_[i] = left[i];
      dryBuffer_[numSamples + i] = right[i];
    }

    // Process through convolution (wet signal)
    leftEngine_.process(left, left, numSamples);
    rightEngine_.process(right, right, numSamples);

    // Mix dry and wet
    for (int i = 0; i < numSamples; ++i) {
      left[i] = dryBuffer_[i] * dryLevel_ + left[i] * wetLevel_;
      right[i] = dryBuffer_[numSamples + i] * dryLevel_ + right[i] * wetLevel_;
    }
  }

  void setMix(float wetLevel, float dryLevel)
  {
    wetLevel_ = wetLevel;
    dryLevel_ = dryLevel;
  }

  void reset()
  {
    leftEngine_.reset();
    rightEngine_.reset();
  }

private:
  ConvolutionEngine leftEngine_;
  ConvolutionEngine rightEngine_;
  std::vector<float> dryBuffer_;
  float wetLevel_ = 0.3f;
  float dryLevel_ = 0.7f;
};

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

    // Waveshaper: apply distortion after kick, before reverb
    float* channels[] = { left, right };
    juce::dsp::AudioBlock<float> block(
      channels, 2, static_cast<size_t>(numSamples));
    juce::dsp::ProcessContextReplacing<float> context(block);
    waveshaper_.process(context);

    convolutionReverb_.process(left, right, numSamples);
  }

  void setReverbMix(float wetLevel, float dryLevel)
  {
    convolutionReverb_.setMix(wetLevel, dryLevel);
  }

  void setWaveshaperDrive(float drive) { drive_ = drive; }

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

  // waveshaper
  juce::dsp::WaveShaper<float, std::function<float(float)>> waveshaper_;
  float drive_ = 6.0f;

  // convolution reverb
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
    .function("setWaveshaperDrive", &Sampler::setWaveshaperDrive);
}

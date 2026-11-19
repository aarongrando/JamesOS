// Copyright 2016 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Triangle waveform approximated by 16 discrete steps.

#ifndef PLAITS_DSP_OSCILLATOR_NES_TRIANGLE_OSCILLATOR_H_
#define PLAITS_DSP_OSCILLATOR_NES_TRIANGLE_OSCILLATOR_H_

#include <algorithm>

#include "stmlib/dsp/dsp.h"
#include "plaits/dsp/oscillator/wavetable_oscillator.h"
#include "stmlib/dsp/parameter_interpolator.h"
#include "stmlib/dsp/polyblep.h"

#include "plaits/resources.h"

namespace plaits {

template<int num_bits=5>
class NESTriangleOscillator {
 public:
  NESTriangleOscillator() { }
  ~NESTriangleOscillator() { }
  
  inline void Init() {
    phase_ = 0.0f;
    step_ = 0;
    ascending_ = true;
    next_sample_ = 0.0f;
    frequency_ = 0.001f;
  }
  
  inline void Render(float frequency, float* out, size_t size) {
    frequency = std::min(frequency, 0.25f);

    stmlib::ParameterInterpolator fm(&frequency_, frequency, size);

    float next_sample = next_sample_;
    while (size--) {
      const float f = fm.Next();
      phase_ += f;

      // Outside the crossfade, one waveform has exactly zero gain.
      float this_sample = next_sample;
      if (f <= 0.5f / kNumStepsF) {
        next_sample = NextSample<false>(f, 1.0f, 0.0f, this_sample);
      } else if (f >= 1.0f / kNumStepsF) {
        next_sample = NextSample<true>(f, 0.0f, 2.0f / kScale, this_sample);
      } else {
        const float fade = (f - 0.5f / kNumStepsF) * 2.0f * kNumStepsF;
        next_sample = NextSample<true>(f, 1.0f - fade, fade * 2.0f / kScale, this_sample);
      }
      *out++ = this_sample * kScale - 1.0f;
    }
    next_sample_ = next_sample;
  }

 private:
  static constexpr int kNumSteps = 1 << num_bits;
  static constexpr int kHalf = kNumSteps / 2;
  static constexpr int kTop = kNumSteps != 2 ? kNumSteps - 1 : 2;
  static constexpr float kNumStepsF = static_cast<float>(kNumSteps);
  static constexpr float kScale = kNumSteps != 2
      ? 4.0f / static_cast<float>(kTop - 1)
      : 2.0f;

  template<bool smooth_component>
  __attribute__((always_inline)) inline float NextSample(
      float frequency, float nes_gain, float tri_gain, float& this_sample) {
    float next_sample = 0.0f;
    // Handle the discontinuity at the top of the naive triangle.
    if (ascending_ && phase_ >= 0.5f) {
      float discontinuity = 4.0f * frequency * tri_gain;
      if (discontinuity) {
        float t = (phase_ - 0.5f) / frequency;
        this_sample -= stmlib::ThisIntegratedBlepSample(t) * discontinuity;
        next_sample -= stmlib::NextIntegratedBlepSample(t) * discontinuity;
      }
      ascending_ = false;
    }

    int next_step = static_cast<int>(phase_ * kNumStepsF);
    if (next_step != step_) {
      bool wrap = false;
      if (next_step >= kNumSteps) {
        phase_ -= 1.0f;
        next_step -= kNumSteps;
        wrap = true;
      }

      float discontinuity = next_step < kHalf ? 1.0f : -1.0f;
      if (kNumSteps == 2) {
        discontinuity = -discontinuity;
      } else {
        if (next_step == 0 || next_step == kHalf) {
          discontinuity = 0.0f;
        }
      }

      // Handle the discontinuity at each step of the NES triangle.
      discontinuity *= nes_gain;
      if (discontinuity) {
        float frac = (phase_ * kNumStepsF - static_cast<float>(next_step));
        float t = frac / (frequency * kNumStepsF);
        this_sample += stmlib::ThisBlepSample(t) * discontinuity;
        next_sample += stmlib::NextBlepSample(t) * discontinuity;
      }

      // Handle the discontinuity at the bottom of the naive triangle.
      if (wrap) {
        float discontinuity = 4.0f * frequency * tri_gain;
        if (discontinuity) {
          float t = phase_ / frequency;
          this_sample += stmlib::ThisIntegratedBlepSample(t) * discontinuity;
          next_sample += stmlib::NextIntegratedBlepSample(t) * discontinuity;
        }
        ascending_ = true;
      }
    }
    step_ = next_step;

    // Contribution from NES triangle.
    next_sample += nes_gain * \
        static_cast<float>(step_ < kHalf ? step_ : kTop - step_);

    // The omitted product is positive zero throughout the normalized phase.
    if (smooth_component) {
      next_sample += tri_gain *
          (phase_ < 0.5f ? 2.0f * phase_ : 2.0f - 2.0f * phase_);
    } else {
      next_sample += 0.0f;
    }
    return next_sample;
  }

  // Oscillator state.
  float phase_;
  float next_sample_;
  int step_;
  bool ascending_;

  // For interpolation of parameters.
  float frequency_;

  DISALLOW_COPY_AND_ASSIGN(NESTriangleOscillator);
};
  
}  // namespace plaits

#endif  // PLAITS_DSP_OSCILLATOR_NES_TRIANGLE_OSCILLATOR_H_

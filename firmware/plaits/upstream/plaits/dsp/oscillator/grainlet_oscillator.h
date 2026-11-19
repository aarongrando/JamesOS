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
// A phase-distortd single cycle sine * another continuously running sine,
// the whole thing synced to a main oscillator.

#ifndef PLAITS_DSP_OSCILLATOR_GRAINLET_OSCILLATOR_H_
#define PLAITS_DSP_OSCILLATOR_GRAINLET_OSCILLATOR_H_

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/parameter_interpolator.h"
#include "stmlib/dsp/polyblep.h"

#include "plaits/dsp/oscillator/oscillator.h"
#include "plaits/dsp/oscillator/sine_oscillator.h"

namespace plaits {

class GrainletOscillator {
 public:
  GrainletOscillator() { }
  ~GrainletOscillator() { }

  void Init() {
    carrier_phase_ = 0.0f;
    formant_phase_ = 0.0f;
    next_sample_ = 0.0f;
  
    carrier_frequency_ = 0.0f;
    formant_frequency_ = 0.0f;
    carrier_shape_ = 0.0f;
    carrier_bleed_ = 0.0f;
  }
  
  // Both synchronized formants use the same carrier and modulation ramps.
  void RenderPair(
      GrainletOscillator& other,
      float carrier_frequency,
      float formant_frequency,
      float other_formant_frequency,
      float carrier_shape,
      float carrier_bleed,
      float* out,
      float* aux,
      size_t size) {
    {
      const float max_carrier_frequency = kMaxFrequency * 0.5f;
      stmlib::ParameterInterpolator carrier_frequency_modulation(
          &carrier_frequency_, std::min(carrier_frequency, max_carrier_frequency), size);
      stmlib::ParameterInterpolator formant_frequency_modulation(
          &formant_frequency_, std::min(formant_frequency, kMaxFrequency), size);
      stmlib::ParameterInterpolator other_frequency_modulation(
          &other.formant_frequency_, std::min(other_formant_frequency, kMaxFrequency), size);
      stmlib::ParameterInterpolator shape_modulation(&carrier_shape_, carrier_shape, size);
      stmlib::ParameterInterpolator bleed_modulation(&carrier_bleed_, carrier_bleed, size);

      float phase = carrier_phase_;
      float formant = formant_phase_;
      float other_formant = other.formant_phase_;
      float next = next_sample_;
      float other_next = other.next_sample_;
      while (size--) {
        float sample = next;
        float other_sample = other_next;
        next = 0.0f;
        other_next = 0.0f;
        const float f0 = carrier_frequency_modulation.Next();
        const float f1 = formant_frequency_modulation.Next();
        const float f2 = other_frequency_modulation.Next();
        phase += f0;
        if (phase >= 1.0f) {
          phase -= 1.0f;
          const float t = phase / f0;
          const float u = 1.0f - t;
          const float before_carrier = Carrier(1.0f, shape_modulation.subsample(u));
          const float before_bleed = bleed_modulation.subsample(u);
          const float before = Grainlet(before_carrier, formant + u * f1, before_bleed);
          const float other_before = Grainlet(before_carrier, other_formant + u * f2, before_bleed);
          const float after = Grainlet(Carrier(0.0f, shape_modulation.subsample(1.0f)),
                                      0.0f, bleed_modulation.subsample(1.0f));
          const float step = after - before;
          const float other_step = after - other_before;
          const float blep = stmlib::ThisBlepSample(t);
          const float next_blep = stmlib::NextBlepSample(t);
          sample += step * blep;
          other_sample += other_step * blep;
          next += step * next_blep;
          other_next += other_step * next_blep;
          formant = t * f1;
          other_formant = t * f2;
        } else {
          formant += f1;
          if (formant >= 1.0f) formant -= 1.0f;
          other_formant += f2;
          if (other_formant >= 1.0f) other_formant -= 1.0f;
        }
        const float carrier = Carrier(phase, shape_modulation.Next());
        const float bleed = bleed_modulation.Next();
        next += Grainlet(carrier, formant, bleed);
        other_next += Grainlet(carrier, other_formant, bleed);
        *out++ = sample;
        *aux++ = other_sample;
      }
      carrier_phase_ = phase;
      formant_phase_ = formant;
      other.formant_phase_ = other_formant;
      next_sample_ = next;
      other.next_sample_ = other_next;
    }
    // Keep both oscillators' state consistent through block boundaries.
    other.carrier_phase_ = carrier_phase_;
    other.carrier_frequency_ = carrier_frequency_;
    other.carrier_shape_ = carrier_shape_;
    other.carrier_bleed_ = carrier_bleed_;
  }

 private:
  inline float Carrier(float phase, float shape) {
    shape *= 3.0f;
    MAKE_INTEGRAL_FRACTIONAL(shape);
    float t = 1.0f - shape_fractional;
    
    if (shape_integral == 0) {
      phase = phase * (1.0f + t * t * t * 15.0f);
      if (phase >= 1.0f) {
        phase = 1.0f;
      }
      phase += 0.75f;
    } else if (shape_integral == 1) {
      float breakpoint = 0.001f + 0.499f * t * t * t;
      if (phase < breakpoint) {
        phase *= (0.5f / breakpoint);
      } else {
        phase = 0.5f + (phase - breakpoint) * 0.5f / (1.0f - breakpoint);
      }
      phase += 0.75f;
    } else {
      t = 1.0f - t;
      phase = 0.25f + phase * (0.5f + t * t * t * 14.5f);
      if (phase >= 0.75f) phase = 0.75f;
    }
    return (Sine(phase) + 1.0f) * 0.25f;
  }

  inline float Grainlet(float carrier, float formant_phase, float bleed) {
    return carrier * (Sine(formant_phase) + bleed) / (1.0f + bleed);
  }

  // Oscillator state.
  float carrier_phase_;
  float formant_phase_;
  float next_sample_;

  // For interpolation of parameters.
  float carrier_frequency_;
  float formant_frequency_;
  float carrier_shape_;
  float carrier_bleed_;
  
  DISALLOW_COPY_AND_ASSIGN(GrainletOscillator);
};
  
}  // namespace plaits

#endif  // PLAITS_DSP_OSCILLATOR_GRAINLET_OSCILLATOR_H_

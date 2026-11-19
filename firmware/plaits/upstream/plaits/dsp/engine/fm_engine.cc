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
// Classic 2-op FM found in Braids, Rings and Elements.

#include "plaits/dsp/engine/fm_engine.h"

#include "stmlib/dsp/parameter_interpolator.h"

#include "plaits/dsp/oscillator/sine_oscillator.h"
#include "plaits/dsp/downsampler/4x_downsampler.h"

namespace plaits {

using namespace stmlib;

PLAITS_COLD_INIT_ATTR void FMEngine::Init(BufferAllocator* allocator) {
  carrier_phase_ = 0;
  modulator_phase_ = 0;
  sub_phase_ = 0;

  previous_carrier_frequency_ = a0;
  previous_modulator_frequency_ = a0;
  previous_amount_ = 0.0f;
  previous_feedback_ = 0.0f;
  previous_sample_ = 0.0f;
}

PLAITS_COLD_INIT_ATTR void FMEngine::Reset() {
  
}

void FMEngine::Render(
    const EngineParameters& parameters,
    float* out,
    float* aux,
    size_t size,
    bool* already_enveloped) {
  
  // 4x oversampling
  const float note = parameters.note - 24.0f;
  
  const float ratio = Interpolate(
      lut_fm_frequency_quantizer,
      parameters.harmonics,
      128.0f);
  
  float modulator_note = note + ratio;
  float target_modulator_frequency = NoteToFrequency(modulator_note);
  CONSTRAIN(target_modulator_frequency, 0.0f, 0.5f);

  // Reduce the maximum FM index for high pitched notes, to prevent aliasing.
  float hf_taming = 1.0f - (modulator_note - 72.0f) * 0.025f;
  CONSTRAIN(hf_taming, 0.0f, 1.0f);
  hf_taming *= hf_taming;
  
  ParameterInterpolator carrier_frequency(
      &previous_carrier_frequency_, NoteToFrequency(note), size);
  ParameterInterpolator modulator_frequency(
      &previous_modulator_frequency_, target_modulator_frequency, size);
  ParameterInterpolator amount_modulation(
      &previous_amount_,
      2.0f * parameters.timbre * parameters.timbre * hf_taming,
      size);
  ParameterInterpolator feedback_modulation(
      &previous_feedback_, 2.0f * parameters.morph - 1.0f, size);
  
  float carrier_head = carrier_fir_, carrier_tail = 0.0f;
  float sub_head = sub_fir_, sub_tail = 0.0f;
  auto accumulate = [](float& head, float& tail, size_t j, float sample)
      __attribute__((always_inline)) {
#if defined(__clang__) && __clang_major__ >= 12
#pragma clang fp reassociate(off)
#endif
    head += sample * lut_4x_downsampler_fir[3 - j];
    tail += sample * lut_4x_downsampler_fir[j];
  };

  while (size--) {
    const float max_uint32 = 4294967296.0f;
    const float amount = amount_modulation.Next();
    const float feedback = feedback_modulation.Next();
    const uint32_t carrier_increment = static_cast<uint32_t>(
        max_uint32 * carrier_frequency.Next());
    const float modulator_step = max_uint32 * modulator_frequency.Next();

    // The sign of this sample's interpolated feedback selects exactly one
    // feedback route. All four oversamples use the same sign and amount.
    auto render_four = [&](bool frequency_feedback) __attribute__((always_inline)) {
      const float phase_feedback = 0.5f * feedback * feedback;
      const float modulator_feedback = 0.25f * feedback * feedback;
      auto step = [&](size_t j) __attribute__((always_inline)) {
        const float increment = frequency_feedback
            ? modulator_step * (1.0f + previous_sample_ * phase_feedback)
            : modulator_step;
        modulator_phase_ += static_cast<uint32_t>(increment);
        carrier_phase_ += carrier_increment;
        sub_phase_ += carrier_increment >> 1;
        const float modulator = SinePM(modulator_phase_,
            frequency_feedback ? 0.0f : modulator_feedback * previous_sample_);
        const float carrier = SinePM(carrier_phase_, amount * modulator);
        const float sub = SinePM(sub_phase_, amount * carrier * 0.25f);
        ONE_POLE(previous_sample_, carrier, 0.05f);
        accumulate(carrier_head, carrier_tail, j, carrier);
        accumulate(sub_head, sub_tail, j, sub);
      };
      static_assert(kOversampling == 4, "FM FIR has four ordered taps");
      step(0);
      step(1);
      step(2);
      step(3);
    };
    if (feedback < 0.0f) {
      render_four(true);
    } else {
      render_four(false);
    }

    *out++ = carrier_head;
    *aux++ = sub_head;
    carrier_head = carrier_tail;
    sub_head = sub_tail;
    carrier_tail = sub_tail = 0.0f;
  }
  carrier_fir_ = carrier_head;
  sub_fir_ = sub_head;
}

}  // namespace plaits

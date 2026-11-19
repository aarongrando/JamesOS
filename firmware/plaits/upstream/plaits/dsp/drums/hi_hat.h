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
// 808 HH, with a few extra parameters to push things to the CY territory...
// The template parameter MetallicNoiseSource allows another kind of "metallic
// noise" to be used, for results which are more similar to KR-55 or FM hi-hats.

#ifndef PLAITS_DSP_DRUMS_HI_HAT_H_
#define PLAITS_DSP_DRUMS_HI_HAT_H_

#include <algorithm>

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/filter.h"
#include "stmlib/dsp/parameter_interpolator.h"
#include "stmlib/dsp/units.h"
#include "stmlib/utils/random.h"

#include "plaits/dsp/dsp.h"
#include "plaits/dsp/oscillator/oscillator.h"

/* Device-only placement of the call-free 808 metallic-noise loop in RAM code.
 * HiHat::Render stays in XIP flash, beyond direct-call range of RAM, so
 * SquareNoise::Render reaches the RAM copy through a volatile member-function
 * pointer (as ResonatorSvf reaches its paired kernel). Body unchanged. */
#ifndef FM1_PLAITS_SQUARE_NOISE_RAM
#define FM1_PLAITS_SQUARE_NOISE_RAM 1
#endif
#if FM1_PLAITS_SQUARE_NOISE_RAM && defined(FM1_DEVICE)
#define FM1_PLAITS_SQUARE_NOISE_RAM_ACTIVE 1
#define FM1_PLAITS_SQUARE_NOISE_RAM_CODE \
  __attribute__((section(".ram_audio.plaits_drums"), noinline))
#else
#define FM1_PLAITS_SQUARE_NOISE_RAM_ACTIVE 0
#endif

namespace plaits {

#if defined(FM1_PLAITS_HIHAT_STATS) && FM1_PLAITS_HIHAT_STATS
// Host test instrumentation only (tests/hihat_exact_harness.cc).
enum {
  HIHAT_STAT_RINGMOD_CHUNKS,
  HIHAT_STAT_RINGMOD_ALL_CONSTANT,
  HIHAT_STAT_RINGMOD_CONSTANT_PAIRS,
  HIHAT_STAT_RINGMOD_STEADY_SAWS,
  HIHAT_STAT_COUNT
};
static uint32_t g_hihat_stats[HIHAT_STAT_COUNT];
#define FM1_HIHAT_STAT(index, amount) (g_hihat_stats[index] += (amount))
#else
#define FM1_HIHAT_STAT(index, amount) ((void)0)
#endif

struct HiHatControlCoefficients {
  float envelope_decay;
  float cut_decay;
  float cutoff;
};

// 808 style "metallic noise" with 6 square oscillators.
class SquareNoise {
 public:
  SquareNoise() { }
  ~SquareNoise() { }

  void Init() {
    std::fill(&phase_[0], &phase_[6], 0);
  }
    
#if FM1_PLAITS_SQUARE_NOISE_RAM_ACTIVE
  typedef void (SquareNoise::*RenderRamFn)(
      float, float*, float*, float*, size_t);

  void Render(float f0, float* temp_1, float* temp_2, float* out, size_t size) {
    RenderRamFn volatile render = &SquareNoise::RenderRam;
    const RenderRamFn render_fn = render;
    (this->*render_fn)(f0, temp_1, temp_2, out, size);
  }

  // Ordinary inline helpers do not inline into another section on this
  // compiler; keep the original body as an always_inline member.
  FM1_PLAITS_SQUARE_NOISE_RAM_CODE void RenderRam(
      float f0, float* temp_1, float* temp_2, float* out, size_t size) {
    RenderBody(f0, temp_1, temp_2, out, size);
  }

  __attribute__((always_inline)) inline void RenderBody(
#else
  void Render(
#endif
      float f0, float* temp_1, float* temp_2, float* out, size_t size) {
    const float ratios[6] = {
        // Nominal f0: 414 Hz
        1.0f, 1.304f, 1.466f, 1.787f, 1.932f, 2.536f
    };
  
    uint32_t increment[6];
    uint32_t phase[6];
    for (int i = 0; i < 6; ++i) {
      float f = f0 * ratios[i];
      if (f >= 0.499f) f = 0.499f;
      increment[i] = static_cast<uint32_t>(f * 4294967296.0f);
      phase[i] = phase_[i];
    }

    while (size--) {
      phase[0] += increment[0];
      phase[1] += increment[1];
      phase[2] += increment[2];
      phase[3] += increment[3];
      phase[4] += increment[4];
      phase[5] += increment[5];
      uint32_t noise = 0;
      noise += (phase[0] >> 31);
      noise += (phase[1] >> 31);
      noise += (phase[2] >> 31);
      noise += (phase[3] >> 31);
      noise += (phase[4] >> 31);
      noise += (phase[5] >> 31);
      *out++ = 0.33f * static_cast<float>(noise) - 1.0f;
    }
  
    for (int i = 0; i < 6; ++i) {
      phase_[i] = phase[i];
    }
  }

 private:
  uint32_t phase_[6];

  DISALLOW_COPY_AND_ASSIGN(SquareNoise);
};

class RingModNoise {
 public:
  RingModNoise() { }
  ~RingModNoise() { }

  void Init() {
    for (int i = 0; i < 6; ++i) {
      oscillator_[i].Init();
    }
  }
  
  void Render(float f0, float* temp_1, float* temp_2, float* out, size_t size) {
    const float ratio = f0 / (0.01f + f0);
    const float f1a = 200.0f / kSampleRate * ratio;
    const float f1b = 7530.0f / kSampleRate * ratio;
    const float f2a = 510.0f / kSampleRate * ratio;
    const float f2b = 8075.0f / kSampleRate * ratio;
    const float f3a = 730.0f / kSampleRate * ratio;
    const float f3b = 10500.0f / kSampleRate * ratio;
    const float f[3][2] = { { f1a, f1b }, { f2a, f2b }, { f3a, f3b } };
#if !(FM1_PLAITS_HIHAT_RINGMOD_CONSTANT_SQUARE && \
      FM1_PLAITS_HIHAT_FAST_BLEP_RECIPROCAL)
    std::fill(&out[0], &out[size], 0.0f);
#endif

#if FM1_PLAITS_HIHAT_FAST_BLEP_RECIPROCAL
    float inverse_f[3][2];
    const bool use_fast_reciprocal = size == kMaxBlockSize;
    const float inverse_block_size = 1.0f / static_cast<float>(kMaxBlockSize);
    if (use_fast_reciprocal) {
      const float inverse_ratio = 1.0f / ratio;
      inverse_f[0][0] = (kSampleRate / 200.0f) * inverse_ratio;
      inverse_f[0][1] = (kSampleRate / 7530.0f) * inverse_ratio;
      inverse_f[1][0] = (kSampleRate / 510.0f) * inverse_ratio;
      inverse_f[1][1] = (kSampleRate / 8075.0f) * inverse_ratio;
      inverse_f[2][0] = (kSampleRate / 730.0f) * inverse_ratio;
      inverse_f[2][1] = (kSampleRate / 10500.0f) * inverse_ratio;
    }
#if FM1_PLAITS_HIHAT_RINGMOD_CONSTANT_SQUARE
    // out = ((0 + q0 * w0) + q1 * w1) + q2 * w2, with q the squares and w the
    // saws. A square that holds one level L for the chunk gives q * w = L * w
    // exactly, so only its saw is rendered (by the unchanged saw code).
    float level[3];
    bool constant[3];
    for (int i = 0; i < 3; ++i) {
      constant[i] = oscillator_[2 * i].TryConstantSquare(
          f[i][0], 0.5f, size, &level[i]);
    }
    FM1_HIHAT_STAT(HIHAT_STAT_RINGMOD_CHUNKS, 1u);
    FM1_HIHAT_STAT(HIHAT_STAT_RINGMOD_CONSTANT_PAIRS,
                   static_cast<uint32_t>(constant[0]) + constant[1] + constant[2]);
    if (constant[0] && constant[1] && constant[2]) {
      FM1_HIHAT_STAT(HIHAT_STAT_RINGMOD_ALL_CONSTANT, 1u);
      RenderSaw(&oscillator_[1], f[0][1], inverse_f[0][1], inverse_block_size,
                use_fast_reciprocal, out, size);
      RenderSaw(&oscillator_[3], f[1][1], inverse_f[1][1], inverse_block_size,
                use_fast_reciprocal, temp_1, size);
      RenderSaw(&oscillator_[5], f[2][1], inverse_f[2][1], inverse_block_size,
                use_fast_reciprocal, temp_2, size);
      const float level_0 = level[0];
      const float level_1 = level[1];
      const float level_2 = level[2];
      // Keep the rounded first sum opaque under the relaxed DSP flags.
      for (size_t j = 0; j < size; ++j) {
        float pair = (0.0f + level_0 * out[j]) + level_1 * temp_1[j];
        __asm__ volatile("" : "+r"(pair));
        out[j] = pair + level_2 * temp_2[j];
      }
      return;
    }
    std::fill(&out[0], &out[size], 0.0f);
    for (int i = 0; i < 3; ++i) {
      if (constant[i]) {
        RenderSaw(&oscillator_[2 * i + 1], f[i][1], inverse_f[i][1],
                  inverse_block_size, use_fast_reciprocal, temp_2, size);
        const float level_i = level[i];
        for (size_t j = 0; j < size; ++j) {
          out[j] += level_i * temp_2[j];
        }
      } else {
        RenderPair(&oscillator_[2 * i], f[i], inverse_f[i],
                   inverse_block_size, use_fast_reciprocal, temp_1, temp_2,
                   out, size);
      }
    }
#else
    for (int i = 0; i < 3; ++i) {
      RenderPair(&oscillator_[2 * i], f[i], inverse_f[i], inverse_block_size,
                 use_fast_reciprocal, temp_1, temp_2, out, size);
    }
#endif
#else
    for (int i = 0; i < 3; ++i) {
      RenderPair(&oscillator_[2 * i], f[i], temp_1, temp_2, out, size);
    }
#endif
  }

 private:
#if FM1_PLAITS_HIHAT_RINGMOD_CONSTANT_SQUARE && \
    FM1_PLAITS_HIHAT_FAST_BLEP_RECIPROCAL
  // The saw half of RenderPair.
  void RenderSaw(
      Oscillator* osc,
      float f,
      float inverse_f,
      float inverse_block_size,
      bool use_fast_reciprocal,
      float* out,
      size_t size) {
#if FM1_PLAITS_HIHAT_RINGMOD_STEADY_SAW
    if (osc->TrySteadySaw(f, 0.5f, inverse_f, inverse_block_size,
                          use_fast_reciprocal, out, size)) {
      FM1_HIHAT_STAT(HIHAT_STAT_RINGMOD_STEADY_SAWS, 1u);
      return;
    }
#endif
    if (use_fast_reciprocal) {
      osc->RenderSteadyReciprocal<OSCILLATOR_SHAPE_SAW>(
          f, 0.5f, inverse_f, inverse_block_size, out, size);
    } else {
      osc->Render<OSCILLATOR_SHAPE_SAW>(f, 0.5f, out, size);
    }
  }
#endif

  void RenderPair(
      Oscillator* osc,
      const float* f,
#if FM1_PLAITS_HIHAT_FAST_BLEP_RECIPROCAL
      const float* inverse_f,
      float inverse_block_size,
      bool use_fast_reciprocal,
#endif
      float* temp_1,
      float* temp_2,
      float* out,
      size_t size) {
#if FM1_PLAITS_HIHAT_FAST_BLEP_RECIPROCAL && \
    FM1_PLAITS_HIHAT_RINGMOD_STEADY_SAW
    // A square with an edge in this slice: rendered as before; its saw
    // still takes the stationary loop when it qualifies.
    if (use_fast_reciprocal) {
      osc[0].RenderSteadyReciprocal<OSCILLATOR_SHAPE_SQUARE>(
          f[0], 0.5f, inverse_f[0], inverse_block_size, temp_1, size);
    } else {
      osc[0].Render<OSCILLATOR_SHAPE_SQUARE>(f[0], 0.5f, temp_1, size);
    }
    RenderSaw(&osc[1], f[1], inverse_f[1], inverse_block_size,
              use_fast_reciprocal, temp_2, size);
#elif FM1_PLAITS_HIHAT_FAST_BLEP_RECIPROCAL
    if (use_fast_reciprocal) {
      osc[0].RenderSteadyReciprocal<OSCILLATOR_SHAPE_SQUARE>(
          f[0], 0.5f, inverse_f[0], inverse_block_size, temp_1, size);
      osc[1].RenderSteadyReciprocal<OSCILLATOR_SHAPE_SAW>(
          f[1], 0.5f, inverse_f[1], inverse_block_size, temp_2, size);
    } else {
      osc[0].Render<OSCILLATOR_SHAPE_SQUARE>(f[0], 0.5f, temp_1, size);
      osc[1].Render<OSCILLATOR_SHAPE_SAW>(f[1], 0.5f, temp_2, size);
    }
#else
    osc[0].Render<OSCILLATOR_SHAPE_SQUARE>(f[0], 0.5f, temp_1, size);
    osc[1].Render<OSCILLATOR_SHAPE_SAW>(f[1], 0.5f, temp_2, size);
#endif
    while (size--) {
      *out++ += *temp_1++ * *temp_2++;
    }
  }
  Oscillator oscillator_[6];
  
  DISALLOW_COPY_AND_ASSIGN(RingModNoise);
};

class SwingVCA {
 public:
  float operator()(float s, float gain) {
   s *= s > 0.0f ? 4.0f : 0.1f;
   s = s / (1.0f + __builtin_fabsf(s));
   return (s + 0.1f) * gain;
  }
};

class LinearVCA {
 public:
  float operator()(float s, float gain) {
   return s * gain;
  }
};

template<
    typename MetallicNoiseSource,
    typename VCA,
    bool resonance,
    bool two_stage_envelope>
class HiHat {
 public:
  HiHat() { }
  ~HiHat() { }

  PLAITS_COLD_INIT_ATTR void Init() {
    envelope_ = 0.0f;
    noise_clock_ = 0.0f;
    noise_sample_ = 0.0f;
    sustain_gain_ = 0.0f;

    metallic_noise_.Init();
    noise_coloration_svf_.Init();
    hpf_.Init();
  }
  
  void Render(
      bool sustain,
      bool trigger,
      float accent,
      float f0,
      float tone,
      float decay,
      const HiHatControlCoefficients& coefficients,
      float noisiness,
      float* temp_1,
      float* temp_2,
      float* out,
      size_t size) {
#if FM1_PLAITS_TRIGGER_ALWAYS_PATCHED
    // Not inlined into HiHatEngine::Render, so the engine's constant does
    // not reach here; drop the unreachable sustain VCA (plaits_config.h).
    sustain = false;
#endif
    if (trigger) {
      envelope_ = (1.5f + 0.5f * (1.0f - decay)) * (0.3f + 0.7f * accent);
    }

    // Render the metallic noise.
    metallic_noise_.Render(2.0f * f0, temp_1, temp_2, out, size);

    // Apply BPF on the metallic noise.
    noise_coloration_svf_.set_f_q<stmlib::FREQUENCY_ACCURATE>(
        coefficients.cutoff, resonance ? 3.0f + 3.0f * tone : 1.0f);
#if FM1_PLAITS_HIHAT_LOCAL_SVF_COEFFICIENTS
    noise_coloration_svf_.ProcessLocalCoefficients<
        stmlib::FILTER_MODE_BAND_PASS>(out, out, size);
#else
    noise_coloration_svf_.Process<stmlib::FILTER_MODE_BAND_PASS>(
        out, out, size);
#endif
    
    // This is not at all part of the 808 circuit! But to add more variety, we
    // add a variable amount of clocked noise to the output of the 6 schmitt
    // trigger oscillators.
    noisiness *= noisiness;
    float noise_f = f0 * (16.0f + 16.0f * (1.0f - noisiness));
    CONSTRAIN(noise_f, 0.0f, 0.5f);
    
#if FM1_PLAITS_HIHAT_LOCAL_STATE
    {
      float noise_clock = noise_clock_;
      float noise_sample = noise_sample_;
      for (size_t i = 0; i < size; ++i) {
        noise_clock += noise_f;
        if (noise_clock >= 1.0f) {
          noise_clock -= 1.0f;
          noise_sample = stmlib::Random::GetFloat() - 0.5f;
        }
        out[i] += noisiness * (noise_sample - out[i]);
      }
      noise_clock_ = noise_clock;
      noise_sample_ = noise_sample;
    }

    // Apply VCA.
    stmlib::ParameterInterpolator sustain_gain(
        &sustain_gain_,
        accent * decay,
        size);
    {
      float envelope = envelope_;
      const float envelope_decay = coefficients.envelope_decay;
      const float cut_decay = coefficients.cut_decay;
      for (size_t i = 0; i < size; ++i) {
        VCA vca;
        envelope *= envelope > 0.5f || !two_stage_envelope
            ? envelope_decay
            : cut_decay;
        out[i] = vca(out[i], sustain ? sustain_gain.Next() : envelope);
      }
      envelope_ = envelope;
    }

#else
    for (size_t i = 0; i < size; ++i) {
      noise_clock_ += noise_f;
      if (noise_clock_ >= 1.0f) {
        noise_clock_ -= 1.0f;
        noise_sample_ = stmlib::Random::GetFloat() - 0.5f;
      }
      out[i] += noisiness * (noise_sample_ - out[i]);
    }

    // Apply VCA.
    stmlib::ParameterInterpolator sustain_gain(
        &sustain_gain_,
        accent * decay,
        size);
    for (size_t i = 0; i < size; ++i) {
      VCA vca;
      envelope_ *= envelope_ > 0.5f || !two_stage_envelope
          ? coefficients.envelope_decay
          : coefficients.cut_decay;
      out[i] = vca(out[i], sustain ? sustain_gain.Next() : envelope_);
    }
#endif
    
    hpf_.set_f_q<stmlib::FREQUENCY_ACCURATE>(coefficients.cutoff, 0.5f);
#if FM1_PLAITS_HIHAT_LOCAL_SVF_COEFFICIENTS
    hpf_.ProcessLocalCoefficients<stmlib::FILTER_MODE_HIGH_PASS>(
        out, out, size);
#else
    hpf_.Process<stmlib::FILTER_MODE_HIGH_PASS>(out, out, size);
#endif
  }

 private:
  float envelope_;
  float noise_clock_;
  float noise_sample_;
  float sustain_gain_;

  MetallicNoiseSource metallic_noise_;
  stmlib::Svf noise_coloration_svf_;
  stmlib::Svf hpf_;
  
  DISALLOW_COPY_AND_ASSIGN(HiHat);
};
  
}  // namespace plaits

#endif  // PLAITS_DSP_DRUMS_HI_HAT_H_

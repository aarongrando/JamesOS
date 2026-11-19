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
// Single waveform oscillator. Can optionally do audio-rate linear FM, with
// through-zero capabilities (negative frequencies).

#ifndef PLAITS_DSP_OSCILLATOR_OSCILLATOR_H_
#define PLAITS_DSP_OSCILLATOR_OSCILLATOR_H_

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/parameter_interpolator.h"
#include "stmlib/dsp/polyblep.h"

#include "plaits_config.h"

namespace plaits {

enum OscillatorShape {
  OSCILLATOR_SHAPE_IMPULSE_TRAIN,
  OSCILLATOR_SHAPE_SAW,
  OSCILLATOR_SHAPE_TRIANGLE,
  OSCILLATOR_SHAPE_SLOPE,
  OSCILLATOR_SHAPE_SQUARE,
  OSCILLATOR_SHAPE_SQUARE_BRIGHT,
  OSCILLATOR_SHAPE_SQUARE_DARK,
  OSCILLATOR_SHAPE_SQUARE_TRIANGLE
};

const float kMaxFrequency = 0.25f;
const float kMinFrequency = 0.000001f;

class Oscillator {
 public:
  Oscillator() { }
  ~Oscillator() { }
  
  void Init() {
    phase_ = 0.5f;
    next_sample_ = 0.0f;
    lp_state_ = 1.0f;
    hp_state_ = 0.0f;
    high_ = true;

    frequency_ = 0.001f;
    pw_ = 0.5f;
  }

  template<OscillatorShape shape>
  void Render(float frequency, float pw, float* out, size_t size) {
    Render<shape, false, false>(frequency, pw, NULL, out, size);
  }
  
  template<OscillatorShape shape>
  void Render(
      float frequency,
      float pw,
      const float* fm,
      float* out,
      size_t size) {
    if (!fm) {
      Render<shape, false, false>(frequency, pw, NULL, out, size);
    } else {
      Render<shape, true, true>(frequency, pw, fm, out, size);
    }
  }

  /* Ring-mod hi-hat oscillators keep stable per-block frequencies. This path
   * retains the per-sample frequency trajectory but multiplies BLEP event
   * fractions by a shared block reciprocal instead of dividing per event.
   * Larger control changes use the original path. */
  template<OscillatorShape shape>
  void RenderSteadyReciprocal(float frequency, float pw,
                              float inverse_frequency, float inverse_block_size,
                              float* out, size_t size) {
    if (!size) return;
    CONSTRAIN(frequency, kMinFrequency, kMaxFrequency);
    CONSTRAIN(pw, __builtin_fabsf(frequency) * 2.0f,
              1.0f - 2.0f * __builtin_fabsf(frequency))
    const float frequency_error = __builtin_fabsf(frequency_ - frequency);
    if ((shape != OSCILLATOR_SHAPE_SAW &&
         shape != OSCILLATOR_SHAPE_SQUARE) ||
        pw_ != pw || frequency_error > 1.0e-6f * frequency ||
        frequency * static_cast<float>(size) < 1.0f) {
      Render<shape, false, false>(frequency, pw, NULL, out, size);
      return;
    }

    const float frequency_increment =
        (frequency - frequency_) * inverse_block_size;
    float interpolated_frequency = frequency_;
    float next_sample = next_sample_;
    if (shape == OSCILLATOR_SHAPE_SAW) {
      while (size--) {
        float this_sample = next_sample;
        next_sample = 0.0f;
        interpolated_frequency += frequency_increment;
        phase_ += interpolated_frequency;
        if (phase_ >= 1.0f) {
          phase_ -= 1.0f;
          const float t = phase_ * inverse_frequency;
          this_sample -= stmlib::ThisBlepSample(t);
          next_sample -= stmlib::NextBlepSample(t);
        }
        next_sample += phase_;
        *out++ = 2.0f * this_sample - 1.0f;
      }
    } else {
      while (size--) {
        float this_sample = next_sample;
        next_sample = 0.0f;
        interpolated_frequency += frequency_increment;
        phase_ += interpolated_frequency;
        if (high_ ^ (phase_ >= pw)) {
          const float t = (phase_ - pw) * inverse_frequency;
          this_sample += stmlib::ThisBlepSample(t);
          next_sample += stmlib::NextBlepSample(t);
          high_ = phase_ >= pw;
        }
        if (phase_ >= 1.0f) {
          phase_ -= 1.0f;
          const float t = phase_ * inverse_frequency;
          this_sample -= stmlib::ThisBlepSample(t);
          next_sample -= stmlib::NextBlepSample(t);
          high_ = false;
        }
        next_sample += phase_ < pw ? 0.0f : 1.0f;
        this_sample = 2.0f * this_sample - 1.0f;
        *out++ = this_sample;
      }
    }
    next_sample_ = next_sample;
    frequency_ = interpolated_frequency;
    pw_ = pw;
  }

#if FM1_PLAITS_HIHAT_RINGMOD_CONSTANT_SQUARE || \
    FM1_PLAITS_HIHAT_RINGMOD_STEADY_SAW
  /* True only if fl(v + increment) == v, i.e. a ParameterInterpolator that
   * starts at v > 0 with this increment returns v at every step and writes v
   * back. increment must be the interpolator's own rounded increment toward
   * target: fl((target - v) / n) or fl((target - v) * fl(1 / n)).
   *
   * Division-free sufficient test. Let v lie in [2^e, 2^(e+1)), so its float
   * spacing is u = 2^(e-23). |increment| <= |target - v| / n * (1 + 2^-22)
   * (two roundings). If |target - v| < n * v * 1.4e-8, then, since
   * 1.4e-8 * (1 + 2^-22)^3 < 2^-26 = 1.4901e-8 (the product n * v * 1.4e-8
   * is itself rounded twice), |increment| < v * 2^-26 < 2^(e+1) * 2^-26 =
   * u / 4. Adding less than u / 4 to v rounds back to v in round-to-nearest
   * (the nearest other float is u away above, and at least u / 2 below).
   * Otherwise the sum is formed and compared, kept opaque so the comparison
   * is made on the rounded float, as the interpolator makes it. */
  static inline bool InterpolationIsStationary(
      float v, float target, float n, float increment) {
    if (v == target) return true;
    if (__builtin_fabsf(target - v) < n * v * 1.4e-8f) return true;
    float moved = v + increment;
    __asm__ volatile("" : "+r"(moved));
    return moved == v;
  }
#endif

#if FM1_PLAITS_HIHAT_RINGMOD_STEADY_SAW
  /* Exact shortcut for a SAW render at a stationary frequency and pulse
   * width: the same per-sample operations as Render<OSCILLATOR_SHAPE_SAW>
   * (or the saw loop of RenderSteadyReciprocal, when that entry point would
   * have used it) minus the two interpolator additions, which are proven to
   * add nothing (InterpolationIsStationary; pw_ == pw gives a +0 increment),
   * and with the phase held in a local. Returns false, changing nothing, if
   * the frequency or pulse width is still moving; the caller then renders
   * normally. reciprocal_entry says the caller would have entered through
   * RenderSteadyReciprocal (the ring-mod hi-hat does for full slices).
   * Not inlined: one copy in flash serves the three saws. */
  __attribute__((noinline)) bool TrySteadySaw(
      float frequency, float pw, float inverse_frequency,
      float inverse_block_size, bool reciprocal_entry,
      float* out, size_t size) {
    if (!size) return false;
    CONSTRAIN(frequency, kMinFrequency, kMaxFrequency);
    CONSTRAIN(pw, __builtin_fabsf(frequency) * 2.0f,
              1.0f - 2.0f * __builtin_fabsf(frequency))
    if (pw_ != pw) return false;
    const float n = static_cast<float>(size);
    const float v = frequency_;
    if (!(v > 0.0f)) return false;
    // Mirror of RenderSteadyReciprocal's own fallback test.
    const bool reciprocal = reciprocal_entry &&
        !(__builtin_fabsf(v - frequency) > 1.0e-6f * frequency ||
          frequency * n < 1.0f);
    const float increment = reciprocal
        ? (frequency - v) * inverse_block_size
        : (frequency - v) / n;
    if (!InterpolationIsStationary(v, frequency, n, increment)) return false;

    float phase = phase_;
    float next_sample = next_sample_;
    if (reciprocal) {
      while (size--) {
        float this_sample = next_sample;
        next_sample = 0.0f;
        phase += v;
        if (phase >= 1.0f) {
          phase -= 1.0f;
          const float t = phase * inverse_frequency;
          this_sample -= stmlib::ThisBlepSample(t);
          next_sample -= stmlib::NextBlepSample(t);
        }
        next_sample += phase;
        *out++ = 2.0f * this_sample - 1.0f;
      }
    } else {
      while (size--) {
        float this_sample = next_sample;
        next_sample = 0.0f;
        phase += v;
        if (phase >= 1.0f) {
          phase -= 1.0f;
          float t = phase / v;
          this_sample -= stmlib::ThisBlepSample(t);
          next_sample -= stmlib::NextBlepSample(t);
        }
        next_sample += phase;
        *out++ = 2.0f * this_sample - 1.0f;
      }
    }
    phase_ = phase;
    next_sample_ = next_sample;
    return true;
  }
#endif

#if FM1_PLAITS_HIHAT_RINGMOD_PHASE_CLOSED_FORM
  /* Exact closed form for n sequential float additions p <- fl(p + v) that
   * stay inside one binade. Returns true with *phase = the value those n
   * additions produce, and that value below limit; returns false (caller
   * falls back to the additions themselves) whenever the closed form does
   * not apply or the result would not be below limit.
   *
   * Let B = 2^e be the largest power of two <= p0, u = B * 2^-23 the float
   * spacing in [B, 2B), and suppose 0 < v < B. Every float in [B, 2B) is a
   * multiple of u. Write v = m * u + r with m an integer and |r| <= u / 2.
   * If |r| < u / 2 and p_k, p_k + m * u both lie in [B, 2B), the exact sum
   * p_k + v is within u / 2 of the float p_k + m * u, strictly, so
   * round-to-nearest gives fl(p_k + v) = p_k + m * u: each addition adds
   * exactly w = m * u, and p_n = p0 + n * w, an integer multiple of u, as
   * long as p_n < 2B (the sums increase, so that covers every step).
   *
   * Float evaluation, each step exact:
   *   w = fl(B + v) - B. B + v lies in [B, 2B), so fl rounds v to a
   *       multiple of u, and the subtraction of B is exact (Sterbenz).
   *   A tie (|r| = u / 2, where the rounding direction of a real step
   *       depends on the parity of p_k) shows as |w - v| = u / 2 exactly
   *       (w - v is a multiple of ulp(v) below 2^24 ulp(v): representable);
   *       ties are refused.
   *   n * w: if the true product is below B it is an integer below 2^23
   *       times u, representable, so fl is exact; if not, fl(n * w) >= B
   *       and the final test fails. p0 + n * w: a multiple of u; below 2B
   *       it is representable, so fl is exact; otherwise fl >= 2B and the
   *       final test fails.
   * The two intermediates are kept opaque so a reassociating compiler
   * cannot cancel (B + v) - B. */
  static inline bool AccumulateInBinade(
      float* phase, float v, float n, float limit) {
    const float p0 = *phase;
    uint32_t bits;
    __builtin_memcpy(&bits, &p0, sizeof(bits));
    bits &= 0x7f800000u;
    float base;
    __builtin_memcpy(&base, &bits, sizeof(base));
    // Normal, positive binade with room for u / 2, and 0 < v < B.
    if (!(base >= 1.0e-30f) || !(p0 >= base) || !(v > 0.0f) || !(v < base)) {
      return false;
    }
    float sum = base + v;
    __asm__ volatile("" : "+r"(sum));
    float w = sum - base;
    __asm__ volatile("" : "+r"(w));
    const float half_spacing = base * 5.9604644775390625e-08f;  // B * 2^-24
    if (__builtin_fabsf(w - v) == half_spacing) return false;
    const float top = base + base;
    const float end = p0 + n * w;
    if (!(end < top) || !(end < limit)) return false;
    *phase = end;
    return true;
  }
#endif

#if FM1_PLAITS_HIHAT_RINGMOD_CONSTANT_SQUARE
  /* Exact shortcut for a SQUARE render whose output is one constant level.
   *
   * Render<OSCILLATOR_SHAPE_SQUARE>(frequency, pw, out, size) computes, per
   * sample, v += dv; pw' += dpw; phase += v; then a BLEP correction if the
   * phase crossed pw' or 1, and out = 2 * this_sample - 1. If
   *   (a) pw_ == pw            (so dpw = +0 and pw' stays pw),
   *   (b) fl(v + dv) == v      (so every sample adds the same v > 0),
   *   (c) high_ == (phase_ >= pw) and next_sample_ == (high_ ? 1 : 0)
   *                            (no BLEP residue is pending), and
   *   (d) after the size sequential additions the phase is still below the
   *       next crossing (pw when low, 1 when high),
   * then, because the additions of v > 0 are monotone, no sample crosses, no
   * correction term is ever added, every output is exactly 2 * 1 - 1 = +1
   * (high) or 2 * 0 - 1 = -1 (low), and the only state that changes is
   * phase_, by those same size additions. This function tests (a)-(d) with
   * the same float operations the render would perform; on success it commits
   * the phase and returns the level, otherwise it changes nothing and the
   * caller renders normally. frequency * size < 1 is required so that both
   * entry points used by the ring-mod hi-hat (Render and
   * RenderSteadyReciprocal) would have run the plain Render loop mirrored
   * here. Not inlined: one copy in flash serves the three squares. */
  __attribute__((noinline)) bool TryConstantSquare(
      float frequency, float pw, size_t size, float* level) {
    if (!size) return false;
    CONSTRAIN(frequency, kMinFrequency, kMaxFrequency);
    CONSTRAIN(pw, __builtin_fabsf(frequency) * 2.0f,
              1.0f - 2.0f * __builtin_fabsf(frequency))
    if (pw_ != pw) return false;
    const float n = static_cast<float>(size);
    if (!(frequency * n < 1.0f)) return false;
    const float v = frequency_;
    if (!(v > 0.0f)) return false;
    if (!InterpolationIsStationary(v, frequency, n, (frequency - v) / n)) {
      return false;
    }
    const bool high = high_;
    float phase = phase_;
    if (high != (phase >= pw)) return false;
    if (next_sample_ != (high ? 1.0f : 0.0f)) return false;
    const float limit = high ? 1.0f : pw;
#if FM1_PLAITS_HIHAT_RINGMOD_PHASE_CLOSED_FORM
    if (!AccumulateInBinade(&phase, v, n, limit))
#endif
    {
      phase = phase_;
      for (size_t i = 0; i < size; ++i) {
        phase += v;
        // One rounded float addition per sample, as the render performs
        // them: a compiler allowed to reassociate must not turn this into
        // phase + size * v, which rounds differently.
        __asm__ volatile("" : "+r"(phase));
      }
    }
    if (!(phase < limit)) return false;
    phase_ = phase;
    *level = high ? 1.0f : -1.0f;
    return true;
  }
#endif

  template<OscillatorShape shape, bool has_external_fm, bool through_zero_fm>
  void Render(
      float frequency,
      float pw,
      const float* external_fm,
      float* out,
      size_t size) {
    
    if (!has_external_fm) {
      if (!through_zero_fm) {
        CONSTRAIN(frequency, kMinFrequency, kMaxFrequency);
      } else {
        CONSTRAIN(frequency, -kMaxFrequency, kMaxFrequency);
      }
      CONSTRAIN(pw, __builtin_fabsf(frequency) * 2.0f, 1.0f - 2.0f * __builtin_fabsf(frequency))
    }
    
    stmlib::ParameterInterpolator fm(&frequency_, frequency, size);
    stmlib::ParameterInterpolator pwm(&pw_, pw, size);
  
    float next_sample = next_sample_;
  
    while (size--) {
      float this_sample = next_sample;
      next_sample = 0.0f;

      float frequency = fm.Next();
      if (has_external_fm) {
        frequency *= (1.0f + *external_fm++);
        if (!through_zero_fm) {
          CONSTRAIN(frequency, kMinFrequency, kMaxFrequency);
        } else {
          CONSTRAIN(frequency, -kMaxFrequency, kMaxFrequency);
        }
      }
      float pw = (shape == OSCILLATOR_SHAPE_SQUARE_TRIANGLE ||
                  shape == OSCILLATOR_SHAPE_TRIANGLE) ? 0.5f : pwm.Next();
      if (has_external_fm) {
        CONSTRAIN(pw, __builtin_fabsf(frequency) * 2.0f, 1.0f - 2.0f * __builtin_fabsf(frequency))
      }
      phase_ += frequency;
      
      if (shape <= OSCILLATOR_SHAPE_SAW) {
        if (phase_ >= 1.0f) {
          phase_ -= 1.0f;
          float t = phase_ / frequency;
          this_sample -= stmlib::ThisBlepSample(t);
          next_sample -= stmlib::NextBlepSample(t);
        } else if (through_zero_fm && phase_ < 0.0f) {
          float t = phase_ / frequency;
          phase_ += 1.0f;
          this_sample += stmlib::ThisBlepSample(t);
          next_sample += stmlib::NextBlepSample(t);
        }
        next_sample += phase_;

        if (shape == OSCILLATOR_SHAPE_SAW) {
          *out++ = 2.0f * this_sample - 1.0f;
        } else {
          lp_state_ += 0.25f * ((hp_state_ - this_sample) - lp_state_);
          *out++ = 4.0f * lp_state_;
          hp_state_ = this_sample;
        }
      } else if (shape <= OSCILLATOR_SHAPE_SLOPE) {
        float slope_up = 2.0f;
        float slope_down = 2.0f;
        if (shape == OSCILLATOR_SHAPE_SLOPE) {
          slope_up = 1.0f / (pw);
          slope_down = 1.0f / (1.0f - pw);
        }
        if (high_ ^ (phase_ < pw)) {
          float t = (phase_ - pw) / frequency;
          float discontinuity = (slope_up + slope_down) * frequency;
          if (through_zero_fm && frequency < 0.0f) {
            discontinuity = -discontinuity;
          }
          this_sample -= stmlib::ThisIntegratedBlepSample(t) * discontinuity;
          next_sample -= stmlib::NextIntegratedBlepSample(t) * discontinuity;
          high_ = phase_ < pw;
        }
        if (phase_ >= 1.0f) {
          phase_ -= 1.0f;
          float t = phase_ / frequency;
          float discontinuity = (slope_up + slope_down) * frequency;
          this_sample += stmlib::ThisIntegratedBlepSample(t) * discontinuity;
          next_sample += stmlib::NextIntegratedBlepSample(t) * discontinuity;
          high_ = true;
        } else if (through_zero_fm && phase_ < 0.0f) {
          float t = phase_ / frequency;
          phase_ += 1.0f;
          float discontinuity = (slope_up + slope_down) * frequency;
          this_sample -= stmlib::ThisIntegratedBlepSample(t) * discontinuity;
          next_sample -= stmlib::NextIntegratedBlepSample(t) * discontinuity;
          high_ = false;
        }
        next_sample += high_
          ? phase_ * slope_up
          : 1.0f - (phase_ - pw) * slope_down;
        *out++ = 2.0f * this_sample - 1.0f;
      } else {
        if (high_ ^ (phase_ >= pw)) {
          float t = (phase_ - pw) / frequency;
          float discontinuity = 1.0f;
          if (through_zero_fm && frequency < 0.0f) {
            discontinuity = -discontinuity;
          }
          this_sample += stmlib::ThisBlepSample(t) * discontinuity;
          next_sample += stmlib::NextBlepSample(t) * discontinuity;
          high_ = phase_ >= pw;
        }
        if (phase_ >= 1.0f) {
          phase_ -= 1.0f;
          float t = phase_ / frequency;
          this_sample -= stmlib::ThisBlepSample(t);
          next_sample -= stmlib::NextBlepSample(t);
          high_ = false;
        } else if (through_zero_fm && phase_ < 0.0f) {
          float t = phase_ / frequency;
          phase_ += 1.0f;
          this_sample += stmlib::ThisBlepSample(t);
          next_sample += stmlib::NextBlepSample(t);
          high_ = true;
        }
        next_sample += phase_ < pw ? 0.0f : 1.0f;
        
        if (shape == OSCILLATOR_SHAPE_SQUARE_TRIANGLE) {
          const float integrator_coefficient = frequency * 0.0625f;
          this_sample = 128.0f * (this_sample - 0.5f);
          lp_state_ += integrator_coefficient * (this_sample - lp_state_);
          *out++ = lp_state_;
        } else if (shape == OSCILLATOR_SHAPE_SQUARE_DARK) {
          const float integrator_coefficient = frequency * 2.0f;
          this_sample = 4.0f * (this_sample - 0.5f);
          lp_state_ += integrator_coefficient * (this_sample - lp_state_);
          *out++ = lp_state_;
        } else if (shape == OSCILLATOR_SHAPE_SQUARE_BRIGHT) {
          const float integrator_coefficient = frequency * 2.0f;
          this_sample = 2.0f * this_sample - 1.0f;
          lp_state_ += integrator_coefficient * (this_sample - lp_state_);
          *out++ = (this_sample - lp_state_) * 0.5f;
        } else {
          this_sample = 2.0f * this_sample - 1.0f;
          *out++ = this_sample;
        }
      }
    }
    next_sample_ = next_sample;
  }
  
 private:
  // Oscillator state.
  float phase_;
  float next_sample_;
  float lp_state_;
  float hp_state_;
  bool high_;

  // For interpolation of parameters.
  float frequency_;
  float pw_;
  
  DISALLOW_COPY_AND_ASSIGN(Oscillator);
};

}  // namespace plaits

#endif  // PLAITS_DSP_OSCILLATOR_OSCILLATOR_H_
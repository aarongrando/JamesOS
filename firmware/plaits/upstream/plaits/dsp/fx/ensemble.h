// Copyright 2014 Emilie Gillet.
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
// Ensemble FX.

#ifndef PLAITS_DSP_FX_ENSEMBLE_H_
#define PLAITS_DSP_FX_ENSEMBLE_H_

#include "stmlib/stmlib.h"

#include "stmlib/dsp/dsp.h"

#include "plaits/dsp/oscillator/sine_oscillator.h"
#include "plaits/dsp/fx/fx_engine.h"
#include "plaits/resources.h"

namespace plaits {

class Ensemble {
 public:
  typedef FxEngine<1024, FORMAT_32_BIT> E;
  
  Ensemble() { }
  ~Ensemble() { }
  
  void Init(E::T* buffer) {
    engine_.Init(buffer);
    phase_1_ = 0;
    phase_2_ = 0;
  }
  
  void Reset() {
    engine_.Clear();
  }
  
  void Process(float* left, float* right, size_t size) {
    typedef E::Reserve<511, E::Reserve<511> > Memory;
    E::DelayLine<Memory, 0> line_l;
    E::DelayLine<Memory, 1> line_r;
    E::Context c;
    
    const float dry_amount = 1.0f - amount_ * 0.5f;
    const float a = depth_ * 160.0f;
    const float b = depth_ * 16.0f;
    while (size) {
      phase_1_ += kSlowStep;
      phase_2_ += kFastStep;
      // Keep fast lookups first to preserve the stock relaxed-FP offset grouping.
      float fast_0 = SineRaw(phase_2_);
      float fast_120 = SineRaw(phase_2_ + kOneThird);
      float fast_240 = SineRaw(phase_2_ + kTwoThirds);
      float slow_0 = SineRaw(phase_1_);
      float slow_120 = SineRaw(phase_1_ + kOneThird);
      float slow_240 = SineRaw(phase_1_ + kTwoThirds);
      
      float mod_1 = slow_0 * a + fast_0 * b;
      float mod_2 = slow_120 * a + fast_120 * b;
      float mod_3 = slow_240 * a + fast_240 * b;
    
      // SineRaw reads a single table cell. Hold the three offsets for exactly
      // as long as all six lookups select their current cells.
      const size_t run = std::min(size, static_cast<size_t>(1 + std::min(
          FramesBeforeCellChange<kSlowStep>(phase_1_),
          FramesBeforeCellChange<kFastStep>(phase_2_))));
      size -= run;
      phase_1_ += (run - 1) * kSlowStep;
      phase_2_ += (run - 1) * kFastStep;
      for (size_t i = 0; i < run; ++i) {
        engine_.Start(&c);
        float wet = 0.0f;

        // Sum L & R channel to send to chorus line.
        c.Read(*left, 1.0f);
        c.Write(line_l, 0.0f);
        c.Read(*right, 1.0f);
        c.Write(line_r, 0.0f);

        c.Interpolate(line_l, mod_1 + 192, 0.33f);
        c.Interpolate(line_l, mod_2 + 192, 0.33f);
        c.Interpolate(line_r, mod_3 + 192, 0.33f);
        c.Write(wet, 0.0f);
        *left = wet * amount_ + *left * dry_amount;

        c.Interpolate(line_r, mod_1 + 192, 0.33f);
        c.Interpolate(line_r, mod_2 + 192, 0.33f);
        c.Interpolate(line_l, mod_3 + 192, 0.33f);
        c.Write(wet, 0.0f);
        *right = wet * amount_ + *right * dry_amount;
        left++;
        right++;
      }
    }
  }
  
  inline void set_amount(float amount) {
    amount_ = amount;
  }
  
  inline void set_depth(float depth) {
    depth_ = depth;
  }
  
 private:
  static constexpr uint32_t kOneThird = 1417339207u;
  static constexpr uint32_t kTwoThirds = 2834678415u;
  static constexpr uint32_t kSlowStep = 67289u;
  static constexpr uint32_t kFastStep = 589980u;

  template<uint32_t step>
  static inline uint32_t FramesBeforeCellChange(uint32_t phase) {
    const uint32_t cell = 1u << (32 - kSineLUTBits);
    const uint32_t mask = cell - 1;
    const uint32_t first = cell - (kOneThird & mask);
    const uint32_t second = cell - (kTwoThirds & mask);
    static_assert(first < second, "ensemble phase offsets must be in cell order");
    const uint32_t fraction = phase & mask;
    const uint32_t end = fraction < first ? first
        : (fraction < second ? second : cell);
    return (end - 1 - fraction) / step;
  }

  E engine_;
  
  float amount_;
  float depth_;
  
  uint32_t phase_1_;
  uint32_t phase_2_;
  
  DISALLOW_COPY_AND_ASSIGN(Ensemble);
};

}  // namespace plaits

#endif  // PLAITS_DSP_FX_ENSEMBLE_H_
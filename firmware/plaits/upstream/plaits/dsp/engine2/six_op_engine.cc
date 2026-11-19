// Copyright 2021 Emilie Gillet.
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
// 6-operator FM synth.

#include "plaits/dsp/engine2/six_op_engine.h"

#include <algorithm>

#include "plaits/resources.h"

namespace plaits {

using namespace fm;
using namespace std;
using namespace stmlib;

PLAITS_COLD_INIT_ATTR void FMVoice::Init(fm::Algorithms<6>* algorithms, float sample_rate) {
  voice_.Init(algorithms, sample_rate);
  lfo_.Init(sample_rate);
  
  parameters_.sustain = false;
  parameters_.gate = false;
  parameters_.note = 48.0f;
  parameters_.velocity = 0.5f;
  parameters_.brightness = 0.5f;
  parameters_.envelope_control = 0.5f;
  parameters_.pitch_mod = 0.0f;
  parameters_.amp_mod = 0.0f;
  
  patch_ = NULL;
}

void FMVoice::Render(float* buffer, size_t size) {
  if (!patch_) {
    return;
  }
  voice_.Render(parameters_, buffer, size);
}

void FMVoice::LoadPatch(const fm::Patch* patch) {
  if (patch == patch_) {
    return;
  }
  patch_ = patch;
  voice_.SetPatch(patch_);
  lfo_.Set(patch_->modulations);
}

const int kNumPatchesPerBank = 32;

PLAITS_COLD_INIT_ATTR void SixOpEngine::Init(BufferAllocator* allocator) {
  patch_index_quantizer_.Init(32, 0.005f, false);

  algorithms_.Init();
  for (int i = 0; i < kNumSixOpVoices; ++i) {
    voice_[i].Init(&algorithms_, kCorrectedSampleRate);
  }
  temp_buffer_ = allocator->Allocate<float>(kMaxBlockSize * 4);
  acc_buffer_ = allocator->Allocate<float>(kMaxBlockSize * kNumSixOpVoices);
  patches_ = allocator->Allocate<fm::Patch>(kNumPatchesPerBank);
  
  active_voice_ = kNumSixOpVoices - 1;
  rendered_voice_ = 0;
}

PLAITS_COLD_INIT_ATTR void SixOpEngine::Reset() {
  
}

void SixOpEngine::LoadUserData(const uint8_t* user_data) {
  for (int i = 0; i < kNumPatchesPerBank; ++i) {
    patches_[i].Unpack(user_data + i * fm::Patch::SYX_SIZE);
  }
  for (int i = 0; i < kNumSixOpVoices; ++i) {
    voice_[i].UnloadPatch();
  }
}

// Twelve frames is the normal block size for all three FM banks. Copy two
// floats at a time without introducing alignment or aliasing requirements.
static __attribute__((always_inline)) inline void PrepareFour(
    float* samples, const float* accumulated) {
  __builtin_memcpy(samples, accumulated, 2 * sizeof(float));
  __builtin_memcpy(samples + 2, accumulated + 2, 2 * sizeof(float));
  samples[12] = 0.0f;
  samples[13] = 0.0f;
  samples[14] = 0.0f;
  samples[15] = 0.0f;
}

void SixOpEngine::Render(
    const EngineParameters& parameters,
    float* out,
    float* aux,
    size_t size,
    bool* already_enveloped) {
#if FM1_PLAITS_TRIGGER_ALWAYS_PATCHED
  const bool trigger_unpatched = false;  // Unreachable here; see plaits_config.h.
#else
  const bool trigger_unpatched = parameters.trigger & TRIGGER_UNPATCHED;
#endif
  int patch_index = patch_index_quantizer_.Process(
      parameters.harmonics * 1.02f);
  
  if (trigger_unpatched) {
    const float t = parameters.morph;
    voice_[0].mutable_lfo()->Scrub(2.0f * kCorrectedSampleRate * t);

    for (int i = 0; i < kNumSixOpVoices; ++i) {
      voice_[i].LoadPatch(&patches_[patch_index]);
      Voice<6>::Parameters* p = voice_[i].mutable_parameters();
      p->sustain = i == 0 ? true : false;
      p->gate = false;
      p->note = parameters.note;
      p->velocity = parameters.accent;
      p->brightness = parameters.timbre;
      p->envelope_control = t;
      voice_[i].set_modulations(voice_[0].lfo());
    }
  } else {
    if (parameters.trigger & TRIGGER_RISING_EDGE) {
      active_voice_ = 1 - active_voice_;
      voice_[active_voice_].LoadPatch(&patches_[patch_index]);
      voice_[active_voice_].mutable_lfo()->Reset();
    }
    static_assert(kNumSixOpVoices == 2, "staggered pair");
    FMVoice& active = voice_[active_voice_];
    FMVoice& other = voice_[1 - active_voice_];
    Voice<6>::Parameters* p = active.mutable_parameters();
    p->note = parameters.note;
    p->velocity = parameters.accent;
    p->envelope_control = parameters.morph;
    p->brightness = parameters.timbre;
    p->sustain = false;
    p->gate = parameters.trigger & TRIGGER_HIGH;
    active.mutable_lfo()->Step(float(size));
    active.set_modulations(active.lfo());

    Voice<6>::Parameters* q = other.mutable_parameters();
    q->brightness = parameters.timbre;
    q->sustain = false;
    q->gate = false;
    if (other.patch() != active.patch()) {
      other.mutable_lfo()->Step(float(size));
      other.set_modulations(other.lfo());
    } else {
      q->pitch_mod = p->pitch_mod;
      q->amp_mod = p->amp_mod;
    }
  }

  float* const temp = temp_buffer_;
  float* const tail = temp + size;
  float* const acc = acc_buffer_;
  if (size == 12) {
    PrepareFour(temp, acc);
    PrepareFour(temp + 4, acc + 4);
    PrepareFour(temp + 8, acc + 8);
  } else {
    for (size_t i = 0; i < size; ++i) {
      temp[i] = acc[i];
      tail[i] = 0.0f;
    }
  }
  rendered_voice_ = 1 - rendered_voice_;
  voice_[rendered_voice_].Render(temp, 2 * size);
  for (size_t i = 0; i < size; ++i) {
    aux[i] = out[i] = SoftClip(temp[i] * 0.25f);
    acc[i] = tail[i];
  }
}

}  // namespace plaits

/* SPDX-License-Identifier: GPL-3.0-only
 * One build of the Plaits hi-hat engine behind a C interface, for
 * tests/test_hihat_exact.cc. Compiled twice into one test binary: once as the
 * tree builds it (HIHAT_HARNESS_PREFIX=cur) and once as the reference
 * (HIHAT_HARNESS_PREFIX=ref, -Dplaits=plaits_ref so the two sets of inline
 * engine code cannot be merged by the linker, and the switches under test set
 * to 0). The engine source is included here so it follows the same rename. */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define private public
#include "plaits/dsp/engine/hi_hat_engine.cc"
#undef private

#include "stmlib/utils/buffer_allocator.h"
#include "stmlib/utils/random.h"

#define HIHAT_CAT2(a, b) a##_##b
#define HIHAT_CAT(a, b) HIHAT_CAT2(a, b)
#define HIHAT_FN(name) HIHAT_CAT(HIHAT_HARNESS_PREFIX, name)

namespace {

plaits::HiHatEngine g_engine;
uint8_t g_scratch[1024];
uint32_t g_rng;

template <typename T>
void Put(uint8_t*& p, const T& v) {
  std::memcpy(p, &v, sizeof v);
  p += sizeof v;
}

void PutSvf(uint8_t*& p, const plaits_stmlib::Svf& f) {
  Put(p, f.g_); Put(p, f.r_); Put(p, f.h_);
  Put(p, f.state_1_); Put(p, f.state_2_);
}

template <typename Hat>
void PutHat(uint8_t*& p, const Hat& h) {
  Put(p, h.envelope_); Put(p, h.noise_clock_); Put(p, h.noise_sample_);
  Put(p, h.sustain_gain_);
  PutSvf(p, h.noise_coloration_svf_);
  PutSvf(p, h.hpf_);
}

}  // namespace

extern "C" {

void HIHAT_FN(init)(uint32_t seed) {
  plaits_stmlib::BufferAllocator allocator(g_scratch, sizeof g_scratch);
  g_engine.Init(&allocator);
  g_rng = seed;
}

/* Re-initialise the engine as the kit does on every trigger; the lane's
 * random state carries on. */
void HIHAT_FN(reinit)(void) {
  plaits_stmlib::BufferAllocator allocator(g_scratch, sizeof g_scratch);
  g_engine.Init(&allocator);
}

void HIHAT_FN(render)(float note, float timbre, float morph, float harmonics,
                      float accent, int trigger, float* out, float* aux,
                      size_t n) {
  plaits::EngineParameters p = {};
  p.note = note;
  p.timbre = timbre;
  p.morph = morph;
  p.harmonics = harmonics;
  p.accent = accent;
  p.trigger = trigger ? plaits::TRIGGER_RISING_EDGE : plaits::TRIGGER_LOW;
  bool already_enveloped = true;
  const uint32_t saved = plaits_stmlib::Random::state();
  plaits_stmlib::Random::Seed(g_rng);
  g_engine.Render(p, out, aux, n, &already_enveloped);
  g_rng = plaits_stmlib::Random::state();
  plaits_stmlib::Random::Seed(saved);
}

/* Every state variable that can influence later output, in a fixed order
 * (coefficient-cache keys are derived values and are left out). */
size_t HIHAT_FN(state)(uint8_t* dst) {
  uint8_t* p = dst;
  Put(p, g_rng);
  PutHat(p, g_engine.hi_hat_1_);
  for (int i = 0; i < 6; ++i) Put(p, g_engine.hi_hat_1_.metallic_noise_.phase_[i]);
  PutHat(p, g_engine.hi_hat_2_);
  for (int i = 0; i < 6; ++i) {
    const plaits::Oscillator& o = g_engine.hi_hat_2_.metallic_noise_.oscillator_[i];
    Put(p, o.phase_); Put(p, o.next_sample_); Put(p, o.lp_state_);
    Put(p, o.hp_state_); Put(p, o.frequency_); Put(p, o.pw_);
    const uint8_t high = o.high_ ? 1u : 0u;
    Put(p, high);
  }
  return static_cast<size_t>(p - dst);
}

#if defined(FM1_PLAITS_HIHAT_STATS) && FM1_PLAITS_HIHAT_STATS
const uint32_t* HIHAT_FN(stats)(void) { return plaits::g_hihat_stats; }
#endif

}  // extern "C"

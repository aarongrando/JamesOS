/* SPDX-License-Identifier: GPL-3.0-only
 * Host benchmark of the Plaits sound engines as this firmware drives them:
 * ns per 256-frame block for the three kit drum engines (24-frame chunks, the
 * kick through its kit entry), their sub-models on their own, and all 24
 * synth models through the C API. A tool, not a test: host times are relative
 * only (build and run with tests/bench_plaits_engines.py). */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#if defined(__SSE2__)
#include <xmmintrin.h>
#endif
#include "plaits_c_api.h"
#include "plaits/dsp/engine/bass_drum_engine.h"
#include "plaits/dsp/engine/hi_hat_engine.h"
#include "plaits/dsp/engine/snare_drum_engine.h"
#include "plaits/wavetable_pack.h"
#include "stmlib/utils/buffer_allocator.h"
#include "stmlib/utils/random.h"

namespace {

volatile float g_sink;
double Now() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}

const int kBlocks = 2000;      /* 256-frame blocks per run: 11.6 s of audio */
const int kRetrigBlocks = 43;  /* a hit every quarter second */
const int kReps = 7;

struct P { float note, timbre, morph, harmonics, accent; };

template <typename F>
double Best(F f) {
  double best = 1e30;
  for (int r = 0; r < kReps; ++r) {
    const double t0 = Now();
    f();
    const double ns = (Now() - t0) / kBlocks;
    if (ns < best) best = ns;
  }
  return best;
}

/* kind 0 bass (kit entry), 1 snare, 2 hat */
double BenchEngine(int kind, const P& q) {
  static uint8_t scratch[16384];
  static plaits::BassDrumEngine bass;
  static plaits::SnareDrumEngine snare;
  static plaits::HiHatEngine hat;
  return Best([&] {
    plaits_stmlib::BufferAllocator allocator(scratch, sizeof scratch);
    plaits::Engine* e = kind == 0 ? static_cast<plaits::Engine*>(&bass) :
        kind == 1 ? static_cast<plaits::Engine*>(&snare) :
        static_cast<plaits::Engine*>(&hat);
    e->Init(&allocator);
    plaits_stmlib::Random::Seed(0x1234567u);
    plaits::EngineParameters p = {};
    p.note = q.note; p.timbre = q.timbre; p.morph = q.morph;
    p.harmonics = q.harmonics; p.accent = q.accent;
    float out[24], aux[24], acc = 0.0f;
    for (int b = 0; b < kBlocks; ++b) {
      for (int done = 0; done < 256; ) {
        const int n = 256 - done < 24 ? 256 - done : 24;
        p.trigger = (done == 0 && b % kRetrigBlocks == 0)
            ? plaits::TRIGGER_RISING_EDGE : plaits::TRIGGER_LOW;
        bool env = true;
        if (kind == 0) bass.RenderKitRate2(p, out, aux, n, &env);
        else e->Render(p, out, aux, n, &env);
        acc += out[n - 1] + aux[n - 1];
        done += n;
      }
    }
    g_sink = acc;
  });
}

/* Sub-models, called as their engine calls them. */
enum Sub { ABD, SBD_RATE2, OVERDRIVE, ASD, SSD, HH1, HH2, SUB_COUNT };
const char* const kSubName[] = {"analog_bass_drum", "synthetic_bass_drum(rate2)",
    "kick overdrive", "analog_snare_drum", "synthetic_snare_drum",
    "hi_hat_1 (808 square)", "hi_hat_2 (ringmod)"};

double BenchSub(Sub sub, const P& q) {
  static plaits::AnalogBassDrum abd;
  static plaits::SyntheticBassDrum sbd;
  static plaits::Overdrive od;
  static plaits::AnalogSnareDrum asd;
  static plaits::SyntheticSnareDrum ssd;
  static plaits::HiHat<plaits::SquareNoise, plaits::SwingVCA, true, false> hh1;
  static plaits::HiHat<plaits::RingModNoise, plaits::LinearVCA, false, true> hh2;
  return Best([&] {
    abd.Init(); sbd.Init(); od.Init(); asd.Init(); ssd.Init(); hh1.Init(); hh2.Init();
    uint32_t rng = 0x1234567u;
    const float f0 = plaits::NoteToFrequency(q.note);
    plaits::HiHatControlCoefficients c;
    c.envelope_decay = 1.0f - 0.003f * plaits_stmlib::SemitonesToRatio(-q.morph * 84.0f);
    c.cut_decay = 1.0f - 0.0025f * plaits_stmlib::SemitonesToRatio(-q.morph * 36.0f);
    c.cutoff = 150.0f / plaits::kSampleRate * plaits_stmlib::SemitonesToRatio(q.timbre * 72.0f);
    if (c.cutoff > 16000.0f / plaits::kSampleRate) c.cutoff = 16000.0f / plaits::kSampleRate;
    const float h = q.harmonics;
    float out[24], tmp[48], acc = 0.0f;
    for (int i = 0; i < 24; ++i) out[i] = 0.0f;
    for (int b = 0; b < kBlocks; ++b) {
      for (int done = 0; done < 256; ) {
        const int n = 256 - done < 24 ? 256 - done : 24;
        const bool trig = done == 0 && b % kRetrigBlocks == 0;
        switch (sub) {
          case ABD:
            abd.Render(false, trig, q.accent, f0, q.timbre, q.morph,
                       h * 4.0f < 1.0f ? h * 4.0f : 1.0f,
                       h * 4.0f - 1.0f < 0.0f ? 0.0f : (h * 4.0f - 1.0f > 1.0f ? 1.0f : h * 4.0f - 1.0f),
                       out, n);
            break;
          case SBD_RATE2:
            if (n & 1) sbd.Render(false, trig, q.accent, f0, q.timbre, q.morph,
                0.4f - 0.25f * q.morph * q.morph, h * 2.0f < 1.0f ? h * 2.0f : 1.0f,
                h * 2.0f - 1.0f > 0.0f ? h * 2.0f - 1.0f : 0.0f, out, n, &rng);
            else sbd.RenderRate2<true>(false, trig, q.accent, f0, q.timbre, q.morph,
                0.4f - 0.25f * q.morph * q.morph, h * 2.0f < 1.0f ? h * 2.0f : 1.0f,
                h * 2.0f - 1.0f > 0.0f ? h * 2.0f - 1.0f : 0.0f, out, n, &rng);
            break;
          case OVERDRIVE:
            for (int i = 0; i < n; ++i) out[i] = 0.3f - 0.02f * i;
            od.Process(0.5f + 0.5f * (h * 2.0f - 1.0f > 0.0f ? h * 2.0f - 1.0f : 0.0f), out, n);
            break;
          case ASD:
            asd.Render(false, trig, q.accent, f0, q.timbre, q.morph, h, out, n, &rng);
            break;
          case SSD:
            ssd.Render(false, trig, q.accent, f0, q.timbre, q.morph, h, out, n, &rng);
            break;
          case HH1:
            hh1.Render(false, trig, q.accent, f0, q.timbre, q.morph, c, h, tmp, tmp + n, out, n);
            break;
          case HH2:
            hh2.Render(false, trig, q.accent, f0, q.timbre, q.morph, c, h, tmp, tmp + n, out, n);
            break;
          default: break;
        }
        acc += out[n - 1];
        done += n;
      }
    }
    g_sink = acc;
  });
}

double BenchSynth(uint32_t model) {
  alignas(8) static uint8_t state[PLAITS_STATE_BYTES];
  return Best([&] {
    plaits_state_init(state, 0x2468aceu);
    int16_t out[256];
    int32_t acc = 0;
    for (int b = 0; b < kBlocks; ++b) {
      if (b % kRetrigBlocks == 0)
        plaits_note_on(state, model, (48 + (b / kRetrigBlocks) % 24) * 128,
                       16384, 16384, 16384, 100);
      if (b % kRetrigBlocks == 30) plaits_note_off(state, 128);
      plaits_render(state, model, (48 + (b / kRetrigBlocks) % 24) * 128,
                    16384, 16384, 16384, 8000, out, 256);
      acc += out[255];
    }
    g_sink = static_cast<float>(acc);
  });
}

}  // namespace

int main(int argc, char** argv) {
  bool synths = false, flush = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--synths")) synths = true;
    if (!std::strcmp(argv[i], "--flush-denormals")) flush = true;
  }
  /* x86 hosts take a large microcode penalty on subnormal operands, which
   * the drum tails reach; --flush-denormals (FTZ+DAZ) shows the cost without
   * it. That mode changes the arithmetic: timing only, never for goldens. */
#if defined(__SSE2__)
  if (flush) _mm_setcsr(_mm_getcsr() | 0x8040u);
#endif
  std::printf("subnormals: %s\n", flush ? "flushed (FTZ+DAZ)" : "IEEE (host default)");
  const float u64 = 64.0f / 127.0f;
  /* Kit notes with the firmware's lane offsets; mid knobs as the kit maps them. */
  const P kit[] = {
      {36.0f, u64, u64, u64, 100.0f / 127.0f},        /* KCK1 */
      {38.0f, u64, u64, u64, 100.0f / 127.0f},        /* SNAR */
      {42.0f, u64, u64, u64, 100.0f / 127.0f},        /* CLOS: 46 - 4 */
      {46.0f, u64, 0.85f, u64, 100.0f / 127.0f},      /* OPEN, long decay */
  };
  std::printf("%-34s %10s\n", "engine (kit, 24-frame chunks)", "ns/block");
  std::printf("%-34s %10.0f\n", "bass_drum (KitRate2), note 36", BenchEngine(0, kit[0]));
  std::printf("%-34s %10.0f\n", "snare_drum, note 38", BenchEngine(1, kit[1]));
  std::printf("%-34s %10.0f\n", "hi_hat, note 42", BenchEngine(2, kit[2]));
  std::printf("%-34s %10.0f\n", "hi_hat, note 46 long", BenchEngine(2, kit[3]));
  std::printf("\n%-34s %10s\n", "sub-model", "ns/block");
  const int owner[SUB_COUNT] = {0, 0, 0, 1, 1, 2, 2};
  for (int s = 0; s < SUB_COUNT; ++s)
    std::printf("%-34s %10.0f\n", kSubName[s], BenchSub(static_cast<Sub>(s), kit[owner[s]]));
  if (synths) {
#if FM1_PLAITS_PACK_WAVES
    if (!fm1_plaits_waves_init(nullptr)) return 1;
#endif
    std::printf("\n%-34s %10s\n", "synth model (C API)", "ns/block");
    for (uint32_t m = 0; m < PLAITS_MODEL_COUNT; ++m) {
      char name[32];
      std::snprintf(name, sizeof name, "model %u", m);
      std::printf("%-34s %10.0f\n", name, BenchSynth(m));
    }
  }
  return 0;
}

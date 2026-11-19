/* SPDX-License-Identifier: GPL-3.0-only
 * Proof by exhaustion-in-practice that the hi-hat engine's exact shortcuts
 * change nothing: the engine as the tree builds it ("cur") and the reference
 * build with the shortcuts compiled out ("ref") are driven side by side
 * (tests/hihat_exact_harness.cc, twice in this binary) and every output
 * float and every state variable is compared byte for byte after every
 * render call. Sweeps: every control over its full range, notes across the
 * keyboard, two accents, kit chunking (24-frame slices, engine re-initialised
 * on each hit as the kit does) with retriggers in mid-decay, synth-track use
 * (odd slice sizes, no re-initialisation, controls and pitch moved while
 * sounding, fractional notes), and minutes-long tails that run every decay
 * down to its floating-point floor. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" {
void cur_init(uint32_t seed);
void cur_reinit(void);
void cur_render(float, float, float, float, float, int, float*, float*, size_t);
size_t cur_state(uint8_t*);
const uint32_t* cur_stats(void);
void ref_init(uint32_t seed);
void ref_reinit(void);
void ref_render(float, float, float, float, float, int, float*, float*, size_t);
size_t ref_state(uint8_t*);
}

namespace {

struct P { float note, timbre, morph, harmonics, accent; };

uint64_t g_calls, g_frames, g_output_bytes, g_state_bytes, g_cases;
uint64_t g_nonzero_frames;
int g_failures;
/* --measure: do not stop a case at its first mismatch; report the largest
 * output difference in 16-bit output LSBs (float 1.0 is 32767 at full lane
 * gain). For builds where bit-identity is not expected (see the runner). */
bool g_measure;
uint64_t g_mismatching_calls;
double g_max_lsb;
uint64_t g_max_case, g_max_frame;

bool Step(const char* what, uint64_t id, const P& p, int trigger, size_t n,
          uint64_t frame) {
  float out_c[24], aux_c[24], out_r[24], aux_r[24];
  uint8_t st_c[512], st_r[512];
  cur_render(p.note, p.timbre, p.morph, p.harmonics, p.accent, trigger, out_c, aux_c, n);
  ref_render(p.note, p.timbre, p.morph, p.harmonics, p.accent, trigger, out_r, aux_r, n);
  const size_t sc = cur_state(st_c);
  const size_t sr = ref_state(st_r);
  ++g_calls;
  g_frames += n;
  g_output_bytes += 2u * n * sizeof(float);
  g_state_bytes += sc;
  for (size_t i = 0; i < n; ++i)
    if (out_r[i] != 0.0f || aux_r[i] != 0.0f) ++g_nonzero_frames;
  const bool same = sc == sr && !std::memcmp(out_c, out_r, n * sizeof(float)) &&
      !std::memcmp(aux_c, aux_r, n * sizeof(float)) && !std::memcmp(st_c, st_r, sc);
  if (same) return true;
  ++g_mismatching_calls;
  for (size_t i = 0; i < n; ++i) {
    const double d_out = 32767.0 * (static_cast<double>(out_c[i]) - out_r[i]);
    const double d_aux = 32767.0 * (static_cast<double>(aux_c[i]) - aux_r[i]);
    const double d = (d_out < 0 ? -d_out : d_out) > (d_aux < 0 ? -d_aux : d_aux)
        ? (d_out < 0 ? -d_out : d_out) : (d_aux < 0 ? -d_aux : d_aux);
    if (d > g_max_lsb) { g_max_lsb = d; g_max_case = id; g_max_frame = frame + i; }
  }
  if (g_measure) return true;
  if (g_failures++ < 5) {
    std::printf("MISMATCH %s case %llu at frame %llu (slice of %zu): note %.3f timbre %.3f "
                "morph %.3f harmonics %.3f accent %.3f\n", what,
                static_cast<unsigned long long>(id), static_cast<unsigned long long>(frame),
                n, p.note, p.timbre, p.morph, p.harmonics, p.accent);
    for (size_t i = 0; i < n; ++i) {
      if (std::memcmp(&out_c[i], &out_r[i], 4) || std::memcmp(&aux_c[i], &aux_r[i], 4)) {
        std::printf("  first differing frame %zu: out %.9g vs %.9g, aux %.9g vs %.9g\n", i,
                    out_c[i], out_r[i], aux_c[i], aux_r[i]);
        break;
      }
    }
    for (size_t i = 0; i < sc && i < sr; ++i)
      if (st_c[i] != st_r[i]) { std::printf("  first differing state byte %zu\n", i); break; }
  }
  return false;
}

/* The kit: 256-frame blocks cut into 24-frame slices (the last is 16), the
 * engine re-initialised on every hit; a second hit lands in mid-decay. */
void RunKit(uint64_t id, const P& p, uint32_t frames, uint32_t retrigger_block) {
  cur_init(0x9e3779b9u + static_cast<uint32_t>(id));
  ref_init(0x9e3779b9u + static_cast<uint32_t>(id));
  ++g_cases;
  uint64_t frame = 0;
  for (uint32_t block = 0; frame < frames; ++block) {
    const bool hit = block == 0 || block == retrigger_block;
    if (hit && block) { cur_reinit(); ref_reinit(); }
    for (uint32_t done = 0; done < 256u; ) {
      const size_t n = 256u - done < 24u ? 256u - done : 24u;
      if (!Step("kit", id, p, hit && done == 0, n, frame)) return;
      done += n;
      frame += n;
    }
  }
}

/* A synth track: irregular slices, no re-initialisation, and the controls
 * and pitch moved while the hat is sounding. */
void RunSynth(uint64_t id, const P& a, const P& b, uint32_t frames) {
  static const size_t kSlices[] = {24, 24, 7, 24, 13, 1, 24, 2, 24, 17, 3, 24, 24, 5, 12};
  cur_init(0x51ed270bu + static_cast<uint32_t>(id));
  ref_init(0x51ed270bu + static_cast<uint32_t>(id));
  ++g_cases;
  uint64_t frame = 0;
  for (uint32_t call = 0; frame < frames; ++call) {
    const size_t n = kSlices[call % (sizeof kSlices / sizeof kSlices[0])];
    /* a, then b, then a glide back in steps, then a again. */
    const uint32_t phase = (call / 97u) % 4u;
    P p = phase == 0 ? a : phase == 1 ? b : a;
    if (phase == 2) {
      const float t = static_cast<float>(call % 97u) / 97.0f;
      p.note = b.note + (a.note - b.note) * t;
      p.timbre = b.timbre + (a.timbre - b.timbre) * t;
      p.morph = b.morph + (a.morph - b.morph) * t;
      p.harmonics = b.harmonics + (a.harmonics - b.harmonics) * t;
    }
    const int trigger = call == 0 || call % 211u == 150u;
    if (!Step("synth", id, p, trigger, n, frame)) return;
    frame += n;
  }
}

}  // namespace

int main(int argc, char** argv) {
  /* --quick: a thin slice of the sweep. --part I N: only the cases whose
   * number is I modulo N (the full sweep in N shorter runs). */
  bool quick = false;
  uint64_t part = 0, parts = 1;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--quick")) quick = true;
    if (!std::strcmp(argv[i], "--measure")) g_measure = true;
    if (!std::strcmp(argv[i], "--part") && i + 2 < argc) {
      part = std::strtoull(argv[i + 1], nullptr, 10);
      parts = std::strtoull(argv[i + 2], nullptr, 10);
      if (!parts || part >= parts) return 2;
    }
  }
  const float knobs[] = {0.0f, 0.25f, 64.0f / 127.0f, 0.75f, 1.0f};
  const float noisy[] = {0.0f, 0.3f, 0.7f, 1.0f};
  const float accents[] = {0.1f, 1.0f};
  uint64_t id = 0;
  /* Kit use: integer notes from far below to far above the hat lanes. */
  for (int note = 12; note <= 108; note += quick ? 24 : 6)
    for (float t : knobs) for (float m : knobs) for (float h : noisy) for (float a : accents) {
      const P p = {static_cast<float>(note), t, m, h, a};
      if (id % parts == part) RunKit(id, p, 2u * 44100u, 5u + static_cast<uint32_t>(id % 60u));
      ++id;
    }
  /* The kit's own hat notes (CLOS 42, OPEN 46, PEDL 58, CYMB 37) with tune. */
  for (int base : {42, 46, 58, 37}) for (int tune = -12; tune <= 12; tune += quick ? 12 : 1)
    for (float m : knobs) {
      const P p = {static_cast<float>(base + tune), 64.0f / 127.0f, m, 64.0f / 127.0f, 100.0f / 127.0f};
      if (id % parts == part) RunKit(id, p, 2u * 44100u, 9u + static_cast<uint32_t>(id % 40u));
      ++id;
    }
  /* Synth-track use, fractional notes, moving controls. */
  for (int i = 0; i < (quick ? 20 : 300); ++i) {
    uint32_t s = 0x1234567u + 2654435761u * static_cast<uint32_t>(i);
    float r[10];
    for (float& v : r) { s = s * 1664525u + 1013904223u; v = static_cast<float>(s >> 8) / 16777216.0f; }
    const P a = {8.0f + 104.0f * r[0], r[1], r[2], r[3], 0.05f + 0.95f * r[4]};
    const P b = {8.0f + 104.0f * r[5], r[6], r[7], r[8], a.accent};
    if (id % parts == part) RunSynth(id, a, b, 3u * 44100u);
    ++id;
  }
  /* Tails to the floor: one hit, then minutes of decay (longest and shortest
   * decay, and a mid one), kit slicing, at low, hat and high notes. */
  for (int note : {24, 42, 46, 84}) for (float m : {0.0f, 64.0f / 127.0f, 1.0f}) {
    const P p = {static_cast<float>(note), 64.0f / 127.0f, m, 64.0f / 127.0f, 1.0f};
    if (id % parts == part) RunKit(id, p, (quick ? 20u : 150u) * 44100u, 0xffffffffu);
    ++id;
  }
  const uint32_t* st = cur_stats();
  std::printf("cases %llu, render calls %llu, frames %llu (%llu with non-zero output), "
              "output bytes compared %llu, state bytes compared %llu\n",
              static_cast<unsigned long long>(g_cases), static_cast<unsigned long long>(g_calls),
              static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_nonzero_frames),
              static_cast<unsigned long long>(g_output_bytes), static_cast<unsigned long long>(g_state_bytes));
  std::printf("shortcut use: ring-mod slices %u, all three squares constant %u, constant square renders %u of %u, "
              "steady saw renders %u of %u\n", st[0], st[1], st[2], 3u * st[0], st[3], 3u * st[0]);
  if (g_measure) {
    std::printf("MEASURED: %llu of %llu render calls differ in output or state; largest output difference "
                "%.6f LSB (16-bit, full lane gain), case %llu frame %llu\n",
                static_cast<unsigned long long>(g_mismatching_calls), static_cast<unsigned long long>(g_calls),
                g_max_lsb, static_cast<unsigned long long>(g_max_case),
                static_cast<unsigned long long>(g_max_frame));
    return 0;
  }
  if (g_failures) {
    std::printf("FAIL: %d mismatching render calls\n", g_failures);
    return 1;
  }
  std::printf("PASS: outputs and state byte-identical\n");
  return 0;
}

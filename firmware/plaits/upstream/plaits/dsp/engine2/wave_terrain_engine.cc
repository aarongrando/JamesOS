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
// Wave terrain synthesis.

#include "plaits/dsp/engine2/wave_terrain_engine.h"

#include <cmath>
#include <algorithm>

#include "plaits/dsp/oscillator/wavetable_oscillator.h"

namespace plaits {

using namespace std;
using namespace stmlib;

PLAITS_COLD_INIT_ATTR void WaveTerrainEngine::Init(BufferAllocator* allocator) {
  path_.Init();
  offset_ = 0.0f;
  terrain_ = 0.0f;
  temp_buffer_ = allocator->Allocate<float>(kMaxBlockSize * 4);
  user_terrain_ = NULL;
}

PLAITS_COLD_INIT_ATTR void WaveTerrainEngine::Reset() {
  
}

inline float TerrainLookup(float x, float y, const int8_t* terrain) {
  const int terrain_size = 64;
  const float value_scale = 1.0f / 128.0f;
  const float coord_scale = float(terrain_size - 2) * 0.5f;

  x = (x + 1.0f) * coord_scale;
  y = (y + 1.0f) * coord_scale;

  MAKE_INTEGRAL_FRACTIONAL(x);
  MAKE_INTEGRAL_FRACTIONAL(y);
  
  float xy[2];
  
  terrain += y_integral * terrain_size;
  xy[0] = InterpolateWave(terrain, x_integral, x_fractional);
  terrain += terrain_size;
  xy[1] = InterpolateWave(terrain, x_integral, x_fractional);
  return (xy[0] + (xy[1] - xy[0]) * y_fractional) * value_scale;
}

template<typename T>
inline float InterpolateIntegratedWave(
    const T* table,
    int32_t index_integral,
    float index_fractional) {
  float a = static_cast<float>(table[index_integral]);
  float b = static_cast<float>(table[index_integral + 1]);
  float c = static_cast<float>(table[index_integral + 2]);
  float t = index_fractional;
  return (b - a) + (c - b - b + a) * t;
}

// The wavetables are stored in integrated form. Either we directly use the
// integrated data (which can have large variations in amplitude), or we
// differentiate it on the fly to recover the original waveform. This second
// option can be noisier, and it would ideally need a low pass-filter.

#define DIFFERENTIATE_WAVE_DATA

// Lookup from the wavetable data re-interpreted as a terrain. :facepalm:
inline float TerrainLookupWT(float x, float y, int bank) {
  const int table_size = 128;
  const int table_size_full = table_size + 4;  // Includes 4 wrapped samples
  const int num_waves = 64;
  const float sample = (y + 1.0f) * 0.5f * float(table_size);
  const float wt = (x + 1.0f) * 0.5f * float(num_waves - 1);
  
  const int16_t* waves = wav_integrated_waves + \
      bank * num_waves * table_size_full;
  
  MAKE_INTEGRAL_FRACTIONAL(sample);
  MAKE_INTEGRAL_FRACTIONAL(wt);
  
  float xy[2];
#ifdef DIFFERENTIATE_WAVE_DATA
  const float value_scale = 1.0f / 1024.0f;
  waves += wt_integral * table_size_full;
  xy[0] = InterpolateIntegratedWave(waves, sample_integral, sample_fractional);
  waves += table_size_full;
  xy[1] = InterpolateIntegratedWave(waves, sample_integral, sample_fractional);
#else
  const float value_scale = 1.0f / 32768.0f;
  waves += wt_integral * table_size_full;
  xy[0] = InterpolateWave(waves, sample_integral, sample_fractional);
  waves += table_size_full;
  xy[1] = InterpolateWave(waves, sample_integral, sample_fractional);
#endif  // DIFFERENTIATE_WAVE_DATA
  return (xy[0] + (xy[1] - xy[0]) * wt_fractional) * value_scale;
}

// With positive phase and a power-of-two table size, scaling before
// wrapping preserves the fractional cell position exactly.
inline float TerrainSine(float phase) {
  const float cell = phase * kSineLUTSize;
  MAKE_INTEGRAL_FRACTIONAL(cell);
  const int index = cell_integral & ((1 << kSineLUTBits) - 1);
  const float a = lut_sine[index];
  const float b = lut_sine[index + 1];
  return a + (b - a) * cell_fractional;
}

inline float Squash(float x, float a) {
  x *= a;
  return x / (1.0f + __builtin_fabsf(x));
}

namespace {

template<int terrain_index>
inline float Terrain(float x, float y, const int8_t* user_terrain) {
  // The Sine function only works for a positive argument.
  // Thus, all calls to Sine include a positive offset of the argument!
  const float k = 4.0f;
  switch (terrain_index) {
    case 0:
      {
        return (Squash(TerrainSine(k + x * 1.273f), 2.0f) - \
            TerrainSine(k + y * (x + 1.571f) * 0.637f)) * 0.57f;
      }
      break;
    case 1:
      {
        const float xy = x * y;
        return TerrainSine(k + TerrainSine(k + (x + y) * 0.637f) /
            (0.2f + xy * xy) * 0.159f);
      }
      break;
    case 2:
      {
        const float xy = x * y;
        return TerrainSine(k + TerrainSine(k + 2.387f * xy) /
            (0.350f + xy * xy) * 0.159f);
      }
      break;
    case 3:
      {
        // Preserve the rounded x*y before the relaxed compiler moves the scale.
        const volatile float xy = x * y;
        const float xys = (x - 0.25f) * (y + 0.25f);
        return TerrainSine(k + xy / (2.0f + __builtin_fabsf(5.0f * xys)) * 6.366f);
      }
      break;
    case 4:
      {
        return Sine(
          0.159f / (0.170f + __builtin_fabsf(y - 0.25f)) + \
          0.477f / (0.350f + __builtin_fabsf((x + 0.5f) * (y + 1.5f))) + k);
      }
      break;
    case 5:
    case 6:
    case 7:
      {
        return TerrainLookupWT(x, y, 2 - (terrain_index - 5));
      }
      break;
    case 8:
      {
        return TerrainLookup(x, y, user_terrain);
      }
    default:
      return 0.0f;
  }
}

struct TerrainValues {
  float first;
  float second;
};

template<int index>
TerrainValues EvaluateSurface(
    float x0, float y0, float x1, float y1, const int8_t* user_terrain) {
  TerrainValues samples = {
    Terrain<index>(x0, y0, user_terrain),
    Terrain<index>(x1, y1, user_terrain)
  };
  return samples;
}

typedef TerrainValues (*Surface)(float, float, float, float, const int8_t*);
const Surface surfaces[] = {
  EvaluateSurface<0>, EvaluateSurface<1>, EvaluateSurface<2>, EvaluateSurface<3>,
  EvaluateSurface<4>, EvaluateSurface<5>, EvaluateSurface<6>, EvaluateSurface<7>,
  EvaluateSurface<8>
};

inline Surface GetSurface(int index) {
  // Match Terrain's silent default for an out-of-range surface.
  if (static_cast<unsigned>(index) >= sizeof(surfaces) / sizeof(surfaces[0])) {
    return EvaluateSurface<-1>;
  }
  return surfaces[index];
}

}  // namespace

void WaveTerrainEngine::Render(
    const EngineParameters& parameters,
    float* out,
    float* aux,
    size_t size,
    bool* already_enveloped) {
  const size_t kOversampling = 2;
  const float kScale = 1.0f / float(kOversampling);

  float* path_x = &temp_buffer_[0];
  float* path_y = &temp_buffer_[kOversampling * size];
  
  const float f0 = NoteToFrequency(parameters.note);
  const float attenuation = max(1.0f - 8.0f * f0, 0.0f);
  const float radius = 0.1f + 0.9f * parameters.timbre * attenuation * \
      (2.0f - attenuation);

  // Use the "magic sine" algorithm to generate sin and cos functions for the
  // trajectory coordinates.
  path_.RenderQuadrature(
      f0 * kScale, radius, path_x, path_y, size * kOversampling);
  
  ParameterInterpolator offset(&offset_, 1.9f * parameters.morph - 1.0f, size);
  int num_terrains = user_terrain_ ? 9 : 8;
  ParameterInterpolator terrain(
      &terrain_,
      min(parameters.harmonics * 1.05f, 1.0f) * float(num_terrains - 1.0001f),
      size);
  
  size_t ij = 0;
  for (size_t i = 0; i < size; ++i) {
    const float x_offset = offset.Next();
    
    const float z = terrain.Next();
    MAKE_INTEGRAL_FRACTIONAL(z);

    const float x_scale = 1.0f - __builtin_fabsf(x_offset);
    const float x0 = path_x[ij] * x_scale + x_offset;
    const float x1 = path_x[ij + 1] * x_scale + x_offset;
    const float y0 = path_y[ij];
    const float y1 = path_y[ij + 1];
    ij += kOversampling;

    // The two oversamples share the same pair of neighboring surfaces.
    const TerrainValues low =
        GetSurface(z_integral)(x0, y0, x1, y1, user_terrain_);
    const TerrainValues high =
        GetSurface(z_integral + 1)(x0, y0, x1, y1, user_terrain_);
    // Keep the native per-oversample accumulation order under relaxed FP.
    const float lower[] = { low.first, low.second };
    const float upper[] = { high.first, high.second };
    const float ys[] = { y0, y1 };
    float out_s = 0.0f;
    float aux_s = 0.0f;
    for (size_t j = 0; j < kOversampling; ++j) {
      const float z = lower[j] + (upper[j] - lower[j]) * z_fractional;
      out_s += z;
      aux_s += ys[j] + z;
    }
    out[i] = kScale * out_s;
    aux[i] = Sine(1.0f + 0.5f * kScale * aux_s);
  }
}

}  // namespace plaits

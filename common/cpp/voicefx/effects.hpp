/* ============================================================================
 * effects.hpp — DSP primitives for VoiceFX (internal, not part of the ABI).
 *
 * Each effect is a small class with:
 *   prepare(sampleRate, maxFrames)  — control thread, MAY allocate.
 *   process(buf, n)                 — audio thread, realtime-safe (no alloc,
 *                                     no locks, no syscalls).
 *   setParam(paramId, value)        — control thread; writes atomics only,
 *                                     safe concurrent with process().
 *
 * Param values are clamped to the ranges of CONTRACT.md §3 before being
 * stored. Continuous params are smoothed inside process() (one-pole) so
 * live tweaking does not click/zipper.
 * ==========================================================================*/

#ifndef VOICEFX_EFFECTS_HPP_
#define VOICEFX_EFFECTS_HPP_

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../include/voicefx.h"

namespace vfx {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* Kill denormals in feedback paths (belt-and-suspenders on top of the FTZ
 * guard set in vfx_process). */
inline float undenorm(float x) {
  return (std::fabs(x) < 1.0e-15f) ? 0.0f : x;
}

/* --------------------------------------------------------------------------
 * Central param table: {id, min, max, default}. Single source of truth for
 * clamping and defaults; mirrors CONTRACT.md §3 exactly.
 * ------------------------------------------------------------------------*/
struct ParamInfo {
  int id;
  float min, max, def;
};

/* Returns NULL for unknown ids. */
const ParamInfo* paramInfo(int paramId);

/* Effect type that owns a paramId (blocks of 100), or -1. */
inline int paramOwnerType(int paramId) {
  int owner = paramId / 100 - 1;
  return (owner >= 0 && owner < VFX_EFFECT_TYPE_COUNT && paramInfo(paramId))
             ? owner
             : -1;
}

/* Clamps `v` to the documented range of paramId (identity if unknown). */
inline float clampParam(int paramId, float v) {
  const ParamInfo* pi = paramInfo(paramId);
  return pi ? clampf(v, pi->min, pi->max) : v;
}

/* --------------------------------------------------------------------------
 * Param: atomic target + audio-thread smoothing state.
 *   set()  — control thread.
 *   next() — audio thread; one-pole step toward target, returns current.
 *   snap() — audio thread; jump to target (for discrete params like type).
 * ------------------------------------------------------------------------*/
class Param {
 public:
  void init(float v) {
    target_.store(v, std::memory_order_relaxed);
    cur_ = v;
  }
  void set(float v) { target_.store(v, std::memory_order_relaxed); }
  float target() const { return target_.load(std::memory_order_relaxed); }
  float next(float coeff) {
    float t = target();
    cur_ += (t - cur_) * coeff;
    if (std::fabs(cur_ - t) < 1.0e-6f) cur_ = t;
    return cur_;
  }
  float snap() {
    cur_ = target();
    return cur_;
  }
  float current() const { return cur_; }

 private:
  std::atomic<float> target_{0.0f};
  float cur_ = 0.0f; /* audio-thread only */
};

/* Per-block smoothing coefficient (~50 ms time constant at 10 ms blocks). */
constexpr float kBlockSmooth = 0.18f;

/* --------------------------------------------------------------------------
 * Effect base class.
 * ------------------------------------------------------------------------*/
class Effect {
 public:
  virtual ~Effect() = default;
  virtual void prepare(int sampleRate, int maxFrames) = 0;
  virtual void process(float* buf, int n) = 0;
  virtual void setParam(int paramId, float value) = 0;
};

/* Factory for all VfxEffectType values (including VFX_PITCH, whose class
 * lives in pitch.hpp). Returns nullptr for unknown types. May allocate;
 * control thread only. */
Effect* createEffect(int effectType);

/* --------------------------------------------------------------------------
 * VFX_REVERB — Freeverb/Schroeder: 8 parallel damped combs + 4 series
 * allpasses. Tunings are the classic 44.1 kHz lengths scaled to the actual
 * sample rate. roomsize/damp/wet per CONTRACT.
 * ------------------------------------------------------------------------*/
class Reverb final : public Effect {
 public:
  Reverb();
  void prepare(int sampleRate, int maxFrames) override;
  void process(float* buf, int n) override;
  void setParam(int paramId, float value) override;

  static constexpr int kCombs = 8;
  static constexpr int kAllpasses = 4;

 private:
  struct Comb {
    std::vector<float> buf;
    int idx = 0;
    float filterStore = 0.0f;
  };
  struct Allpass {
    std::vector<float> buf;
    int idx = 0;
  };

  Comb combs_[kCombs];
  Allpass aps_[kAllpasses];
  Param roomsize_, damp_, wet_;
};

/* --------------------------------------------------------------------------
 * VFX_DELAY — feedback delay line, fractional read with linear interpolation.
 * Delay time is slewed per-sample (tape-style) so live changes do not click.
 * Buffer is pre-allocated for the max time (2000 ms).
 * ------------------------------------------------------------------------*/
class Delay final : public Effect {
 public:
  Delay();
  void prepare(int sampleRate, int maxFrames) override;
  void process(float* buf, int n) override;
  void setParam(int paramId, float value) override;

 private:
  std::vector<float> buf_;
  int size_ = 0; /* buffer length in samples */
  int w_ = 0;    /* write index */
  float delaySm_ = 0.0f; /* smoothed delay in samples */
  int sr_ = 48000;
  Param timeMs_, feedback_, mix_;
};

/* --------------------------------------------------------------------------
 * VFX_BIQUAD — RBJ cookbook 2nd-order filter (LP/HP/BP/notch), transposed
 * direct form II. Coefficients are recomputed per block from smoothed params.
 * ------------------------------------------------------------------------*/
class BiquadFilter final : public Effect {
 public:
  BiquadFilter();
  void prepare(int sampleRate, int maxFrames) override;
  void process(float* buf, int n) override;
  void setParam(int paramId, float value) override;

 private:
  void computeCoeffs(int type, float freq, float q);
  int sr_ = 48000;
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  float z1_ = 0, z2_ = 0;
  Param type_, freq_, q_;
};

/* --------------------------------------------------------------------------
 * VFX_RINGMOD — sine-carrier ring modulator.
 * ------------------------------------------------------------------------*/
class RingMod final : public Effect {
 public:
  RingMod();
  void prepare(int sampleRate, int maxFrames) override;
  void process(float* buf, int n) override;
  void setParam(int paramId, float value) override;

 private:
  int sr_ = 48000;
  float phase_ = 0.0f;
  Param freq_, mix_;
};

/* --------------------------------------------------------------------------
 * VFX_DISTORTION — tanh waveshaper with linear pre-gain (drive) and wet mix.
 * ------------------------------------------------------------------------*/
class Distortion final : public Effect {
 public:
  Distortion();
  void prepare(int sampleRate, int maxFrames) override;
  void process(float* buf, int n) override;
  void setParam(int paramId, float value) override;

 private:
  Param drive_, mix_;
};

/* --------------------------------------------------------------------------
 * VFX_NOISE — additive colored-noise bed. White noise from a deterministic
 * xorshift32 PRNG; "pink" via Paul Kellet's economy filter; `color`
 * crossfades white -> pink/dark.
 * ------------------------------------------------------------------------*/
class Noise final : public Effect {
 public:
  Noise();
  void prepare(int sampleRate, int maxFrames) override;
  void process(float* buf, int n) override;
  void setParam(int paramId, float value) override;

 private:
  float white() {
    /* xorshift32 — deterministic, no rand(), no syscalls. */
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(static_cast<int32_t>(rng_)) *
           (1.0f / 2147483648.0f);
  }
  uint32_t rng_ = 0x9E3779B9u;
  float b0_ = 0, b1_ = 0, b2_ = 0; /* pink filter state */
  Param level_, color_;
};

/* --------------------------------------------------------------------------
 * VFX_TREMOLO — sine LFO amplitude modulation.
 * gain(t) = 1 - depth * (1 + sin)/2  -> sweeps [1-depth, 1].
 * ------------------------------------------------------------------------*/
class Tremolo final : public Effect {
 public:
  Tremolo();
  void prepare(int sampleRate, int maxFrames) override;
  void process(float* buf, int n) override;
  void setParam(int paramId, float value) override;

 private:
  int sr_ = 48000;
  float phase_ = 0.0f;
  Param rate_, depth_;
};

/* --------------------------------------------------------------------------
 * VFX_CHORUS — short modulated delay (sine LFO), fractional read, wet mix.
 * depth 0..1 maps to ~0..8 ms of sweep over a ~5 ms base delay.
 * ------------------------------------------------------------------------*/
class Chorus final : public Effect {
 public:
  Chorus();
  void prepare(int sampleRate, int maxFrames) override;
  void process(float* buf, int n) override;
  void setParam(int paramId, float value) override;

 private:
  std::vector<float> buf_;
  int size_ = 0;
  int w_ = 0;
  int sr_ = 48000;
  float phase_ = 0.0f;
  Param rate_, depth_, mix_;
};

} /* namespace vfx */

#endif /* VOICEFX_EFFECTS_HPP_ */

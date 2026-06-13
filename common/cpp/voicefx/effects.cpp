/* effects.cpp — DSP primitive implementations. See effects.hpp. */

#include "effects.hpp"

#include "pitch.hpp"

namespace vfx {

/* ==========================================================================
 * Param table (CONTRACT.md §3, authoritative copy).
 * ========================================================================*/
static const ParamInfo kParamTable[] = {
    {VFX_P_REVERB_ROOMSIZE, 0.0f, 1.0f, 0.50f},
    {VFX_P_REVERB_DAMP, 0.0f, 1.0f, 0.50f},
    {VFX_P_REVERB_WET, 0.0f, 1.0f, 0.33f},
    {VFX_P_DELAY_TIME_MS, 1.0f, 2000.0f, 350.0f},
    {VFX_P_DELAY_FEEDBACK, 0.0f, 0.95f, 0.35f},
    {VFX_P_DELAY_MIX, 0.0f, 1.0f, 0.50f},
    {VFX_P_BIQUAD_TYPE, 0.0f, 6.0f, 0.0f},
    {VFX_P_BIQUAD_FREQ, 20.0f, 20000.0f, 1000.0f},
    {VFX_P_BIQUAD_Q, 0.1f, 10.0f, 0.707f},
    {VFX_P_BIQUAD_GAIN_DB, -15.0f, 15.0f, 0.0f},
    {VFX_P_RINGMOD_FREQ, 1.0f, 2000.0f, 30.0f},
    {VFX_P_RINGMOD_MIX, 0.0f, 1.0f, 1.0f},
    {VFX_P_DIST_DRIVE, 1.0f, 50.0f, 8.0f},
    {VFX_P_DIST_MIX, 0.0f, 1.0f, 1.0f},
    {VFX_P_PITCH_SEMITONES, -12.0f, 12.0f, 0.0f},
    {VFX_P_PITCH_FORMANT, 0.5f, 2.0f, 1.0f},
    {VFX_P_NOISE_LEVEL, 0.0f, 1.0f, 0.05f},
    {VFX_P_NOISE_COLOR, 0.0f, 1.0f, 0.50f},
    {VFX_P_TREMOLO_RATE, 0.1f, 20.0f, 5.0f},
    {VFX_P_TREMOLO_DEPTH, 0.0f, 1.0f, 0.5f},
    {VFX_P_CHORUS_RATE, 0.05f, 5.0f, 0.8f},
    {VFX_P_CHORUS_DEPTH, 0.0f, 1.0f, 0.4f},
    {VFX_P_CHORUS_MIX, 0.0f, 1.0f, 0.5f},
    {VFX_P_COMP_GATE_THRESH, -80.0f, 0.0f, -45.0f},
    {VFX_P_COMP_GATE_RELEASE, 10.0f, 300.0f, 120.0f},
    {VFX_P_COMP_RATIO, 1.0f, 20.0f, 3.0f},
    {VFX_P_COMP_THRESH, -40.0f, 0.0f, -18.0f},
    {VFX_P_COMP_ATTACK, 1.0f, 50.0f, 10.0f},
    {VFX_P_COMP_RELEASE, 20.0f, 300.0f, 100.0f},
    {VFX_P_COMP_MAKEUP, 0.0f, 24.0f, 0.0f},
    {VFX_P_CRUSH_BITS, 1.0f, 16.0f, 8.0f},
    {VFX_P_CRUSH_DOWNSAMPLE, 1.0f, 32.0f, 1.0f},
    {VFX_P_CRUSH_MIX, 0.0f, 1.0f, 1.0f},
    {VFX_P_VIBRATO_RATE, 0.1f, 12.0f, 5.5f},
    {VFX_P_VIBRATO_DEPTH_CENTS, 0.0f, 50.0f, 20.0f},
    {VFX_P_FLANGER_RATE, 0.05f, 2.0f, 0.4f},
    {VFX_P_FLANGER_DEPTH, 0.0f, 1.0f, 0.6f},
    {VFX_P_FLANGER_FEEDBACK, 0.0f, 0.9f, 0.5f},
    {VFX_P_FLANGER_MIX, 0.0f, 1.0f, 0.5f},
};

const ParamInfo* paramInfo(int paramId) {
  for (const ParamInfo& pi : kParamTable) {
    if (pi.id == paramId) return &pi;
  }
  return nullptr;
}

static float defOf(int paramId) {
  const ParamInfo* pi = paramInfo(paramId);
  return pi ? pi->def : 0.0f;
}

Effect* createEffect(int effectType) {
  switch (effectType) {
    case VFX_REVERB:     return new Reverb();
    case VFX_DELAY:      return new Delay();
    case VFX_BIQUAD:     return new BiquadFilter();
    case VFX_RINGMOD:    return new RingMod();
    case VFX_DISTORTION: return new Distortion();
    case VFX_PITCH:      return new PitchShifter();
    case VFX_NOISE:      return new Noise();
    case VFX_TREMOLO:    return new Tremolo();
    case VFX_CHORUS:     return new Chorus();
    case VFX_COMP:       return new CompGate();
    case VFX_BITCRUSH:   return new Bitcrush();
    case VFX_VIBRATO:    return new Vibrato();
    case VFX_FLANGER:    return new Flanger();
    default:             return nullptr;
  }
}

/* ==========================================================================
 * Reverb (Freeverb)
 * ========================================================================*/

/* Classic Freeverb tunings at 44.1 kHz (mono set), scaled in prepare(). */
static const int kCombTuning44k[Reverb::kCombs] = {1116, 1188, 1277, 1356,
                                                   1422, 1491, 1557, 1617};
static const int kApTuning44k[Reverb::kAllpasses] = {556, 441, 341, 225};
static constexpr float kFixedGain = 0.015f; /* comb input gain */
static constexpr float kScaleWet = 3.0f;
static constexpr float kScaleDamp = 0.4f;
static constexpr float kScaleRoom = 0.28f;
static constexpr float kOffsetRoom = 0.7f;
static constexpr float kApFeedback = 0.5f;

Reverb::Reverb() {
  roomsize_.init(defOf(VFX_P_REVERB_ROOMSIZE));
  damp_.init(defOf(VFX_P_REVERB_DAMP));
  wet_.init(defOf(VFX_P_REVERB_WET));
}

void Reverb::prepare(int sampleRate, int /*maxFrames*/) {
  const float scale = static_cast<float>(sampleRate) / 44100.0f;
  for (int i = 0; i < kCombs; ++i) {
    int len = static_cast<int>(kCombTuning44k[i] * scale);
    if (len < 8) len = 8;
    combs_[i].buf.assign(static_cast<size_t>(len), 0.0f);
    combs_[i].idx = 0;
    combs_[i].filterStore = 0.0f;
  }
  for (int i = 0; i < kAllpasses; ++i) {
    int len = static_cast<int>(kApTuning44k[i] * scale);
    if (len < 4) len = 4;
    aps_[i].buf.assign(static_cast<size_t>(len), 0.0f);
    aps_[i].idx = 0;
  }
}

void Reverb::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_REVERB_ROOMSIZE: roomsize_.set(value); break;
    case VFX_P_REVERB_DAMP:     damp_.set(value); break;
    case VFX_P_REVERB_WET:      wet_.set(value); break;
    default: break;
  }
}

void Reverb::process(float* buf, int n) {
  const float feedback = roomsize_.next(kBlockSmooth) * kScaleRoom + kOffsetRoom;
  const float damp1 = damp_.next(kBlockSmooth) * kScaleDamp;
  const float damp2 = 1.0f - damp1;
  const float wet = wet_.next(kBlockSmooth) * kScaleWet;

  for (int i = 0; i < n; ++i) {
    const float dry = buf[i];
    const float input = dry * kFixedGain;
    float acc = 0.0f;

    /* 8 parallel lowpass-feedback combs. */
    for (int c = 0; c < kCombs; ++c) {
      Comb& cb = combs_[c];
      float out = cb.buf[static_cast<size_t>(cb.idx)];
      cb.filterStore = undenorm(out * damp2 + cb.filterStore * damp1);
      cb.buf[static_cast<size_t>(cb.idx)] =
          undenorm(input + cb.filterStore * feedback);
      if (++cb.idx >= static_cast<int>(cb.buf.size())) cb.idx = 0;
      acc += out;
    }

    /* 4 series allpasses to diffuse. */
    for (int a = 0; a < kAllpasses; ++a) {
      Allpass& ap = aps_[a];
      float bufout = ap.buf[static_cast<size_t>(ap.idx)];
      float out = -acc + bufout;
      ap.buf[static_cast<size_t>(ap.idx)] =
          undenorm(acc + bufout * kApFeedback);
      if (++ap.idx >= static_cast<int>(ap.buf.size())) ap.idx = 0;
      acc = out;
    }

    buf[i] = dry * (1.0f - wet_.current()) + acc * wet;
  }
}

/* ==========================================================================
 * Delay
 * ========================================================================*/

Delay::Delay() {
  timeMs_.init(defOf(VFX_P_DELAY_TIME_MS));
  feedback_.init(defOf(VFX_P_DELAY_FEEDBACK));
  mix_.init(defOf(VFX_P_DELAY_MIX));
}

void Delay::prepare(int sampleRate, int /*maxFrames*/) {
  sr_ = sampleRate;
  /* Max delay 2000 ms + interpolation guard. */
  size_ = sampleRate * 2 + 4;
  buf_.assign(static_cast<size_t>(size_), 0.0f);
  w_ = 0;
  delaySm_ = timeMs_.target() * 0.001f * static_cast<float>(sr_);
}

void Delay::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_DELAY_TIME_MS:  timeMs_.set(value); break;
    case VFX_P_DELAY_FEEDBACK: feedback_.set(value); break;
    case VFX_P_DELAY_MIX:      mix_.set(value); break;
    default: break;
  }
}

void Delay::process(float* buf, int n) {
  const float target =
      clampf(timeMs_.target() * 0.001f * static_cast<float>(sr_), 1.0f,
             static_cast<float>(size_ - 3));
  const float fb = feedback_.next(kBlockSmooth);
  const float mix = mix_.next(kBlockSmooth);
  /* Per-sample slew (~80 ms time constant @48k): time changes glide like
   * tape instead of clicking. */
  const float slew = 1.0f / (0.08f * static_cast<float>(sr_));

  for (int i = 0; i < n; ++i) {
    delaySm_ += (target - delaySm_) * slew;
    float rpos = static_cast<float>(w_) - delaySm_;
    if (rpos < 0.0f) rpos += static_cast<float>(size_);
    int r0 = static_cast<int>(rpos);
    float frac = rpos - static_cast<float>(r0);
    int r1 = r0 + 1;
    if (r0 >= size_) r0 -= size_;
    if (r1 >= size_) r1 -= size_;
    const float d = buf_[static_cast<size_t>(r0)] +
                    (buf_[static_cast<size_t>(r1)] -
                     buf_[static_cast<size_t>(r0)]) * frac;

    const float in = buf[i];
    buf_[static_cast<size_t>(w_)] = undenorm(in + d * fb);
    if (++w_ >= size_) w_ = 0;

    buf[i] = in * (1.0f - mix) + d * mix;
  }
}

/* ==========================================================================
 * Biquad (RBJ cookbook, TDF-II)
 * ========================================================================*/

BiquadFilter::BiquadFilter() {
  type_.init(defOf(VFX_P_BIQUAD_TYPE));
  freq_.init(defOf(VFX_P_BIQUAD_FREQ));
  q_.init(defOf(VFX_P_BIQUAD_Q));
  gainDb_.init(defOf(VFX_P_BIQUAD_GAIN_DB));
}

void BiquadFilter::prepare(int sampleRate, int /*maxFrames*/) {
  sr_ = sampleRate;
  z1_ = z2_ = 0.0f;
  computeCoeffs(static_cast<int>(type_.snap() + 0.5f), freq_.snap(),
                q_.snap(), gainDb_.snap());
}

void BiquadFilter::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_BIQUAD_TYPE:    type_.set(value); break;
    case VFX_P_BIQUAD_FREQ:    freq_.set(value); break;
    case VFX_P_BIQUAD_Q:       q_.set(value); break;
    case VFX_P_BIQUAD_GAIN_DB: gainDb_.set(value); break;
    default: break;
  }
}

void BiquadFilter::computeCoeffs(int type, float freq, float q, float gainDb) {
  freq = clampf(freq, 20.0f, 0.49f * static_cast<float>(sr_));
  if (q < 0.1f) q = 0.1f;
  const float w0 = kTwoPi * freq / static_cast<float>(sr_);
  const float cw = std::cos(w0);
  const float sw = std::sin(w0);
  const float alpha = sw / (2.0f * q);
  /* RBJ shelf/peaking gain factor: A = 10^(gainDb/40). For LP/HP/BP/NOTCH the
   * shaping terms below are unused, so this is harmless there. */
  const float A = std::pow(10.0f, gainDb * (1.0f / 40.0f));
  float b0, b1, b2, a0, a1, a2;
  switch (type) {
    default:
    case VFX_BIQUAD_LOWPASS:
      b0 = (1.0f - cw) * 0.5f; b1 = 1.0f - cw; b2 = b0;
      a0 = 1.0f + alpha; a1 = -2.0f * cw; a2 = 1.0f - alpha;
      break;
    case VFX_BIQUAD_HIGHPASS:
      b0 = (1.0f + cw) * 0.5f; b1 = -(1.0f + cw); b2 = b0;
      a0 = 1.0f + alpha; a1 = -2.0f * cw; a2 = 1.0f - alpha;
      break;
    case VFX_BIQUAD_BANDPASS: /* constant 0 dB peak gain */
      b0 = alpha; b1 = 0.0f; b2 = -alpha;
      a0 = 1.0f + alpha; a1 = -2.0f * cw; a2 = 1.0f - alpha;
      break;
    case VFX_BIQUAD_NOTCH:
      b0 = 1.0f; b1 = -2.0f * cw; b2 = 1.0f;
      a0 = 1.0f + alpha; a1 = b1; a2 = 1.0f - alpha;
      break;
    case VFX_BIQUAD_PEAKING: /* RBJ peaking EQ: bell boost/cut at freq. */
      b0 = 1.0f + alpha * A; b1 = -2.0f * cw; b2 = 1.0f - alpha * A;
      a0 = 1.0f + alpha / A; a1 = b1; a2 = 1.0f - alpha / A;
      break;
    case VFX_BIQUAD_LOWSHELF: { /* RBJ low-shelf: Q here used as RBJ shelf slope. */
      const float ap1 = A + 1.0f;
      const float am1 = A - 1.0f;
      const float beta = 2.0f * std::sqrt(A) * alpha;
      b0 = A * (ap1 - am1 * cw + beta);
      b1 = 2.0f * A * (am1 - ap1 * cw);
      b2 = A * (ap1 - am1 * cw - beta);
      a0 = ap1 + am1 * cw + beta;
      a1 = -2.0f * (am1 + ap1 * cw);
      a2 = ap1 + am1 * cw - beta;
      break;
    }
    case VFX_BIQUAD_HIGHSHELF: { /* RBJ high-shelf. */
      const float ap1 = A + 1.0f;
      const float am1 = A - 1.0f;
      const float beta = 2.0f * std::sqrt(A) * alpha;
      b0 = A * (ap1 + am1 * cw + beta);
      b1 = -2.0f * A * (am1 + ap1 * cw);
      b2 = A * (ap1 + am1 * cw - beta);
      a0 = ap1 - am1 * cw + beta;
      a1 = 2.0f * (am1 - ap1 * cw);
      a2 = ap1 - am1 * cw - beta;
      break;
    }
  }
  const float inv = 1.0f / a0;
  b0_ = b0 * inv; b1_ = b1 * inv; b2_ = b2 * inv;
  a1_ = a1 * inv; a2_ = a2 * inv;
}

void BiquadFilter::process(float* buf, int n) {
  /* Smooth freq/Q/gain per block and recompute coefficients (cheap). Type
   * switches snap; the TDF-II state carries over without big transients. */
  const int type = static_cast<int>(type_.snap() + 0.5f);
  const float freq = freq_.next(kBlockSmooth);
  const float q = q_.next(kBlockSmooth);
  const float gainDb = gainDb_.next(kBlockSmooth);
  computeCoeffs(type, freq, q, gainDb);

  float z1 = z1_, z2 = z2_;
  for (int i = 0; i < n; ++i) {
    const float x = buf[i];
    const float y = b0_ * x + z1;
    z1 = b1_ * x - a1_ * y + z2;
    z2 = b2_ * x - a2_ * y;
    buf[i] = y;
  }
  z1_ = undenorm(z1);
  z2_ = undenorm(z2);
}

/* ==========================================================================
 * RingMod
 * ========================================================================*/

RingMod::RingMod() {
  freq_.init(defOf(VFX_P_RINGMOD_FREQ));
  mix_.init(defOf(VFX_P_RINGMOD_MIX));
}

void RingMod::prepare(int sampleRate, int /*maxFrames*/) {
  sr_ = sampleRate;
  phase_ = 0.0f;
  xPrev_ = 0.0f;
  lpZ_ = 0.0f;
  /* One-pole LP run at the 2x oversample rate. Cutoff ~6.5 kHz: the
   * multiplication in*carrier pushes content toward Nyquist, so we attenuate
   * the upper sideband at 2x BEFORE decimation, where it would otherwise fold
   * straight back into the voice band. Clamp the cutoff below the oversampled
   * Nyquist for very low sample rates. */
  const float os = 2.0f * static_cast<float>(sr_);
  float fc = 6500.0f;
  const float fcMax = 0.45f * os;
  if (fc > fcMax) fc = fcMax;
  lpA_ = std::exp(-kTwoPi * fc / os);
}

void RingMod::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_RINGMOD_FREQ: freq_.set(value); break;
    case VFX_P_RINGMOD_MIX:  mix_.set(value); break;
    default: break;
  }
}

void RingMod::process(float* buf, int n) {
  /* `inc` is the carrier phase increment per INPUT sample; at 2x we advance
   * half of it per oversampled sub-sample. */
  const float inc = freq_.next(kBlockSmooth) / static_cast<float>(sr_);
  const float incHalf = inc * 0.5f;
  const float mix = mix_.next(kBlockSmooth);
  const float a = lpA_;
  const float b = 1.0f - a;

  float xPrev = xPrev_;
  float lpZ = lpZ_;

  for (int i = 0; i < n; ++i) {
    const float in = buf[i];
    /* 2x linear upsample of the dry input: midpoint, then the sample itself. */
    const float xMid = 0.5f * (xPrev + in);

    /* Sub-sample 0 (midpoint): multiply by carrier, LP at 2x. */
    float carrier = std::sin(kTwoPi * phase_);
    lpZ = b * (xMid * carrier) + a * lpZ;
    phase_ += incHalf;
    if (phase_ >= 1.0f) phase_ -= 1.0f;

    /* Sub-sample 1 (the input sample): multiply by carrier, LP at 2x. */
    carrier = std::sin(kTwoPi * phase_);
    lpZ = b * (in * carrier) + a * lpZ;
    phase_ += incHalf;
    if (phase_ >= 1.0f) phase_ -= 1.0f;

    /* Decimate by 2: keep the band-limited sub-sample 1 as this output. */
    const float ring = lpZ;
    xPrev = in;
    buf[i] = in * (1.0f - mix) + ring * mix;
  }
  xPrev_ = undenorm(xPrev);
  lpZ_ = undenorm(lpZ);
}

/* ==========================================================================
 * Distortion
 * ========================================================================*/

Distortion::Distortion() {
  drive_.init(defOf(VFX_P_DIST_DRIVE));
  mix_.init(defOf(VFX_P_DIST_MIX));
}

void Distortion::prepare(int /*sampleRate*/, int /*maxFrames*/) {
  xPrev_ = 0.0f;
  fPrev_ = 0.0f;
}

void Distortion::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_DIST_DRIVE: drive_.set(value); break;
    case VFX_P_DIST_MIX:   mix_.set(value); break;
    default: break;
  }
}

/* Numerically stable log(cosh(z)) = |z| - log(2) + log1p(exp(-2|z|)).
 * Avoids cosh() overflow at high drive without any branch on sign. */
static inline float logCosh(float z) {
  const float az = std::fabs(z);
  return az - 0.6931471805599453f + std::log1p(std::exp(-2.0f * az));
}

void Distortion::process(float* buf, int n) {
  const float drive = drive_.next(kBlockSmooth);
  const float mix = mix_.next(kBlockSmooth);
  /* Slight makeup attenuation so high drive does not just become "louder". */
  const float makeup = 1.0f / std::sqrt(1.0f + drive * 0.15f);
  const float invDrive = 1.0f / drive;
  /* eps-guard: below this |x - xPrev| the ADAA quotient is numerically
   * unstable (0/0 in quasi-DC: silence, sustained vowels) and would click;
   * fall back to the direct waveshaper at the midpoint. */
  constexpr float kEps = 1.0e-4f;

  float xPrev = xPrev_;
  /* Recompute the cached antiderivative with THIS block's smoothed drive so
   * the first sample's difference quotient stays consistent (drive glides
   * slowly via the smoother, so the discontinuity is negligible). */
  float fPrev = logCosh(xPrev * drive) * invDrive;

  for (int i = 0; i < n; ++i) {
    const float x = buf[i];
    const float dx = x - xPrev;
    float shaped;
    if (std::fabs(dx) > kEps) {
      /* First-order ADAA: y = (F(x) - F(xPrev)) / (x - xPrev). */
      const float fx = logCosh(x * drive) * invDrive;
      shaped = (fx - fPrev) / dx;
      fPrev = fx;
    } else {
      /* Quasi-DC fallback: direct waveshaper at the midpoint. */
      const float mid = 0.5f * (x + xPrev);
      shaped = std::tanh(mid * drive);
      /* Keep fPrev coherent for the next iteration. */
      fPrev = logCosh(x * drive) * invDrive;
    }
    xPrev = x;
    shaped *= makeup;
    buf[i] = x * (1.0f - mix) + shaped * mix;
  }
  xPrev_ = undenorm(xPrev);
  fPrev_ = fPrev;
}

/* ==========================================================================
 * Noise
 * ========================================================================*/

Noise::Noise() {
  level_.init(defOf(VFX_P_NOISE_LEVEL));
  color_.init(defOf(VFX_P_NOISE_COLOR));
}

void Noise::prepare(int /*sampleRate*/, int /*maxFrames*/) {
  b0_ = b1_ = b2_ = 0.0f;
}

void Noise::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_NOISE_LEVEL: level_.set(value); break;
    case VFX_P_NOISE_COLOR: color_.set(value); break;
    default: break;
  }
}

void Noise::process(float* buf, int n) {
  const float level = level_.next(kBlockSmooth);
  const float color = color_.next(kBlockSmooth);
  for (int i = 0; i < n; ++i) {
    const float w = white();
    /* Paul Kellet economy pink filter (3 one-poles). */
    b0_ = 0.99765f * b0_ + w * 0.0990460f;
    b1_ = 0.96300f * b1_ + w * 0.2965164f;
    b2_ = 0.57000f * b2_ + w * 1.0526913f;
    const float pink = (b0_ + b1_ + b2_ + w * 0.1848f) * 0.30f;
    const float noise = w * (1.0f - color) + pink * color;
    buf[i] += noise * level;
  }
}

/* ==========================================================================
 * Tremolo
 * ========================================================================*/

Tremolo::Tremolo() {
  rate_.init(defOf(VFX_P_TREMOLO_RATE));
  depth_.init(defOf(VFX_P_TREMOLO_DEPTH));
}

void Tremolo::prepare(int sampleRate, int /*maxFrames*/) {
  sr_ = sampleRate;
  phase_ = 0.0f;
}

void Tremolo::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_TREMOLO_RATE:  rate_.set(value); break;
    case VFX_P_TREMOLO_DEPTH: depth_.set(value); break;
    default: break;
  }
}

void Tremolo::process(float* buf, int n) {
  const float inc = rate_.next(kBlockSmooth) / static_cast<float>(sr_);
  const float depth = depth_.next(kBlockSmooth);
  for (int i = 0; i < n; ++i) {
    const float lfo = 0.5f * (1.0f + std::sin(kTwoPi * phase_));
    phase_ += inc;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
    buf[i] *= 1.0f - depth * lfo;
  }
}

/* ==========================================================================
 * Chorus
 * ========================================================================*/

Chorus::Chorus() {
  rate_.init(defOf(VFX_P_CHORUS_RATE));
  depth_.init(defOf(VFX_P_CHORUS_DEPTH));
  mix_.init(defOf(VFX_P_CHORUS_MIX));
}

void Chorus::prepare(int sampleRate, int /*maxFrames*/) {
  sr_ = sampleRate;
  /* Max delay: 5 ms base + 8 ms sweep + guard -> 20 ms. */
  size_ = sampleRate / 50 + 4;
  buf_.assign(static_cast<size_t>(size_), 0.0f);
  w_ = 0;
  phase_ = 0.0f;
}

void Chorus::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_CHORUS_RATE:  rate_.set(value); break;
    case VFX_P_CHORUS_DEPTH: depth_.set(value); break;
    case VFX_P_CHORUS_MIX:   mix_.set(value); break;
    default: break;
  }
}

void Chorus::process(float* buf, int n) {
  const float inc = rate_.next(kBlockSmooth) / static_cast<float>(sr_);
  const float depth = depth_.next(kBlockSmooth);
  const float mix = mix_.next(kBlockSmooth);
  const float srMs = static_cast<float>(sr_) * 0.001f;
  const float baseSamp = 5.0f * srMs;          /* 5 ms base */
  const float sweepSamp = 8.0f * srMs * depth; /* up to +8 ms */

  for (int i = 0; i < n; ++i) {
    const float lfo = 0.5f * (1.0f + std::sin(kTwoPi * phase_));
    phase_ += inc;
    if (phase_ >= 1.0f) phase_ -= 1.0f;

    float delay = baseSamp + sweepSamp * lfo;
    if (delay > static_cast<float>(size_ - 3)) {
      delay = static_cast<float>(size_ - 3);
    }
    float rpos = static_cast<float>(w_) - delay;
    if (rpos < 0.0f) rpos += static_cast<float>(size_);
    int r0 = static_cast<int>(rpos);
    const float frac = rpos - static_cast<float>(r0);
    int r1 = r0 + 1;
    if (r0 >= size_) r0 -= size_;
    if (r1 >= size_) r1 -= size_;
    const float d = buf_[static_cast<size_t>(r0)] +
                    (buf_[static_cast<size_t>(r1)] -
                     buf_[static_cast<size_t>(r0)]) * frac;

    const float in = buf[i];
    buf_[static_cast<size_t>(w_)] = undenorm(in);
    if (++w_ >= size_) w_ = 0;

    buf[i] = in * (1.0f - mix) + d * mix;
  }
}

/* ==========================================================================
 * Bitcrush (bit quantization + sample&hold decimator)
 * ========================================================================*/

Bitcrush::Bitcrush() {
  bits_.init(defOf(VFX_P_CRUSH_BITS));
  downsample_.init(defOf(VFX_P_CRUSH_DOWNSAMPLE));
  mix_.init(defOf(VFX_P_CRUSH_MIX));
}

void Bitcrush::prepare(int /*sampleRate*/, int /*maxFrames*/) {
  holdCount_ = 0;
  held_ = 0.0f;
}

void Bitcrush::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_CRUSH_BITS:       bits_.set(value); break;
    case VFX_P_CRUSH_DOWNSAMPLE: downsample_.set(value); break;
    case VFX_P_CRUSH_MIX:        mix_.set(value); break;
    default: break;
  }
}

void Bitcrush::process(float* buf, int n) {
  /* bits/downsample are discrete: snap (no smoothing) like biquad type. */
  const float bits = clampf(bits_.snap(), 1.0f, 16.0f);
  int hold = static_cast<int>(downsample_.snap() + 0.5f);
  if (hold < 1) hold = 1;
  const float mix = mix_.next(kBlockSmooth);

  /* Quantization step: 2^bits levels mapped over [-1, +1]. */
  const float levels = std::pow(2.0f, bits);
  const float step = 2.0f / levels;
  const float invStep = 1.0f / step;

  for (int i = 0; i < n; ++i) {
    const float in = buf[i];
    /* Sample & hold every `hold` samples. */
    if (holdCount_ <= 0) {
      held_ = in;
      holdCount_ = hold;
    }
    --holdCount_;
    /* Mid-tread quantizer. */
    const float crushed = std::floor(held_ * invStep + 0.5f) * step;
    buf[i] = in * (1.0f - mix) + crushed * mix;
  }
}

/* ==========================================================================
 * Vibrato (LFO-modulated short delay, 100% wet -> pitch wobble)
 * ========================================================================*/

Vibrato::Vibrato() {
  rate_.init(defOf(VFX_P_VIBRATO_RATE));
  depthCents_.init(defOf(VFX_P_VIBRATO_DEPTH_CENTS));
}

void Vibrato::prepare(int sampleRate, int /*maxFrames*/) {
  sr_ = sampleRate;
  /* Base delay 3 ms + max sweep ~3 ms + guard -> ~10 ms ring. */
  size_ = sampleRate / 100 + 4;
  buf_.assign(static_cast<size_t>(size_), 0.0f);
  w_ = 0;
  phase_ = 0.0f;
}

void Vibrato::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_VIBRATO_RATE:        rate_.set(value); break;
    case VFX_P_VIBRATO_DEPTH_CENTS: depthCents_.set(value); break;
    default: break;
  }
}

void Vibrato::process(float* buf, int n) {
  const float inc = rate_.next(kBlockSmooth) / static_cast<float>(sr_);
  const float cents = depthCents_.next(kBlockSmooth);
  const float srMs = static_cast<float>(sr_) * 0.001f;
  const float baseSamp = 3.0f * srMs; /* 3 ms center delay */
  /* Map cents of pitch deviation to a delay-sweep amplitude (small-signal:
   * peak deviation ~ cents/100 * baseDelay for a sine-modulated delay). */
  const float sweepSamp = (cents / 100.0f) * baseSamp;

  for (int i = 0; i < n; ++i) {
    const float lfo = std::sin(kTwoPi * phase_); /* -1..+1, no dry mix */
    phase_ += inc;
    if (phase_ >= 1.0f) phase_ -= 1.0f;

    float delay = baseSamp + sweepSamp * lfo;
    if (delay < 1.0f) delay = 1.0f;
    if (delay > static_cast<float>(size_ - 3)) {
      delay = static_cast<float>(size_ - 3);
    }
    float rpos = static_cast<float>(w_) - delay;
    if (rpos < 0.0f) rpos += static_cast<float>(size_);
    int r0 = static_cast<int>(rpos);
    const float frac = rpos - static_cast<float>(r0);
    int r1 = r0 + 1;
    if (r0 >= size_) r0 -= size_;
    if (r1 >= size_) r1 -= size_;
    const float d = buf_[static_cast<size_t>(r0)] +
                    (buf_[static_cast<size_t>(r1)] -
                     buf_[static_cast<size_t>(r0)]) * frac;

    buf_[static_cast<size_t>(w_)] = undenorm(buf[i]);
    if (++w_ >= size_) w_ = 0;

    buf[i] = d; /* fully wet: vibrato replaces the signal */
  }
}

/* ==========================================================================
 * Flanger (short comb + LFO + feedback)
 * ========================================================================*/

Flanger::Flanger() {
  rate_.init(defOf(VFX_P_FLANGER_RATE));
  depth_.init(defOf(VFX_P_FLANGER_DEPTH));
  feedback_.init(defOf(VFX_P_FLANGER_FEEDBACK));
  mix_.init(defOf(VFX_P_FLANGER_MIX));
}

void Flanger::prepare(int sampleRate, int /*maxFrames*/) {
  sr_ = sampleRate;
  /* Comb 0.5..7 ms + guard -> ~10 ms ring. */
  size_ = sampleRate / 100 + 4;
  buf_.assign(static_cast<size_t>(size_), 0.0f);
  w_ = 0;
  phase_ = 0.0f;
}

void Flanger::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_FLANGER_RATE:     rate_.set(value); break;
    case VFX_P_FLANGER_DEPTH:    depth_.set(value); break;
    case VFX_P_FLANGER_FEEDBACK: feedback_.set(value); break;
    case VFX_P_FLANGER_MIX:      mix_.set(value); break;
    default: break;
  }
}

void Flanger::process(float* buf, int n) {
  const float inc = rate_.next(kBlockSmooth) / static_cast<float>(sr_);
  const float depth = depth_.next(kBlockSmooth);
  const float fb = feedback_.next(kBlockSmooth);
  const float mix = mix_.next(kBlockSmooth);
  const float srMs = static_cast<float>(sr_) * 0.001f;
  const float baseSamp = 0.5f * srMs;            /* 0.5 ms minimum comb */
  const float sweepSamp = 6.5f * srMs * depth;   /* up to +6.5 ms (->7 ms) */

  for (int i = 0; i < n; ++i) {
    const float lfo = 0.5f * (1.0f + std::sin(kTwoPi * phase_));
    phase_ += inc;
    if (phase_ >= 1.0f) phase_ -= 1.0f;

    float delay = baseSamp + sweepSamp * lfo;
    if (delay > static_cast<float>(size_ - 3)) {
      delay = static_cast<float>(size_ - 3);
    }
    float rpos = static_cast<float>(w_) - delay;
    if (rpos < 0.0f) rpos += static_cast<float>(size_);
    int r0 = static_cast<int>(rpos);
    const float frac = rpos - static_cast<float>(r0);
    int r1 = r0 + 1;
    if (r0 >= size_) r0 -= size_;
    if (r1 >= size_) r1 -= size_;
    const float d = buf_[static_cast<size_t>(r0)] +
                    (buf_[static_cast<size_t>(r1)] -
                     buf_[static_cast<size_t>(r0)]) * frac;

    const float in = buf[i];
    buf_[static_cast<size_t>(w_)] = undenorm(in + d * fb);
    if (++w_ >= size_) w_ = 0;

    buf[i] = in * (1.0f - mix) + d * mix;
  }
}

/* ==========================================================================
 * CompGate (noise gate + compressor)
 * ========================================================================*/

/* dB <-> linear helpers (local; voltage/amplitude convention, 20*log10). */
static inline float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }

/* One-pole time-constant coefficient for a given time in ms: the per-sample
 * factor so the envelope reaches ~63% of a step in `ms` milliseconds. */
static inline float tcCoeff(float ms, float sr) {
  if (ms < 0.001f) return 0.0f; /* instant */
  return std::exp(-1.0f / (ms * 0.001f * sr));
}

CompGate::CompGate() {
  gateThresh_.init(defOf(VFX_P_COMP_GATE_THRESH));
  gateRelease_.init(defOf(VFX_P_COMP_GATE_RELEASE));
  ratio_.init(defOf(VFX_P_COMP_RATIO));
  thresh_.init(defOf(VFX_P_COMP_THRESH));
  attack_.init(defOf(VFX_P_COMP_ATTACK));
  release_.init(defOf(VFX_P_COMP_RELEASE));
  makeup_.init(defOf(VFX_P_COMP_MAKEUP));
}

void CompGate::prepare(int sampleRate, int /*maxFrames*/) {
  sr_ = sampleRate;
  gateEnv_ = 0.0f;
  gateGain_ = 0.0f;
  rms_ = 0.0f;
  compGain_ = 1.0f;
}

void CompGate::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_COMP_GATE_THRESH:  gateThresh_.set(value); break;
    case VFX_P_COMP_GATE_RELEASE: gateRelease_.set(value); break;
    case VFX_P_COMP_RATIO:        ratio_.set(value); break;
    case VFX_P_COMP_THRESH:       thresh_.set(value); break;
    case VFX_P_COMP_ATTACK:       attack_.set(value); break;
    case VFX_P_COMP_RELEASE:      release_.set(value); break;
    case VFX_P_COMP_MAKEUP:       makeup_.set(value); break;
    default: break;
  }
}

void CompGate::process(float* buf, int n) {
  const float sr = static_cast<float>(sr_);

  /* Smoothed param targets (per block) -> linear domain / coefficients. */
  const float gateThreshLin = dbToLin(gateThresh_.next(kBlockSmooth));
  /* Gate: instant peak attack, configurable release. A small hysteresis
   * (open 6 dB above the close threshold) avoids chatter on borderline level. */
  const float gateOpenLin = gateThreshLin * 1.9952623f; /* +6 dB */
  const float gateRelCoeff = tcCoeff(gateRelease_.next(kBlockSmooth), sr);
  /* Gate gain glides at the same release rate when closing; opens fast. */
  const float gateGainRel = gateRelCoeff;

  const float ratio = ratio_.next(kBlockSmooth);
  const float invRatio = 1.0f / ratio;
  const float compThreshDb = thresh_.next(kBlockSmooth);
  const float attCoeff = tcCoeff(attack_.next(kBlockSmooth), sr);
  const float relCoeff = tcCoeff(release_.next(kBlockSmooth), sr);
  const float makeupLin = dbToLin(makeup_.next(kBlockSmooth));
  /* RMS detector time constant (~10 ms) — independent of comp attack/release;
   * the attack/release shape the GAIN, the RMS window shapes the LEVEL. */
  const float rmsCoeff = tcCoeff(10.0f, sr);

  float gateEnv = gateEnv_;
  float gateGain = gateGain_;
  float rms = rms_;
  float compGain = compGain_;

  for (int i = 0; i < n; ++i) {
    const float x = buf[i];
    const float ax = std::fabs(x);

    /* ---- Gate: fast PEAK detector (own envelope, NOT the comp RMS). ---- */
    if (ax > gateEnv) {
      gateEnv = ax; /* instant attack */
    } else {
      gateEnv = undenorm(ax + (gateEnv - ax) * gateRelCoeff);
    }
    /* Soft target: 1 fully above the open point, 0 below the close point,
     * linear ramp across the 6 dB knee. */
    float gateTarget;
    if (gateEnv >= gateOpenLin) {
      gateTarget = 1.0f;
    } else if (gateEnv <= gateThreshLin) {
      gateTarget = 0.0f;
    } else {
      const float span = gateOpenLin - gateThreshLin;
      gateTarget = (span > 1.0e-12f) ? (gateEnv - gateThreshLin) / span : 1.0f;
    }
    /* Open fast, close at the configured release rate. */
    if (gateTarget > gateGain) {
      gateGain = gateTarget; /* instant open */
    } else {
      gateGain = undenorm(gateTarget + (gateGain - gateTarget) * gateGainRel);
    }
    const float gated = x * gateGain;

    /* ---- Compressor: own one-pole RMS detector on the GATED signal. ---- */
    const float sq = gated * gated;
    rms = undenorm(sq + (rms - sq) * rmsCoeff);
    const float level = std::sqrt(rms);
    /* Static curve: gain reduction (in dB) above threshold, then ratio. */
    float targetGain = 1.0f;
    if (level > 1.0e-9f) {
      const float levelDb = 20.0f * std::log10(level);
      if (levelDb > compThreshDb) {
        const float over = levelDb - compThreshDb;
        const float reducedDb = over * (invRatio - 1.0f); /* negative */
        targetGain = dbToLin(reducedDb);
      }
    }
    /* Attack when clamping down (targetGain < current), release otherwise. */
    const float coeff = (targetGain < compGain) ? attCoeff : relCoeff;
    compGain = undenorm(targetGain + (compGain - targetGain) * coeff);

    buf[i] = gated * compGain * makeupLin;
  }

  gateEnv_ = undenorm(gateEnv);
  gateGain_ = gateGain;
  rms_ = undenorm(rms);
  compGain_ = compGain;
}


} /* namespace vfx */

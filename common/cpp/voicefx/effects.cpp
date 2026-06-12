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
    {VFX_P_BIQUAD_TYPE, 0.0f, 3.0f, 0.0f},
    {VFX_P_BIQUAD_FREQ, 20.0f, 20000.0f, 1000.0f},
    {VFX_P_BIQUAD_Q, 0.1f, 10.0f, 0.707f},
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
}

void BiquadFilter::prepare(int sampleRate, int /*maxFrames*/) {
  sr_ = sampleRate;
  z1_ = z2_ = 0.0f;
  computeCoeffs(static_cast<int>(type_.snap() + 0.5f), freq_.snap(),
                q_.snap());
}

void BiquadFilter::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_BIQUAD_TYPE: type_.set(value); break;
    case VFX_P_BIQUAD_FREQ: freq_.set(value); break;
    case VFX_P_BIQUAD_Q:    q_.set(value); break;
    default: break;
  }
}

void BiquadFilter::computeCoeffs(int type, float freq, float q) {
  freq = clampf(freq, 20.0f, 0.49f * static_cast<float>(sr_));
  if (q < 0.1f) q = 0.1f;
  const float w0 = kTwoPi * freq / static_cast<float>(sr_);
  const float cw = std::cos(w0);
  const float sw = std::sin(w0);
  const float alpha = sw / (2.0f * q);
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
  }
  const float inv = 1.0f / a0;
  b0_ = b0 * inv; b1_ = b1 * inv; b2_ = b2 * inv;
  a1_ = a1 * inv; a2_ = a2 * inv;
}

void BiquadFilter::process(float* buf, int n) {
  /* Smooth freq/Q per block and recompute coefficients (cheap). Type
   * switches snap; the TDF-II state carries over without big transients. */
  const int type = static_cast<int>(type_.snap() + 0.5f);
  const float freq = freq_.next(kBlockSmooth);
  const float q = q_.next(kBlockSmooth);
  computeCoeffs(type, freq, q);

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
  const float inc = freq_.next(kBlockSmooth) / static_cast<float>(sr_);
  const float mix = mix_.next(kBlockSmooth);
  for (int i = 0; i < n; ++i) {
    const float carrier = std::sin(kTwoPi * phase_);
    phase_ += inc;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
    const float in = buf[i];
    buf[i] = in * (1.0f - mix) + (in * carrier) * mix;
  }
}

/* ==========================================================================
 * Distortion
 * ========================================================================*/

Distortion::Distortion() {
  drive_.init(defOf(VFX_P_DIST_DRIVE));
  mix_.init(defOf(VFX_P_DIST_MIX));
}

void Distortion::prepare(int /*sampleRate*/, int /*maxFrames*/) {}

void Distortion::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_DIST_DRIVE: drive_.set(value); break;
    case VFX_P_DIST_MIX:   mix_.set(value); break;
    default: break;
  }
}

void Distortion::process(float* buf, int n) {
  const float drive = drive_.next(kBlockSmooth);
  const float mix = mix_.next(kBlockSmooth);
  /* Slight makeup attenuation so high drive does not just become "louder". */
  const float makeup = 1.0f / std::sqrt(1.0f + drive * 0.15f);
  for (int i = 0; i < n; ++i) {
    const float in = buf[i];
    const float shaped = std::tanh(in * drive) * makeup;
    buf[i] = in * (1.0f - mix) + shaped * mix;
  }
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

} /* namespace vfx */

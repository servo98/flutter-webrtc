/* pitch.cpp — phase-vocoder pitch shifter with cepstral formant warping.
 * Algorithm and latency notes in pitch.hpp. */

#include "pitch.hpp"

#include <cstring>

namespace vfx {

PitchShifter::PitchShifter() {
  semitones_.init(0.0f);
  formant_.init(1.0f);
}

void PitchShifter::prepare(int sampleRate, int /*maxFrames*/) {
  sr_ = sampleRate;
  rover_ = kLatency;

  inFifo_.assign(kFft, 0.0f);
  outFifo_.assign(kHop, 0.0f);
  accum_.assign(kFft, 0.0f);

  re_.assign(kFft, 0.0f);
  im_.assign(kFft, 0.0f);
  cepRe_.assign(kFft, 0.0f);
  cepIm_.assign(kFft, 0.0f);

  win_.resize(kFft);
  for (int k = 0; k < kFft; ++k) {
    win_[static_cast<size_t>(k)] =
        0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(k) /
                               static_cast<float>(kFft));
  }

  /* FFT tables: bit-reversal permutation + forward twiddles. */
  bitrev_.assign(kFft, 0);
  for (int i = 0; i < kFft; ++i) {
    int j = 0;
    for (int b = 1, m = kFft >> 1; m; b <<= 1, m >>= 1) {
      if (i & b) j |= m;
    }
    bitrev_[static_cast<size_t>(i)] = j;
  }
  twRe_.resize(kFft / 2);
  twIm_.resize(kFft / 2);
  for (int k = 0; k < kFft / 2; ++k) {
    const float a = kTwoPi * static_cast<float>(k) / static_cast<float>(kFft);
    twRe_[static_cast<size_t>(k)] = std::cos(a);
    twIm_[static_cast<size_t>(k)] = -std::sin(a); /* forward = e^(-i a) */
  }

  lastPhase_.assign(kBins + 1, 0.0f);
  sumPhase_.assign(kBins + 1, 0.0f);
  mag_.assign(kBins + 1, 0.0f);
  trueBin_.assign(kBins + 1, 0.0f);
  env_.assign(kBins + 1, 0.0f);
  exc_.assign(kBins + 1, 0.0f);
  synMag_.assign(kBins + 1, 0.0f);
  synBin_.assign(kBins + 1, 0.0f);

  /* Lifter cutoff: keep quefrencies below the shortest expected pitch
   * period (~350 Hz f0) so the envelope captures formants, not harmonics. */
  int cut = sampleRate / 350;
  if (cut < 16) cut = 16;
  if (cut > kFft / 4) cut = kFft / 4;
  cepCut_ = cut;
}

void PitchShifter::setParam(int paramId, float value) {
  value = clampParam(paramId, value);
  switch (paramId) {
    case VFX_P_PITCH_SEMITONES: semitones_.set(value); break;
    case VFX_P_PITCH_FORMANT:   formant_.set(value); break;
    default: break;
  }
}

void PitchShifter::fft(float* re, float* im, bool inverse) {
  for (int i = 0; i < kFft; ++i) {
    const int j = bitrev_[static_cast<size_t>(i)];
    if (j > i) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= kFft; len <<= 1) {
    const int half = len >> 1;
    const int step = kFft / len;
    for (int base = 0; base < kFft; base += len) {
      for (int k = 0; k < half; ++k) {
        const int t = k * step;
        const float wr = twRe_[static_cast<size_t>(t)];
        const float wi = inverse ? -twIm_[static_cast<size_t>(t)]
                                 : twIm_[static_cast<size_t>(t)];
        const int a = base + k;
        const int b = a + half;
        const float vr = re[b] * wr - im[b] * wi;
        const float vi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - vr;
        im[b] = im[a] - vi;
        re[a] += vr;
        im[a] += vi;
      }
    }
  }
  if (inverse) {
    const float s = 1.0f / static_cast<float>(kFft);
    for (int i = 0; i < kFft; ++i) {
      re[i] *= s;
      im[i] *= s;
    }
  }
}

float PitchShifter::envAt(float x) const {
  x = clampf(x, 0.0f, static_cast<float>(kBins));
  const int i0 = static_cast<int>(x);
  const int i1 = i0 < kBins ? i0 + 1 : kBins;
  const float frac = x - static_cast<float>(i0);
  return env_[static_cast<size_t>(i0)] +
         (env_[static_cast<size_t>(i1)] - env_[static_cast<size_t>(i0)]) *
             frac;
}

static inline float wrapPhase(float p) {
  /* Wrap to (-pi, pi]. */
  return p - kTwoPi * std::round(p / kTwoPi);
}

void PitchShifter::processFrame(float pitchRatio, float formantRatio) {
  const float expct = kTwoPi * static_cast<float>(kHop) /
                      static_cast<float>(kFft); /* expected phase adv / bin */
  const float binPerRad = static_cast<float>(kOverlap) / kTwoPi;

  /* ---- 1. Analysis: windowed FFT, magnitude + true bin frequency ------- */
  float* re = re_.data();
  float* im = im_.data();
  for (int k = 0; k < kFft; ++k) {
    re[k] = inFifo_[static_cast<size_t>(k)] * win_[static_cast<size_t>(k)];
    im[k] = 0.0f;
  }
  fft(re, im, false);
  for (int k = 0; k <= kBins; ++k) {
    const float m = std::sqrt(re[k] * re[k] + im[k] * im[k]);
    const float phase = std::atan2(im[k], re[k]);
    float d = phase - lastPhase_[static_cast<size_t>(k)];
    lastPhase_[static_cast<size_t>(k)] = phase;
    d -= static_cast<float>(k) * expct;
    d = wrapPhase(d);
    mag_[static_cast<size_t>(k)] = m;
    trueBin_[static_cast<size_t>(k)] = static_cast<float>(k) + d * binPerRad;
  }

  /* ---- 2. Spectral envelope via cepstral liftering ---------------------- */
  float* cre = cepRe_.data();
  float* cim = cepIm_.data();
  for (int k = 0; k <= kBins; ++k) {
    cre[k] = std::log(mag_[static_cast<size_t>(k)] + 1.0e-6f);
  }
  for (int k = kBins + 1; k < kFft; ++k) cre[k] = cre[kFft - k];
  std::memset(cim, 0, sizeof(float) * kFft);
  fft(cre, cim, false); /* -> real cepstrum (input is real & even) */
  for (int q = cepCut_ + 1; q < kFft - cepCut_; ++q) {
    cre[q] = 0.0f;
    cim[q] = 0.0f;
  }
  fft(cre, cim, true); /* back to smoothed log-magnitude */
  for (int k = 0; k <= kBins; ++k) {
    env_[static_cast<size_t>(k)] = std::exp(clampf(cre[k], -16.0f, 8.0f));
    /* 3. Whitened excitation (formant-free). */
    exc_[static_cast<size_t>(k)] =
        mag_[static_cast<size_t>(k)] / (env_[static_cast<size_t>(k)] + 1.0e-9f);
  }

  /* ---- 4+5. Shift excitation by P, recolor with envelope warped by F --- */
  for (int k = 0; k <= kBins; ++k) {
    synMag_[static_cast<size_t>(k)] = 0.0f;
    synBin_[static_cast<size_t>(k)] = static_cast<float>(k); /* neutral adv */
  }
  for (int k = 0; k <= kBins; ++k) {
    const int idx =
        static_cast<int>(static_cast<float>(k) * pitchRatio + 0.5f);
    if (idx < 0 || idx > kBins) continue;
    synMag_[static_cast<size_t>(idx)] +=
        exc_[static_cast<size_t>(k)] *
        envAt(static_cast<float>(idx) / formantRatio);
    synBin_[static_cast<size_t>(idx)] =
        trueBin_[static_cast<size_t>(k)] * pitchRatio;
  }

  /* ---- 6. Synthesis: phase accumulation, IFFT, windowed OLA ------------ */
  for (int k = 0; k <= kBins; ++k) {
    sumPhase_[static_cast<size_t>(k)] =
        wrapPhase(sumPhase_[static_cast<size_t>(k)] +
                  synBin_[static_cast<size_t>(k)] * expct);
    const float ph = sumPhase_[static_cast<size_t>(k)];
    const float m = synMag_[static_cast<size_t>(k)];
    re[k] = m * std::cos(ph);
    im[k] = m * std::sin(ph);
  }
  for (int k = kBins + 1; k < kFft; ++k) { /* hermitian mirror */
    re[k] = re[kFft - k];
    im[k] = -im[kFft - k];
  }
  fft(re, im, true);

  /* Hann on both analysis and synthesis with hop N/4: sum of win^2 = 1.5. */
  const float olaNorm = 1.0f / 1.5f;
  for (int k = 0; k < kFft; ++k) {
    accum_[static_cast<size_t>(k)] +=
        win_[static_cast<size_t>(k)] * re[k] * olaNorm;
  }
  std::memcpy(outFifo_.data(), accum_.data(), sizeof(float) * kHop);
  std::memmove(accum_.data(), accum_.data() + kHop,
               sizeof(float) * (kFft - kHop));
  std::memset(accum_.data() + (kFft - kHop), 0, sizeof(float) * kHop);
  std::memmove(inFifo_.data(), inFifo_.data() + kHop,
               sizeof(float) * (kFft - kHop));
}

void PitchShifter::process(float* buf, int n) {
  /* Params snap per call; the PV itself transitions smoothly because
   * magnitudes evolve frame to frame. */
  const float pitchRatio =
      std::pow(2.0f, semitones_.snap() * (1.0f / 12.0f));
  const float formantRatio = formant_.snap();

  for (int i = 0; i < n; ++i) {
    inFifo_[static_cast<size_t>(rover_)] = buf[i];
    buf[i] = outFifo_[static_cast<size_t>(rover_ - kLatency)];
    if (++rover_ >= kFft) {
      processFrame(pitchRatio, formantRatio);
      rover_ = kLatency;
    }
  }
}

} /* namespace vfx */

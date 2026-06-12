/* ============================================================================
 * pitch.hpp — VFX_PITCH: real-time pitch shifter with INDEPENDENT formant
 * control (phase vocoder + cepstral envelope warping).
 *
 * ALGORITHM
 *   STFT phase vocoder (Hann window, N = 1024, hop = N/4 = 256, i.e. the
 *   classic smbPitchShift streaming layout, re-implemented from scratch):
 *     1. Analysis: windowed FFT; per-bin magnitude + true frequency from
 *        phase difference (standard PV frequency estimation).
 *     2. Spectral envelope: real cepstrum of log|X|, low-quefrency liftering
 *        (cutoff ~ sr/350 -> keeps formants, drops pitch harmonics), back to
 *        a smooth log-envelope E(k).
 *     3. Excitation = |X| / E  (whitened spectrum, formant-free).
 *     4. Pitch shift: excitation bin k is moved to round(k * P), with true
 *        frequencies scaled by P (P = 2^(semitones/12)).
 *     5. Formant: the shifted excitation is recolored with the WARPED
 *        envelope E(k / F) — pitch and formants move independently.
 *     6. Synthesis: phase accumulation per bin, IFFT, windowed overlap-add.
 *
 * LATENCY: fixed N - hop = 768 samples = 16 ms @ 48 kHz (within the 30 ms
 * budget of CONTRACT.md). The latency is constant regardless of params; the
 * shifter keeps processing at semitones=0/formant=1 so toggling params never
 * jumps time alignment.
 *
 * QUALITY NOTE: this is a compact, dependency-free PV; transients smear a
 * little and large shifts get the usual "phasey" PV character. Signalsmith
 * Stretch (MIT) or Rubber Band can be dropped behind this same Effect
 * interface later for higher quality without touching the ABI.
 *
 * Realtime-safety: all buffers/tables are allocated in prepare(); process()
 * performs no allocation, locks or syscalls.
 * ==========================================================================*/

#ifndef VOICEFX_PITCH_HPP_
#define VOICEFX_PITCH_HPP_

#include <vector>

#include "effects.hpp"

namespace vfx {

class PitchShifter final : public Effect {
 public:
  PitchShifter();
  void prepare(int sampleRate, int maxFrames) override;
  void process(float* buf, int n) override;
  void setParam(int paramId, float value) override;

  /* Fixed algorithmic latency in samples (N - hop). */
  static constexpr int kFft = 1024;
  static constexpr int kOverlap = 4;
  static constexpr int kHop = kFft / kOverlap;
  static constexpr int kLatency = kFft - kHop; /* 768 = 16 ms @ 48 kHz */
  static constexpr int kBins = kFft / 2;       /* bins 0..kBins inclusive */

 private:
  void processFrame(float pitchRatio, float formantRatio);
  /* In-place complex radix-2 FFT over preallocated re/im arrays.
   * inverse=true includes the 1/N normalization. */
  void fft(float* re, float* im, bool inverse);
  /* Linear-interp lookup into env_[0..kBins] at fractional bin x. */
  float envAt(float x) const;

  int sr_ = 48000;
  int rover_ = kLatency;

  /* Streaming FIFOs (smbPitchShift layout). */
  std::vector<float> inFifo_;   /* kFft */
  std::vector<float> outFifo_;  /* kHop */
  std::vector<float> accum_;    /* kFft, overlap-add accumulator */

  /* FFT workspace + tables. */
  std::vector<float> re_, im_;       /* kFft */
  std::vector<float> cepRe_, cepIm_; /* kFft, cepstrum workspace */
  std::vector<float> win_;           /* kFft, Hann */
  std::vector<int> bitrev_;          /* kFft */
  std::vector<float> twRe_, twIm_;   /* kFft/2 twiddles */

  /* Per-bin analysis/synthesis state. */
  std::vector<float> lastPhase_; /* kBins+1 */
  std::vector<float> sumPhase_;  /* kBins+1 */
  std::vector<float> mag_, trueBin_, env_, exc_;
  std::vector<float> synMag_, synBin_;

  int cepCut_ = 137; /* lifter cutoff in quefrency bins (set in prepare) */

  Param semitones_, formant_;
};

} /* namespace vfx */

#endif /* VOICEFX_PITCH_HPP_ */

// spectral_ns.cc — [chatpapol] Supresor de ruido espectral (Wiener + MCRA).
// Ver spectral_ns.h para la teoría. Autocontenido: FFT radix-2 propia.
#include "spectral_ns.h"

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace chatpapol {
namespace {

// FFT radix-2 iterativa, in-place, complejo. n potencia de 2.
// inverse=false: forward (sin normalizar). inverse=true: divide por n.
void Fft(float* re, float* im, int n, bool inverse) {
  // bit-reversal
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = (inverse ? 2.0 : -2.0) * M_PI / len;
    const float wlr = static_cast<float>(std::cos(ang));
    const float wli = static_cast<float>(std::sin(ang));
    for (int i = 0; i < n; i += len) {
      float wr = 1.0f, wi = 0.0f;
      for (int k = 0; k < len / 2; ++k) {
        const float ur = re[i + k], ui = im[i + k];
        const float vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
        const float vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
        re[i + k] = ur + vr;
        im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr;
        im[i + k + len / 2] = ui - vi;
        const float nwr = wr * wlr - wi * wli;
        wi = wr * wli + wi * wlr;
        wr = nwr;
      }
    }
  }
  if (inverse) {
    const float inv = 1.0f / n;
    for (int i = 0; i < n; ++i) {
      re[i] *= inv;
      im[i] *= inv;
    }
  }
}

constexpr float kInScale = 1.0f / 32768.0f;
constexpr float kOutScale = 32768.0f;
constexpr float kEps = 1e-10f;

}  // namespace

SpectralDenoiser::SpectralDenoiser() {}

void SpectralDenoiser::SetLevel(int level) {
  if (level < 0) level = 0;
  if (level > 2) level = 2;
  level_ = level;
}

void SpectralDenoiser::EnsureInit() {
  if (inited_) return;
  inited_ = true;
  win_.resize(kWin);
  // sqrt-Hann periódica (ana=syn): producto = Hann → COLA=1.0 a 50% de solape.
  for (int n = 0; n < kWin; ++n) {
    const float hann = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * n / kWin));
    win_[n] = std::sqrt(hann);
  }
  in_.assign(kWin, 0.0f);
  ola_.assign(kWin, 0.0f);
  re_.assign(kFft, 0.0f);
  im_.assign(kFft, 0.0f);
  noise_pow_.assign(kBins, 0.0f);
  p_smooth_.assign(kBins, 0.0f);
  p_min_.assign(kBins, 1e9f);
  p_tmp_.assign(kBins, 1e9f);
  g_prev_.assign(kBins, 1.0f);
  xprev_pow_.assign(kBins, 0.0f);
  Reset();
}

void SpectralDenoiser::Reset() {
  if (!inited_) return;
  std::fill(in_.begin(), in_.end(), 0.0f);
  std::fill(ola_.begin(), ola_.end(), 0.0f);
  std::fill(noise_pow_.begin(), noise_pow_.end(), 0.0f);
  std::fill(p_smooth_.begin(), p_smooth_.end(), 0.0f);
  std::fill(p_min_.begin(), p_min_.end(), 1e9f);
  std::fill(p_tmp_.begin(), p_tmp_.end(), 1e9f);
  std::fill(g_prev_.begin(), g_prev_.end(), 1.0f);
  std::fill(xprev_pow_.begin(), xprev_pow_.end(), 0.0f);
  min_ctr_ = 0;
  warm_ = false;
  warm_ctr_ = 0;
}

void SpectralDenoiser::Process(float* data, int n) {
  if (level_ <= 0) return;  // off = passthrough bit-exact
  EnsureInit();
  if (n != kHop) return;    // contrato: solo bloques de 480 (feeder 48k)

  // 1) desplaza la entrada: in_ = [trama previa | trama nueva]
  for (int i = 0; i < kWin - kHop; ++i) in_[i] = in_[i + kHop];
  for (int i = 0; i < kHop; ++i) in_[kWin - kHop + i] = data[i] * kInScale;

  ProcessFrame();

  // 4) saca el hop más antiguo del acumulador y reescala a FloatS16
  for (int i = 0; i < kHop; ++i) {
    float v = ola_[i] * kOutScale;
    if (v > 32767.0f) v = 32767.0f;
    else if (v < -32768.0f) v = -32768.0f;
    data[i] = v;
  }
  // desplaza el acumulador y limpia la cola
  for (int i = 0; i < kWin - kHop; ++i) ola_[i] = ola_[i + kHop];
  for (int i = kWin - kHop; i < kWin; ++i) ola_[i] = 0.0f;
}

void SpectralDenoiser::ProcessFrame() {
  // ventana de análisis + zero-pad a kFft
  for (int i = 0; i < kWin; ++i) {
    re_[i] = in_[i] * win_[i];
    im_[i] = 0.0f;
  }
  for (int i = kWin; i < kFft; ++i) {
    re_[i] = 0.0f;
    im_[i] = 0.0f;
  }
  Fft(re_.data(), im_.data(), kFft, /*inverse=*/false);

  // parámetros por nivel
  const float oversub = (level_ >= 2) ? 2.4f : 1.4f;   // sobre-resta
  const float gmin = (level_ >= 2) ? 0.06f : 0.16f;    // suelo (-24 / -16 dB)

  // estimación de ruido + ganancia por bin (0..kBins-1)
  for (int k = 0; k < kBins; ++k) {
    const float pw = re_[k] * re_[k] + im_[k] * im_[k];  // |X|²

    // suavizado de potencia
    p_smooth_[k] = 0.7f * p_smooth_[k] + 0.3f * pw;

    // warm-up: las primeras tramas se asumen ruido → siembra el estimador
    if (!warm_) {
      noise_pow_[k] = (warm_ctr_ == 0) ? p_smooth_[k]
                                       : 0.9f * noise_pow_[k] + 0.1f * p_smooth_[k];
      p_min_[k] = p_smooth_[k];
      p_tmp_[k] = p_smooth_[k];
    }

    // mínimo corrido con SUBIDA LENTA: baja instantáneo al mínimo reciente,
    // sube ~+2 dB/s. Así un tono/voz SOSTENIDO no se confunde con ruido (el
    // mínimo no se dispara mientras hablas), pero sigue ruidos reales lentos.
    if (p_smooth_[k] < p_min_[k]) p_min_[k] = p_smooth_[k];
    else p_min_[k] *= 1.0005f;
    const float pmin = p_min_[k];

    // presencia de voz: razón señal/mínimo
    const float sr = p_smooth_[k] / (pmin + kEps);
    const float ppres = sr > 5.0f ? 1.0f : 0.0f;

    // actualización del ruido (MCRA): congela cuando hay voz (ppres≈1)
    const float ad = 0.85f + 0.15f * ppres;  // →1 con voz: ruido casi congelado
    if (warm_) noise_pow_[k] = ad * noise_pow_[k] + (1.0f - ad) * p_smooth_[k];

    const float npw = noise_pow_[k] + kEps;

    // SNR a posteriori y a priori (decision-directed, Ephraim–Malah)
    const float gamma = pw / npw;
    const float prio_inst = gamma > 1.0f ? (gamma - 1.0f) : 0.0f;
    float xi = 0.98f * (g_prev_[k] * g_prev_[k] * xprev_pow_[k] / npw) +
               0.02f * prio_inst;
    xi /= oversub;  // agresividad
    if (xi < 1e-6f) xi = 1e-6f;

    // ganancia de Wiener con suelo (la voz nunca se cierra del todo)
    float g = xi / (1.0f + xi);
    if (g < gmin) g = gmin;

    g_prev_[k] = g;
    xprev_pow_[k] = pw;

    // aplica al bin y a su espejo conjugado
    re_[k] *= g;
    im_[k] *= g;
    if (k > 0 && k < kFft - k) {
      re_[kFft - k] *= g;
      im_[kFft - k] *= g;
    }
  }

  if (!warm_ && ++warm_ctr_ >= 8) warm_ = true;

  // IFFT y overlap-add con ventana de síntesis
  Fft(re_.data(), im_.data(), kFft, /*inverse=*/true);
  for (int i = 0; i < kWin; ++i) ola_[i] += re_[i] * win_[i];
}

}  // namespace chatpapol

// test_spectral_ns.cc — verificación NUMÉRICA del supresor (sin oído).
// Compila: g++ -O2 -I../include test_spectral_ns.cc ../src/spectral_ns.cc -o /tmp/tns
// Mide: reducción de ruido en silencio y preservación del tono de "voz".
#include "spectral_ns.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace chatpapol;

static double goertzel(const std::vector<float>& x, int start, int len, double freq, double sr) {
  const double w = 2.0 * M_PI * freq / sr;
  const double c = 2.0 * std::cos(w);
  double s0 = 0, s1 = 0, s2 = 0;
  for (int i = 0; i < len; ++i) {
    s0 = x[start + i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - c * s1 * s2;  // energía
}

static double rms(const std::vector<float>& x, int start, int len) {
  double s = 0;
  for (int i = 0; i < len; ++i) s += double(x[start + i]) * x[start + i];
  return std::sqrt(s / len);
}

int main() {
  const int sr = 48000;
  const int N = sr * 3;  // 3 s
  const double tone = 400.0;
  std::mt19937 rng(1234);
  std::normal_distribution<float> noise(0.0f, 900.0f);  // ruido blanco ±~900

  std::vector<float> in(N), out;
  // 0..1s silencio (solo ruido); 1..3s voz (tono 6000) + ruido
  for (int i = 0; i < N; ++i) {
    float v = noise(rng);
    if (i >= sr) v += 6000.0f * std::sin(2.0 * M_PI * tone * i / sr);
    in[i] = v;
  }
  out = in;  // se procesa in-place sobre out

  const int lvl = (std::getenv("LVL") ? atoi(std::getenv("LVL")) : 1);
  SpectralDenoiser d;
  d.SetLevel(lvl);
  for (int i = 0; i + 480 <= N; i += 480) d.Process(&out[i], 480);
  printf("(nivel=%d)\n", lvl);

  // NaN/inf?
  bool bad = false;
  for (float v : out) if (!std::isfinite(v)) { bad = true; break; }

  // métricas (deja margen para warm-up y latencia: usa zonas internas)
  const double inSil = rms(in, sr / 2, sr / 4);          // silencio (solo ruido)
  const double outSil = rms(out, sr / 2, sr / 4);
  const double redDb = 20.0 * std::log10((outSil + 1e-9) / (inSil + 1e-9));

  const int vStart = sr + sr / 2, vLen = sr;             // zona de voz
  const double inTone = goertzel(in, vStart, vLen, tone, sr);
  const double outTone = goertzel(out, vStart, vLen, tone, sr);
  const double toneKeep = 10.0 * std::log10((outTone + 1e-9) / (inTone + 1e-9));

  // ruido de banda ancha durante la voz: energía lejos del tono (vía RMS - tono)
  printf("=== SpectralDenoiser test ===\n");
  printf("finito (sin NaN/inf): %s\n", bad ? "NO ✗" : "SÍ ✓");
  printf("ruido en silencio:  in=%.1f  out=%.1f  → %.1f dB (negativo = limpia)\n",
         inSil, outSil, redDb);
  printf("tono de voz (400Hz): %.1f dB conservado (0 = intacto)\n", toneKeep);

  const bool ok = !bad && redDb < -6.0 && toneKeep > -3.0;
  printf("RESULTADO: %s\n", ok ? "PASA ✓ (limpia ruido y conserva la voz)" : "REVISAR ✗");
  return ok ? 0 : 1;
}

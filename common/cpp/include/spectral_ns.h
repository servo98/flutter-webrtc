// spectral_ns.h — [chatpapol] Supresor de ruido espectral propio (Wiener + MCRA).
//
// TEORÍA (clásica, replicable, sin GPU, ~CPU baja):
//   - STFT con WOLA: ventana sqrt-Hann de 960 (20ms@48k), hop 480 (10ms, 50%
//     solape), FFT 1024 (zero-pad). sqrt-Hann análisis+síntesis → COLA exacta.
//   - Estimación de ruido tipo MCRA / Minimum-Statistics: rastrea el piso de
//     ruido por bin INCLUSO durante el habla (un gate no puede), vía mínimo
//     corrido + probabilidad de presencia de voz.
//   - SNR a priori "decision-directed" (Ephraim–Malah) → suprime el ruido
//     musical típico de la sustracción espectral cruda.
//   - Ganancia de Wiener G = ξ/(1+ξ), con SUELO de ganancia (gain floor) que
//     NUNCA cierra del todo → la voz no se "agacha" (el problema de RNNoise@48k).
//   - Agresividad regulable (Off / Estándar / Fuerte) = suelo + sobre-resta.
//
// Diseñado para 48 kHz mono, bloques de 480 muestras (el feeder kCustom).
// Escala de entrada/salida: FloatS16 (±32768), igual que el resto del path.
// Realtime-safe: cero malloc en estado estable; todo preasignado en Init.
#ifndef CHATPAPOL_SPECTRAL_NS_H_
#define CHATPAPOL_SPECTRAL_NS_H_

#include <vector>
#include <cstdint>

namespace chatpapol {

class SpectralDenoiser {
 public:
  SpectralDenoiser();

  // level: 0 = off (bypass bit-exact), 1 = estándar, 2 = fuerte.
  void SetLevel(int level);
  int level() const { return level_; }

  // Procesa IN-PLACE un bloque mono de [n] muestras a 48 kHz (escala ±32768).
  // n debe ser 480 (hop). Si el rate/n cambian, se reinicia el estado.
  void Process(float* data, int n);

  // Resetea el estado (envolventes, mínimos, buffers de solape). Llamar al
  // arrancar la captura para no arrastrar estado viejo.
  void Reset();

 private:
  void EnsureInit();
  void ProcessFrame();  // una trama STFT (usa in_/ola_/spec_*)

  int level_ = 1;

  static constexpr int kHop = 480;
  static constexpr int kWin = 960;
  static constexpr int kFft = 1024;
  static constexpr int kBins = kFft / 2 + 1;  // 513

  bool inited_ = false;

  // Ventanas sqrt-Hann (análisis y síntesis = misma).
  std::vector<float> win_;          // [kWin]

  // Buffers de streaming.
  std::vector<float> in_;           // [kWin] últimas 2 tramas de entrada
  std::vector<float> ola_;          // [kWin] acumulador overlap-add de salida

  // Trabajo STFT.
  std::vector<float> re_, im_;      // [kFft] espectro (full complex)

  // Estado del estimador por bin.
  std::vector<float> noise_pow_;    // [kBins] potencia de ruido estimada
  std::vector<float> p_smooth_;     // [kBins] potencia suavizada de la señal
  std::vector<float> p_min_;        // [kBins] mínimo corrido (minimum statistics)
  std::vector<float> p_tmp_;        // [kBins] mínimo de la ventana en curso
  std::vector<float> g_prev_;       // [kBins] ganancia previa (decision-directed)
  std::vector<float> xprev_pow_;    // [kBins] |X|² previo (decision-directed)
  int min_ctr_ = 0;                 // contador de subventana del mínimo
  bool warm_ = false;              // ya inicializó el ruido con las 1ªs tramas
  int warm_ctr_ = 0;
};

}  // namespace chatpapol

#endif  // CHATPAPOL_SPECTRAL_NS_H_

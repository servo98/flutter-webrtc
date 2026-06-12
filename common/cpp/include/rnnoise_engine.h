// rnnoise_engine.h — motor de supresión de ruido por canal (RNNoise @48k).
// NO depende de libwebrtc: procesable y testeable de forma aislada.
//
// Trabaja en escala FloatS16 (floats en rango int16, NO normalizado [-1,1]),
// que es lo que entrega el capture post-processing de webrtc-sdk libwebrtc.
// RNNoise también opera en esa escala, así que NO hay conversión de escala.
//
// Si el rate != 48000, resamplea rate->48k antes de RNNoise y 48k->rate después
// (RNNoise es estrictamente 48k/480). Mantiene estado continuo entre llamadas
// (streaming sin clicks) con un pequeño retardo de arranque (priming).
#ifndef CHATPAPOL_RNNOISE_ENGINE_H_
#define CHATPAPOL_RNNOISE_ENGINE_H_

#include <vector>

struct DenoiseState;            // rnnoise.h (opaco)
struct SpeexResamplerState_;    // speex_resampler.h (opaco)
typedef struct SpeexResamplerState_ SpeexResamplerState;

namespace chatpapol {

class RnnoiseEngine {
 public:
  RnnoiseEngine();
  ~RnnoiseEngine();

  // (Re)inicializa para [sample_rate_hz]. Rates no soportados (<=0) => passthrough.
  void Reset(int sample_rate_hz);

  // Procesa [n] muestras in-place (escala FloatS16). No-op si passthrough.
  void ProcessInPlace(float* data, int n);

 private:
  void Free();

  int rate_ = 0;
  bool passthrough_ = true;  // true => no toca el audio
  bool native48_ = false;    // rate == 48000 => sin resample

  DenoiseState* rnn_ = nullptr;
  SpeexResamplerState* up_ = nullptr;    // rate -> 48000
  SpeexResamplerState* down_ = nullptr;  // 48000 -> rate

  std::vector<float> in48_;    // muestras @48k pendientes de RNNoise
  std::vector<float> den48_;   // muestras @48k ya denoised, pendientes de bajar
  std::vector<float> outq_;    // salida @rate lista para emitir
  std::vector<float> scratch_; // buffer temporal de resample
  bool primed_ = false;        // ya se inyectó el retardo de arranque
};

}  // namespace chatpapol

#endif  // CHATPAPOL_RNNOISE_ENGINE_H_

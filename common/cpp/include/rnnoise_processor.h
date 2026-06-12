// rnnoise_processor.h — post-procesador de captura COMBINADO de ChatPapol.
//
// Implementa la interfaz CustomProcessing del capture post-processing de
// webrtc-sdk libwebrtc y aplica, sobre la banda 0 (0-8kHz @16k):
//   1) RNNoise (supresor de ruido)         — si rnnoise_on_
//   2) la cadena de efectos de voz voicefx — si fx_on_ y hay nodos
//
// Cuando los efectos están activos, además pone a CERO las bandas altas
// (8kHz+): así la salida fullband contiene SOLO la banda 0 procesada → una voz
// de 16kHz con el efecto correcto (pitch/formantes incluidos), en vez de un mix
// incoherente (banda baja con efecto + banda alta seca). Con efectos OFF no
// toca las bandas altas (calidad 48k intacta).
//
// El mic es mono en la práctica (chans_==1), donde el layout de bandas es
// inequívoco (banda b en offset b*num_frames).
#ifndef CHATPAPOL_RNNOISE_PROCESSOR_H_
#define CHATPAPOL_RNNOISE_PROCESSOR_H_

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "rnnoise_engine.h"
#include "rtc_audio_processing.h"  // libwebrtc::RTCAudioProcessing::CustomProcessing
#include "voicefx.h"               // C ABI de los efectos de voz

namespace chatpapol {

// Nodo de la cadena de efectos ya parseado (en el hilo de plataforma) para no
// parsear strings en el hilo de audio.
struct VfxNodeSpec {
  int type = 0;
  bool bypass = false;
  std::vector<std::pair<int, float>> params;  // (paramId, valor)
};

class RnnoiseProcessor
    : public libwebrtc::RTCAudioProcessing::CustomProcessing {
 public:
  RnnoiseProcessor() = default;
  ~RnnoiseProcessor() override { DestroyFx(); }

  // --- CustomProcessing (hilo de audio) ---
  void Initialize(int sample_rate_hz, int num_channels) override;
  void Process(int num_bands, int num_frames, int buffer_size,
               float* buffer) override;
  void Reset(int new_rate) override;
  void Release() override;

  // --- control (hilo de plataforma; protegido por mu_) ---
  void SetRnnoise(bool on);
  void SetVoiceFx(bool enabled, const std::string& spec);
  bool active();  // ¿hay algo que procesar? (para registrar/desregistrar el APM)

 private:
  void RebuildFxLocked(int band_rate, int frames);  // requiere mu_ tomado
  void DestroyFx();                                  // requiere mu_ tomado

  std::mutex mu_;
  int rate_ = 0;
  int chans_ = 0;

  // RNNoise (uno por canal)
  std::vector<std::unique_ptr<RnnoiseEngine>> engines_;
  bool rnnoise_on_ = false;

  // voicefx
  bool fx_on_ = false;
  bool fx_dirty_ = true;     // hay que reconstruir las cadenas nativas
  int fx_rate_ = 0;          // rate de banda con el que se crearon las cadenas
  int fx_frames_ = 0;        // maxFrames con el que se crearon
  float fx_wet_ = 1.0f;
  float fx_gain_ = 1.0f;
  std::vector<VfxNodeSpec> fx_parsed_;  // cadena parseada (control thread)
  std::vector<VfxChain*> fx_chains_;    // una cadena mono por canal
  std::vector<float> fx_scratch_;       // buffer [-1,1] reutilizable
};

}  // namespace chatpapol

#endif  // CHATPAPOL_RNNOISE_PROCESSOR_H_

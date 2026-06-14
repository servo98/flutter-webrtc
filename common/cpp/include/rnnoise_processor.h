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
#include "spectral_ns.h"  // [chatpapol] supresor espectral propio (path 48k)
#include "rtc_audio_processing.h"  // libwebrtc::RTCAudioProcessing::CustomProcessing

// Handle opaco de voicefx, forward-declarado: así este header PÚBLICO no arrastra
// voicefx.h a los dependientes (p.ej. livekit_client) que no tienen su include
// dir. El .cc sí incluye voicefx.h para llamar a la C ABI.
struct VfxChain;

// [chatpapol] Estado del monitor local ("escucharme"): reproductor waveOut
// (Windows). Forward-decl (PIMPL) para no arrastrar windows.h a este header
// público; se define en el .cc.
struct VfxMonitorState;

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
  // ctor y dtor van EN EL .cc: monitor_ (unique_ptr<VfxMonitorState>) es de tipo
  // incompleto aquí, así que las funciones que lo destruyen deben compilarse
  // donde VfxMonitorState está completo (si no, otros TU que construyan un
  // RnnoiseProcessor fallan con "can't delete an incomplete type").
  RnnoiseProcessor();
  ~RnnoiseProcessor() override;

  // --- CustomProcessing (hilo de audio) ---
  void Initialize(int sample_rate_hz, int num_channels) override;
  void Process(int num_bands, int num_frames, int buffer_size,
               float* buffer) override;
  void Reset(int new_rate) override;
  void Release() override;

  // --- control (hilo de plataforma; protegido por mu_) ---
  void SetRnnoise(bool on);
  void SetVoiceFx(bool enabled, const std::string& spec);
  // Monitor local ("escucharme"): reproduce el micro YA PROCESADO en los
  // altavoces locales (solo suena mientras hay captura, p.ej. en un canal de
  // voz). Solo Windows por ahora; no-op en otras plataformas.
  void SetMonitor(bool on);
  // [chatpapol] Boost de captura (volumen de entrada): multiplicador lineal del
  // micro, 0..~3 (1.0 = sin cambio). Lo controla el usuario; reemplaza al AGC
  // siempre-on del path 48k. Se aplica con soft-clip para no reventar.
  void SetInputGain(float gain);
  // [chatpapol] Nivel del supresor espectral del path 48k: 0=off, 1=estándar,
  // 2=fuerte. Reemplaza al viejo gate+AGC. (El path 16k sigue usando RNNoise.)
  void SetNsLevel(int level);
  bool active();  // ¿hay algo que procesar? (para registrar/desregistrar el APM)

  // --- Ruta de micro kCustom 48k (Stage 4) ---
  // El micro custom NO pasa por el APM (que baja a 16k): un capturador nativo
  // (Stage 2) entrega 48 kHz mono FULLBAND y llama a estos métodos directamente
  // (no vía CustomProcessing::Process). InitializeCustom48 prepara 1 motor
  // RNNoise a 48k (su rate nativo) y marca la cadena voicefx para recrearse a
  // 48k. ProcessCustom48 aplica RNNoise + voicefx + monitor in-place sobre el
  // buffer 48k mono (escala FloatS16, igual que el APM): SIN band-split, SIN el
  // memset de bandas altas, SIN resample 16k<->48k.
  void InitializeCustom48();
  void ProcessCustom48(float* data, int num_frames);

 private:
  void RebuildFxLocked(int band_rate, int frames);  // requiere mu_ tomado
  void DestroyFx();                                  // requiere mu_ tomado
  // Empuja band-0 del canal 0 (ya procesado) al reproductor de monitor.
  void EmitMonitorLocked(int num_frames, const float* band0);  // requiere mu_

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

  // [chatpapol 48k] Boost de captura (volumen de entrada) + supresor espectral
  // propio. Reemplazan al viejo AGC+gate siempre-on (que machacaba el audio
  // incluso sin filtros). El boost lo controla el usuario; la NS es Wiener+MCRA.
  float input_gain_ = 1.0f;   // multiplicador de captura (1.0 = sin cambio)
  int ns_level_ = 1;          // 0 off · 1 estándar · 2 fuerte
  SpectralDenoiser ns48_;     // supresor espectral del path 48k

  // monitor local ("escucharme")
  bool monitor_on_ = false;
  std::unique_ptr<VfxMonitorState> monitor_;  // reproductor waveOut; null si off
};

}  // namespace chatpapol

#endif  // CHATPAPOL_RNNOISE_PROCESSOR_H_

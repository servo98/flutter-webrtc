#ifndef CHATPAPOL_MIC_CAPTURER_H_
#define CHATPAPOL_MIC_CAPTURER_H_

// [chatpapol 48k — Stage 2] Capturador de micrófono nativo a 48 kHz fullband que
// alimenta una fuente de audio kCustom (sin APM → sin downsample a 16k) vía
// source->CaptureFrame(feed, 16, 48000, 1, 480) en frames de 10 ms. Es el análogo
// del ApplicationLoopbackCapturer (screenshare-con-audio) pero para el MICRO:
// mismo patrón ring-buffer + hilo de captura + hilo feeder con reloj pacing y
// drift-compensation (probado, evita la carrera del AudioSendStream).
//
// SIN AEC: al no pasar por el APM no hay cancelación de eco → AURICULARES
// OBLIGATORIOS. voicefx/RNNoise a 48k se enganchan en FeederTick vía un
// RnnoiseProcessor opcional (Stage 4): ProcessCustom48 hace RNNoise + voicefx +
// monitor "escucharme" sobre el buffer 48k mono, IN-PLACE.
//
// Implementaciones por plataforma: mic_capturer_windows.cc (WASAPI shared) y
// mic_capturer_linux.cc (PulseAudio simple; va contra PipeWire vía pipewire-pulse).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libwebrtc.h"
#include "rtc_audio_source.h"

// [chatpapol 48k — Stage 4] Procesador opcional (RNNoise/voicefx/monitor a 48k).
// Forward-decl para no arrastrar rnnoise_processor.h a este header.
namespace chatpapol {
class RnnoiseProcessor;
}

namespace flutter_webrtc_plugin {

using namespace libwebrtc;

// Salida fija hacia la fuente kCustom: 48 kHz, mono, int16, frames de 10 ms.
static constexpr int kMicSampleRate = 48000;
static constexpr size_t kMicChannels = 1;
static constexpr size_t kMicFramesPer10ms = kMicSampleRate / 100;  // 480

class MicCapturer {
 public:
  MicCapturer();
  ~MicCapturer();

  // [chatpapol 48k — Stage 4] Procesador 48k a aplicar en cada FeederTick antes
  // de CaptureFrame (RNNoise + voicefx + monitor). Opcional; null = PCM crudo.
  // Llamar ANTES de Start().
  void SetFxProcessor(chatpapol::RnnoiseProcessor* fx) { fx_ = fx; }

  // Arranca la captura del dispositivo `device_id` (vacío = por defecto) y
  // empieza a alimentar `source` a 48k/mono/int16. Devuelve false si no se pudo
  // abrir el dispositivo o el formato. No bloquea: lanza sus hilos.
  bool Start(scoped_refptr<RTCAudioSource> source, const std::string& device_id);

  // Para los hilos y libera el dispositivo. Idempotente.
  void Stop();

  bool running() const { return running_.load(); }

 private:
  // Hilo que lee del dispositivo (bloqueante en Linux / event-driven en Win) y
  // escribe muestras mono int16 @48k en el ring.
  void CaptureThread();
  // Hilo paced a 10 ms que consume 480 frames del ring y los empuja a la fuente.
  void FeederThread();
  // Empuja exactamente kMicFramesPer10ms a la fuente (punto de enganche voicefx).
  void FeederTick();

  // --- Ring buffer (CaptureThread escribe, FeederThread lee) ---
  void RingWrite(const int16_t* samples, size_t num_frames);
  size_t RingRead(int16_t* out, size_t want_frames);  // devuelve frames leídos

  scoped_refptr<RTCAudioSource> source_;
  chatpapol::RnnoiseProcessor* fx_ = nullptr;  // [chatpapol 48k] Stage 4 (opcional)
  std::string device_id_;
  std::atomic<bool> running_{false};
  std::thread capture_thread_;
  std::thread feeder_thread_;

  std::mutex ring_mutex_;
  std::vector<int16_t> ring_buf_;        // mono int16
  size_t ring_capacity_frames_ = 0;
  size_t ring_write_frame_ = 0;
  size_t ring_read_frame_ = 0;
  size_t ring_frames_avail_ = 0;

  // Buffer reutilizado por FeederTick.
  std::vector<int16_t> feed_;
  std::vector<float> feed_f_;  // [chatpapol 48k] scratch float para ProcessCustom48

  // Estado opaco de plataforma (WASAPI / Pulse). Lo gestiona cada .cc.
  struct PlatformState;
  PlatformState* plat_ = nullptr;
};

}  // namespace flutter_webrtc_plugin

#endif  // CHATPAPOL_MIC_CAPTURER_H_

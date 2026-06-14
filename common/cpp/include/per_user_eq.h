// per_user_eq.h — [chatpapol] EQ por-usuario, individual y LOCAL.
//
// OBJETIVO: cuando el dueño ajusta graves/medios/agudos + volumen de UN amigo,
// SOLO él lo oye así y SOLO afecta a ESE amigo. Implementación:
//   - Por cada pista remota con EQ activo se registra un PerUserEqSink vía
//     RTCAudioTrack::AddSink: recibe el PCM decodificado de ESA pista en OnData.
//   - El sink aplica 3 biquads (low-shelf 250Hz / peak 1kHz / high-shelf 6kHz)
//     + ganancia, y reproduce el resultado por su propia salida waveOut local.
//   - Para evitar doble salida, el caller (flutter_webrtc.cc) hace
//     RTCAudioTrack::SetVolume(0) sobre esa pista (silencia el playout nativo de
//     WebRTC) mientras el EQ está activo, y lo restaura al limpiar.
//
// GATING DE SEGURIDAD: este módulo SOLO existe para pistas explícitamente
// eq-eadas. Si el usuario no tiene EQ en nadie, Dart no llama a nada de aquí,
// no se instala ningún sink, no se toca SetVolume → playout WebRTC 100% intacto.
//
// THREADING:
//   - OnData() corre en el hilo de audio del ADM/receive-stream de WebRTC.
//     Debe ser REALTIME-SAFE: sin locks que el hilo de control pueda retener,
//     sin malloc en estado estable. NO dropeamos frames (audio continuo: dropear
//     = clicks). La creación de la cadena ocurre en OnData (el hilo de audio es
//     dueño del puntero), guiada por parámetros atómicos que pone el control.
//   - SetEq()/SetGain() corren en el hilo de plataforma. Solo escriben atómicos;
//     la cadena voicefx admite vfx_set_param concurrente con vfx_process (ABI).
//
// PLATAFORMA: la salida real (WaveOutPlayer) solo suena en Windows. En Linux la
// salida es no-op; el EQ NO debe activarse en Linux (gating en Dart por
// Platform.isWindows) porque SetVolume(0) en el caller silenciaría al amigo sin
// reproducirlo por la ruta custom → MUTE. Ver flutter_webrtc.cc (rechaza no-Win).
//
// INCIERTO BLOQUEANTE (probar de oído / con logs en el primer test):
//   (1) ¿AddSink entrega PCM remoto decodificado en el libwebrtc m144 prebuilt?
//   (2) ¿SetVolume(0) silencia el playout PERO deja fluir el PCM al sink, o lo
//       pone a ceros? Si lo pone a ceros, este approach no es viable tal cual.
// per_user_eq.cc loguea en el PRIMER OnData por pista {bits,rate,ch,frames,rms}
// para responder ambas con datos reales (ver kEqProbeLog).
#ifndef CHATPAPOL_PER_USER_EQ_H_
#define CHATPAPOL_PER_USER_EQ_H_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rtc_media_track.h"  // libwebrtc::AudioTrackSink

// Handle opaco de voicefx (la C ABI vive en el .cc; no arrastrar voicefx.h aquí).
struct VfxChain;

namespace chatpapol {

struct WaveOutPlayer;  // wave_out_player.h (incluido solo en el .cc)

// Un sink por pista remota con EQ activo. Vive entre el hilo de audio (OnData) y
// el de control (SetEq/SetGain). Posee su propia cadena voicefx y su salida.
class PerUserEqSink : public libwebrtc::AudioTrackSink {
 public:
  explicit PerUserEqSink(std::string trackId);
  ~PerUserEqSink() override;

  // --- hilo de audio de WebRTC ---
  void OnData(const void* audio_data, int bits_per_sample, int sample_rate,
              size_t number_of_channels, size_t number_of_frames) override;

  // --- hilo de control (plataforma) ---
  static constexpr int kBands = 8;  // EQ de 8 bandas (freqs fijas en el .cc)
  void SetEq(const std::vector<float>& gainsDb);  // kBands valores, -12..+12 dB
  void SetGain(float gainLinear);                          // 0..~4 (outVol*userVol)

 private:
  void EnsureChainAudioThread(int rate, int max_frames);  // crea/recrea la chain

  std::string track_id_;

  // Parámetros: el control thread escribe atómicos; el audio thread los lee y
  // los empuja a la chain con vfx_set_param (ABI: seguro concurrente con
  // vfx_process). dirty_ marca que hay cambios pendientes de aplicar a la chain.
  std::atomic<float> gains_[kBands];  // ganancia por banda (dB); init 0 en el ctor
  std::atomic<float> gain_{1.0f};
  std::atomic<bool> params_dirty_{true};

  // Propiedad EXCLUSIVA del hilo de audio (creados/leídos/destruidos en OnData
  // y en el dtor cuando ya no hay OnData en vuelo —garantizado por el orden
  // RemoveSink-antes-de-destruir del caller—). No tocar desde el control thread.
  VfxChain* chain_ = nullptr;
  int chain_rate_ = 0;
  int chain_frames_ = 0;
  std::vector<float> mono_;  // scratch downmix [-1,1]
  std::unique_ptr<WaveOutPlayer> out_;

  // Probe del primer frame (responde a los inciertos bloqueantes). Solo loguea
  // una vez por sink.
  std::atomic<bool> logged_first_{false};
};

// Orquestador: dueño de los sinks por trackId. Lo posee FlutterWebRTCBase. NO
// toca RTCAudioTrack (Add/RemoveSink/SetVolume los hace el caller, que ya tiene
// MediaTrackForId); el router solo administra el ciclo de vida de los sinks.
class PerUserEqRouter {
 public:
  PerUserEqRouter() = default;
  ~PerUserEqRouter();

  // Devuelve el sink existente o crea uno nuevo. Llamado en el hilo de control.
  PerUserEqSink* GetOrCreate(const std::string& trackId);
  // Devuelve el sink o nullptr. Hilo de control.
  PerUserEqSink* Find(const std::string& trackId);
  // Destruye el sink. El caller DEBE haber hecho RemoveSink ANTES (si no, el
  // hilo de audio podría llamar OnData sobre un objeto destruido). Hilo control.
  void Remove(const std::string& trackId);
  // Destruye todos los sinks (al desconectar). El caller debe haber quitado los
  // sinks de sus pistas antes (o las pistas ya no existen).
  void Clear();

 private:
  std::mutex mu_;  // protege el mapa (solo control thread; OnData no lo toca)
  std::map<std::string, std::unique_ptr<PerUserEqSink>> sinks_;
};

}  // namespace chatpapol

#endif  // CHATPAPOL_PER_USER_EQ_H_

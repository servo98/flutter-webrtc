// per_user_eq.cc — ver per_user_eq.h.
#include "per_user_eq.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#ifdef _WIN32
#pragma warning(disable : 4996)  // getenv
#endif

// [chatpapol] El PROBE va a un ARCHIVO (stderr lo descarta una app GUI de
// Windows). Ruta: %APPDATA%\dev.papol\chatpapol\logs\eq-probe.txt (junto a los
// session logs) / $HOME/eq-probe.txt en otros SO.
static void chatpapol_eq_probe_log(const char* line) {
#ifdef _WIN32
  const char* base = std::getenv("APPDATA");
  std::string path = base ? std::string(base) +
                                "\\dev.papol\\chatpapol\\logs\\eq-probe.txt"
                          : std::string("eq-probe.txt");
#else
  const char* base = std::getenv("HOME");
  std::string path =
      base ? std::string(base) + "/eq-probe.txt" : std::string("/tmp/eq-probe.txt");
#endif
  if (FILE* f = std::fopen(path.c_str(), "a")) {
    std::fputs(line, f);
    std::fclose(f);
  }
  std::fprintf(stderr, "%s", line);
  std::fflush(stderr);
}

// windows.h/mmsystem.h ANTES de wave_out_player.h (el cuerpo Windows del player
// usa sus tipos). En no-Windows wave_out_player.h es no-op puro.
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#include "voicefx.h"          // C ABI de los biquads
#include "wave_out_player.h"  // salida waveOut (Windows) / no-op (otros)

namespace chatpapol {

namespace {
// Bandas del EQ por-usuario (8, fijas; el usuario controla la ganancia dB de c/u).
// Banda 0 = low-shelf, 1..6 = peaking, 7 = high-shelf. Espaciado ~1 octava.
constexpr float kEqFreqs[PerUserEqSink::kBands] = {
    60.0f, 120.0f, 250.0f, 500.0f, 1000.0f, 2400.0f, 6000.0f, 12000.0f};
constexpr float kInv16 = 1.0f / 32768.0f;

inline float ClampDb(float db) {
  if (db > 15.0f) return 15.0f;   // rango voicefx VFX_P_BIQUAD_GAIN_DB
  if (db < -15.0f) return -15.0f;
  return db;
}
}  // namespace

PerUserEqSink::PerUserEqSink(std::string trackId)
    : track_id_(std::move(trackId)) {
  for (int i = 0; i < kBands; ++i)
    gains_[i].store(0.0f, std::memory_order_relaxed);  // atomic<float> no auto-init
}

PerUserEqSink::~PerUserEqSink() {
  // El caller garantiza RemoveSink ANTES de destruir → no hay OnData en vuelo.
  if (chain_) {
    vfx_destroy(chain_);
    chain_ = nullptr;
  }
  // out_ (unique_ptr<WaveOutPlayer>) se destruye solo → cierra waveOut.
}

void PerUserEqSink::SetEq(const std::vector<float>& gainsDb) {
  for (int i = 0; i < kBands; ++i) {
    const float g = i < static_cast<int>(gainsDb.size()) ? ClampDb(gainsDb[i]) : 0.0f;
    gains_[i].store(g, std::memory_order_relaxed);
  }
  params_dirty_.store(true, std::memory_order_release);
}

void PerUserEqSink::SetGain(float gainLinear) {
  if (gainLinear < 0.0f) gainLinear = 0.0f;
  if (gainLinear > 4.0f) gainLinear = 4.0f;  // tope sano
  gain_.store(gainLinear, std::memory_order_relaxed);
}

// Crea (o recrea si cambió el rate/frames) la cadena de 3 biquads. SOLO se llama
// desde OnData (hilo de audio): el chain_ es propiedad exclusiva de ese hilo.
// Patrón idéntico a RnnoiseProcessor::RebuildFxLocked, pero con nodos fijos.
void PerUserEqSink::EnsureChainAudioThread(int rate, int max_frames) {
  if (chain_ && chain_rate_ == rate && chain_frames_ >= max_frames) return;
  if (chain_) {
    vfx_destroy(chain_);
    chain_ = nullptr;
  }
  const int sr = rate > 0 ? rate : 48000;
  // Sembrar un max_frames GENEROSO fijo: así no recreamos la cadena (malloc en
  // el hilo de audio, NO realtime-safe) si la build entrega bloques de tamaño
  // variable. Solo se recrea si cambia el rate. (fix [BAJA] de la crítica.)
  const int mf = max_frames < 4096 ? 4096 : max_frames;
  chain_ = vfx_create(sr, mf);
  if (!chain_) return;
  vfx_clear(chain_);
  vfx_set_master(chain_, 1.0f, 1.0f);  // wet=1, gain lo aplicamos nosotros aparte
  // 8 nodos: banda 0 = low-shelf, 1..6 = peaking (Q~1.1, ~1 octava), 7 = high-shelf.
  for (int i = 0; i < kBands; ++i) {
    int n = vfx_add(chain_, VFX_BIQUAD);
    if (n < 0) break;  // tope VFX_MAX_NODES (16): 8 caben de sobra
    const int type = (i == 0)            ? VFX_BIQUAD_LOWSHELF
                     : (i == kBands - 1) ? VFX_BIQUAD_HIGHSHELF
                                         : VFX_BIQUAD_PEAKING;
    vfx_set_param(chain_, n, VFX_P_BIQUAD_TYPE, (float)type);
    vfx_set_param(chain_, n, VFX_P_BIQUAD_FREQ, kEqFreqs[i]);
    if (type == VFX_BIQUAD_PEAKING)
      vfx_set_param(chain_, n, VFX_P_BIQUAD_Q, 1.1f);
  }
  chain_rate_ = sr;
  chain_frames_ = mf;
  params_dirty_.store(true, std::memory_order_release);  // forzar aplicar dB
}

void PerUserEqSink::OnData(const void* audio_data, int bits_per_sample,
                           int sample_rate, size_t number_of_channels,
                           size_t number_of_frames) {
  const int frames = static_cast<int>(number_of_frames);
  const int chans = static_cast<int>(number_of_channels);
  if (audio_data == nullptr || frames <= 0 || chans <= 0) return;

  // --- PROBE del primer frame: responde los inciertos bloqueantes con datos
  // reales. Calcula RMS para distinguir "PCM real" de "ceros" (SetVolume(0)
  // mató el sink). Solo loguea una vez por sink. ---
  if (!logged_first_.exchange(true, std::memory_order_relaxed)) {
    double sumsq = 0.0;
    if (bits_per_sample == 16) {
      const int16_t* p = static_cast<const int16_t*>(audio_data);
      const size_t n = number_of_frames * number_of_channels;
      for (size_t i = 0; i < n; ++i) sumsq += (double)p[i] * (double)p[i];
      double rms = n ? std::sqrt(sumsq / (double)n) : 0.0;
      char buf[320];
      std::snprintf(buf, sizeof(buf),
          "[chatpapol][per_user_eq] PROBE track=%s bits=%d rate=%d ch=%zu "
          "frames=%zu rms16=%.1f%s\n",
          track_id_.c_str(), bits_per_sample, sample_rate, number_of_channels,
          number_of_frames, rms, rms < 1.0 ? "  <-- SILENCIO (revisar SetVolume(0))" : "");
      chatpapol_eq_probe_log(buf);
    } else {
      char buf[320];
      std::snprintf(buf, sizeof(buf),
          "[chatpapol][per_user_eq] PROBE track=%s bits=%d (NO 16-bit!) rate=%d "
          "ch=%zu frames=%zu\n",
          track_id_.c_str(), bits_per_sample, sample_rate, number_of_channels,
          number_of_frames);
      chatpapol_eq_probe_log(buf);
    }
  }

  // El sink de WebRTC entrega int16 interleaved (contrato AudioTrackSink). Si
  // alguna build entrega otro bits_per_sample, salimos (la salida quedaría muda
  // pero NO crashea; el probe ya lo habrá logueado para diagnosticar).
  if (bits_per_sample != 16) return;
  const int16_t* in = static_cast<const int16_t*>(audio_data);

  // Downmix a mono float [-1,1] (escala int16, igual que rnnoise_processor).
  if (static_cast<int>(mono_.size()) < frames) mono_.resize(frames);
  if (chans == 1) {
    for (int i = 0; i < frames; ++i) mono_[i] = (float)in[i] * kInv16;
  } else {
    // promedio de canales
    const float invc = 1.0f / (float)chans;
    for (int i = 0; i < frames; ++i) {
      int acc = 0;
      for (int c = 0; c < chans; ++c) acc += in[i * chans + c];
      mono_[i] = ((float)acc * invc) * kInv16;
    }
  }

  // Cadena de biquads al rate real de la pista (perezoso / re-crea si cambia).
  EnsureChainAudioThread(sample_rate, frames);
  if (chain_) {
    // Aplica los dB pendientes (el control thread solo marca dirty + atómicos).
    // vfx_set_param es seguro concurrente con vfx_process (ABI), y aquí ni
    // siquiera hay concurrencia: mismo hilo de audio.
    if (params_dirty_.exchange(false, std::memory_order_acquire)) {
      for (int i = 0; i < kBands; ++i)
        vfx_set_param(chain_, i, VFX_P_BIQUAD_GAIN_DB,
                      gains_[i].load(std::memory_order_relaxed));
    }
    vfx_process(chain_, mono_.data(), mono_.data(), frames);
  }

  // Ganancia (volumen por-usuario) tras el EQ, en escala int16 para el player.
  const float g = gain_.load(std::memory_order_relaxed);
  for (int i = 0; i < frames; ++i) mono_[i] = mono_[i] * 32768.0f * g;

  // Salida propia (waveOut en Windows; no-op en otros).
  if (!out_) out_.reset(new WaveOutPlayer());
  if (!out_->opened || out_->rate != sample_rate) {
    if (!out_->Open(sample_rate, frames)) return;
  }
  out_->Push(mono_.data(), frames);
}

// ---------------------------------------------------------------------------
// PerUserEqRouter
// ---------------------------------------------------------------------------
PerUserEqRouter::~PerUserEqRouter() { Clear(); }

PerUserEqSink* PerUserEqRouter::GetOrCreate(const std::string& trackId) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sinks_.find(trackId);
  if (it != sinks_.end()) return it->second.get();
  auto sink = std::make_unique<PerUserEqSink>(trackId);
  PerUserEqSink* raw = sink.get();
  sinks_[trackId] = std::move(sink);
  return raw;
}

PerUserEqSink* PerUserEqRouter::Find(const std::string& trackId) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sinks_.find(trackId);
  return it == sinks_.end() ? nullptr : it->second.get();
}

void PerUserEqRouter::Remove(const std::string& trackId) {
  std::lock_guard<std::mutex> lock(mu_);
  sinks_.erase(trackId);  // dtor del sink cierra chain + waveOut
}

void PerUserEqRouter::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  sinks_.clear();
}

}  // namespace chatpapol

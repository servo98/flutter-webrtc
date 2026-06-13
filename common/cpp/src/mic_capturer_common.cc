// [chatpapol 48k — Stage 2] Parte común del capturador de micro: ring buffer y
// el hilo feeder paced a 10 ms con drift-compensation. La captura cruda del
// dispositivo vive en mic_capturer_windows.cc / mic_capturer_linux.cc (definen
// PlatformState, CaptureThread y la apertura/cierre del dispositivo).
//
// El feeder replica el FeederThread del ApplicationLoopbackCapturer (probado en
// este fork): pre-buffer de arranque, tope duro de latencia y skip por drift
// para no enviar audio más rápido que tiempo real (evita flush del jitter buffer
// del receptor y la carrera del AudioSendStream).

#include "mic_capturer.h"

#include "rnnoise_processor.h"  // [chatpapol 48k — Stage 4] ProcessCustom48

#include <algorithm>
#include <chrono>
#include <cstring>

namespace flutter_webrtc_plugin {

void MicCapturer::RingWrite(const int16_t* samples, size_t num_frames) {
  if (ring_capacity_frames_ == 0 || num_frames == 0) return;
  std::lock_guard<std::mutex> lock(ring_mutex_);
  for (size_t f = 0; f < num_frames; ++f) {
    if (ring_frames_avail_ >= ring_capacity_frames_) {
      // Ring lleno: descarta el más viejo (preferible a bloquear el hilo).
      ring_read_frame_ = (ring_read_frame_ + 1) % ring_capacity_frames_;
      --ring_frames_avail_;
    }
    ring_buf_[ring_write_frame_] = samples[f];
    ring_write_frame_ = (ring_write_frame_ + 1) % ring_capacity_frames_;
    ++ring_frames_avail_;
  }
}

size_t MicCapturer::RingRead(int16_t* out, size_t want_frames) {
  std::lock_guard<std::mutex> lock(ring_mutex_);
  size_t got = 0;
  while (got < want_frames && ring_frames_avail_ > 0) {
    out[got] = ring_buf_[ring_read_frame_];
    ring_read_frame_ = (ring_read_frame_ + 1) % ring_capacity_frames_;
    --ring_frames_avail_;
    ++got;
  }
  return got;
}

void MicCapturer::FeederTick() {
  // Rellena `feed_` con 10 ms del ring (zeros si no hay suficiente).
  std::fill(feed_.begin(), feed_.end(), int16_t{0});
  RingRead(feed_.data(), kMicFramesPer10ms);

  // [chatpapol 48k — Stage 4] Procesado a 48k FUERA del APM (escala FloatS16,
  // NO normalizar): int16 -> float [-32768,32767], ProcessCustom48 hace RNNoise
  // + voicefx + monitor "escucharme" IN-PLACE, y de vuelta a int16. Si no hay
  // procesador, se envía el PCM crudo del micro.
  if (fx_) {
    if (feed_f_.size() < kMicFramesPer10ms) feed_f_.assign(kMicFramesPer10ms, 0.0f);
    for (size_t i = 0; i < kMicFramesPer10ms; ++i)
      feed_f_[i] = static_cast<float>(feed_[i]);
    fx_->ProcessCustom48(feed_f_.data(), static_cast<int>(kMicFramesPer10ms));
    for (size_t i = 0; i < kMicFramesPer10ms; ++i) {
      float v = feed_f_[i];
      if (v > 32767.0f) v = 32767.0f;
      else if (v < -32768.0f) v = -32768.0f;
      feed_[i] = static_cast<int16_t>(v);
    }
  }

  if (source_) {
    source_->CaptureFrame(feed_.data(), /*bits_per_sample=*/16,
                          /*sample_rate=*/kMicSampleRate,
                          /*number_of_channels=*/kMicChannels,
                          /*number_of_frames=*/kMicFramesPer10ms);
  }
}

void MicCapturer::FeederThread() {
  using Clock = std::chrono::steady_clock;

  const size_t target_prebuf = 8 * kMicFramesPer10ms;   // 80 ms de arranque
  const size_t max_buffered = 20 * kMicFramesPer10ms;   // 200 ms tope duro

  // Espera a pre-buffer (o a que paren).
  while (running_.load()) {
    {
      std::lock_guard<std::mutex> lock(ring_mutex_);
      if (ring_frames_avail_ >= target_prebuf) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  auto feeder_start = Clock::now();
  int64_t total_frames_del = 0;
  auto next_tick = Clock::now();

  while (running_.load()) {
    next_tick += std::chrono::milliseconds(10);
    std::this_thread::sleep_until(next_tick);
    if (!running_.load()) break;

    // Tope duro: recorta lo más viejo para no pasar de 200 ms de latencia.
    {
      std::lock_guard<std::mutex> lock(ring_mutex_);
      if (ring_frames_avail_ > max_buffered) {
        const size_t drop = ring_frames_avail_ - max_buffered;
        ring_read_frame_ = (ring_read_frame_ + drop) % ring_capacity_frames_;
        ring_frames_avail_ -= drop;
      }
    }

    // Drift: si vamos más de un frame por delante de tiempo real, salta el tick
    // (no consumir ring, no CaptureFrame) para mantener el ritmo nominal.
    const double elapsed_sec =
        std::chrono::duration<double>(Clock::now() - feeder_start).count();
    const int64_t expected =
        static_cast<int64_t>(elapsed_sec * kMicSampleRate);
    if (total_frames_del >
        expected + static_cast<int64_t>(kMicFramesPer10ms)) {
      continue;
    }

    FeederTick();
    total_frames_del += static_cast<int64_t>(kMicFramesPer10ms);
  }
}

}  // namespace flutter_webrtc_plugin

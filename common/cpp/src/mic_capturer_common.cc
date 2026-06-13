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

#ifdef _WIN32
// [chatpapol 48k] timing preciso del feeder en Windows: waitable timer (10 ms) +
// timeBeginPeriod + prioridad de audio MMCSS. Sin esto, sleep_until tickea a
// ~15.6 ms (resolución por defecto del SO) → el feeder entrega ~64 fps en vez de
// 100 → underrun constante → audio CORTADO. Mismo patrón que el loopback WASAPI.
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#endif

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
  // `feed_` ya viene relleno con 10 ms del ring (lo lee FeederThread bajo su
  // lógica de prebuffer/drift/cap). Aquí solo procesamos y empujamos.

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

  // Prebuffer 160 ms (igual que el loopback probado): más tolerancia al jitter
  // de la captura WASAPI/Pulse que los 80 ms anteriores (que se quedaban cortos
  // → underrun → cortes).
  const size_t target_prebuf = 16 * kMicFramesPer10ms;  // 160 ms de arranque
  const size_t max_buffered = 20 * kMicFramesPer10ms;   // 200 ms tope duro
  bool prebuffering = true;
  bool feeder_start_valid = false;
  int64_t total_frames_del = 0;
  auto feeder_start = Clock::now();

#ifdef _WIN32
  // Timing preciso (ver includes): MMCSS + timeBeginPeriod + waitable timer 10ms.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD task_index = 0;
  HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
  timeBeginPeriod(10);
  HANDLE timer = CreateWaitableTimerW(nullptr, /*manualReset=*/FALSE, nullptr);
  LARGE_INTEGER due = {};
  due.QuadPart = -100000LL;  // 10 ms inicial (unidades de 100 ns)
  SetWaitableTimer(timer, &due, /*period_ms=*/10, nullptr, nullptr, FALSE);
#else
  auto next_tick = Clock::now();
#endif

  while (running_.load()) {
#ifdef _WIN32
    if (WaitForSingleObject(timer, /*timeout_ms=*/40) == WAIT_FAILED) break;
#else
    next_tick += std::chrono::milliseconds(10);
    std::this_thread::sleep_until(next_tick);
#endif
    if (!running_.load()) break;

    // Drift: si vamos más de un frame por delante de tiempo real, salta el tick
    // para mantener el ritmo nominal (no sobrellenar el jitter buffer del rx).
    if (feeder_start_valid) {
      const double elapsed_sec =
          std::chrono::duration<double>(Clock::now() - feeder_start).count();
      const int64_t expected =
          static_cast<int64_t>(elapsed_sec * kMicSampleRate);
      if (total_frames_del >
          expected + static_cast<int64_t>(kMicFramesPer10ms)) {
        continue;
      }
    }

    // Tope duro + prebuffer bajo el lock.
    {
      std::lock_guard<std::mutex> lock(ring_mutex_);
      if (ring_frames_avail_ > max_buffered) {
        const size_t drop = ring_frames_avail_ - max_buffered;
        ring_read_frame_ = (ring_read_frame_ + drop) % ring_capacity_frames_;
        ring_frames_avail_ -= drop;
      }
      if (prebuffering) {
        if (ring_frames_avail_ >= target_prebuf) {
          prebuffering = false;
          feeder_start = Clock::now();  // arranca el reloj de drift AHORA
          feeder_start_valid = true;
        } else {
          continue;  // aún acumulando: no empujar todavía
        }
      }
    }

    // Lee 10 ms del ring (zeros si no hay suficiente) y procesa+empuja.
    std::fill(feed_.begin(), feed_.end(), int16_t{0});
    RingRead(feed_.data(), kMicFramesPer10ms);
    FeederTick();
    total_frames_del += static_cast<int64_t>(kMicFramesPer10ms);
  }

#ifdef _WIN32
  CancelWaitableTimer(timer);
  CloseHandle(timer);
  timeEndPeriod(10);
  if (task) AvRevertMmThreadCharacteristics(task);
  CoUninitialize();
#endif
}

}  // namespace flutter_webrtc_plugin

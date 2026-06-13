// [chatpapol 48k — Stage 2] Captura de micrófono Linux vía PulseAudio simple API
// (pa_simple_read). Pide PA_SAMPLE_S16NE / 48000 / 1 canal, así que entrega ya
// int16 mono 48k sin resampleo manual. En CachyOS (PipeWire) pa_simple va contra
// pipewire-pulse de forma transparente (no requiere Wayland ni API nativa de PW).
// El device_id es el `source name` de Pulse (p.ej. "alsa_input..."); vacío = por
// defecto. La lectura es bloqueante → corre en CaptureThread dedicado.
//
// Requiere libpulse-simple (ver linux/CMakeLists.txt). Si CHATPAPOL_HAVE_PULSE no
// está definido (pulse no encontrado en configure), Start() devuelve false y el
// method channel reporta el error en runtime.
#ifdef __linux__

#include "mic_capturer.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

#ifdef CHATPAPOL_HAVE_PULSE
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

namespace flutter_webrtc_plugin {

struct MicCapturer::PlatformState {
#ifdef CHATPAPOL_HAVE_PULSE
  pa_simple* pa = nullptr;
#endif
};

MicCapturer::MicCapturer() : plat_(new PlatformState()) {}
MicCapturer::~MicCapturer() {
  Stop();
  delete plat_;
  plat_ = nullptr;
}

bool MicCapturer::Start(scoped_refptr<RTCAudioSource> source,
                        const std::string& device_id) {
#ifndef CHATPAPOL_HAVE_PULSE
  (void)source;
  (void)device_id;
  std::cerr << "[MicCapturer] built without PulseAudio support.\n";
  return false;
#else
  if (running_.load()) return true;
  source_ = source;
  device_id_ = device_id;

  pa_sample_spec spec;
  spec.format = PA_SAMPLE_S16NE;
  spec.rate = static_cast<uint32_t>(kMicSampleRate);
  spec.channels = static_cast<uint8_t>(kMicChannels);

  // Latencia objetivo ~20 ms (en bytes): tlength/fragsize gestionados por pulse.
  pa_buffer_attr attr;
  std::memset(&attr, 0xff, sizeof(attr));  // -1 = deja que el server decida
  attr.fragsize =
      static_cast<uint32_t>(kMicFramesPer10ms * 2 * sizeof(int16_t));  // ~20 ms

  int err = 0;
  const char* dev = device_id.empty() ? nullptr : device_id.c_str();
  plat_->pa = pa_simple_new(/*server=*/nullptr, "chatpapol",
                            PA_STREAM_RECORD, dev, "mic48k", &spec,
                            /*channel_map=*/nullptr, &attr, &err);
  if (!plat_->pa) {
    std::cerr << "[MicCapturer] pa_simple_new failed: "
              << pa_strerror(err) << "\n";
    return false;
  }

  // Ring de 500 ms.
  ring_capacity_frames_ = 50 * kMicFramesPer10ms;
  ring_buf_.assign(ring_capacity_frames_, int16_t{0});
  ring_write_frame_ = ring_read_frame_ = ring_frames_avail_ = 0;
  feed_.assign(kMicFramesPer10ms, int16_t{0});

  running_.store(true);
  capture_thread_ = std::thread(&MicCapturer::CaptureThread, this);
  feeder_thread_ = std::thread(&MicCapturer::FeederThread, this);
  return true;
#endif
}

void MicCapturer::Stop() {
  running_.store(false);
  if (capture_thread_.joinable()) capture_thread_.join();
  if (feeder_thread_.joinable()) feeder_thread_.join();
#ifdef CHATPAPOL_HAVE_PULSE
  if (plat_ && plat_->pa) {
    pa_simple_free(plat_->pa);
    plat_->pa = nullptr;
  }
#endif
  source_ = nullptr;
}

void MicCapturer::CaptureThread() {
#ifdef CHATPAPOL_HAVE_PULSE
  // Lee bloques de 10 ms (480 muestras mono int16) y los empuja al ring.
  std::vector<int16_t> block(kMicFramesPer10ms, int16_t{0});
  const size_t block_bytes = block.size() * sizeof(int16_t);
  while (running_.load()) {
    int err = 0;
    if (pa_simple_read(plat_->pa, block.data(), block_bytes, &err) < 0) {
      std::cerr << "[MicCapturer] pa_simple_read failed: "
                << pa_strerror(err) << "\n";
      break;
    }
    RingWrite(block.data(), block.size());
  }
#endif
}

}  // namespace flutter_webrtc_plugin

#endif  // __linux__

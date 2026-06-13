// rnnoise_engine.cc — ver rnnoise_engine.h.
#include "rnnoise_engine.h"

#include <algorithm>
#include <cstdint>

extern "C" {
#include "rnnoise.h"               // rnnoise_create / process_frame / destroy
#include "speex/speex_resampler.h" // resampler (símbolos con RANDOM_PREFIX)
}

namespace chatpapol {

namespace {
constexpr int kRnnFrame = 480;       // rnnoise_get_frame_size() @48k
constexpr int kQuality = 5;          // calidad del resampler (0-10)
}  // namespace

RnnoiseEngine::RnnoiseEngine() = default;
RnnoiseEngine::~RnnoiseEngine() { Free(); }

void RnnoiseEngine::Free() {
  if (rnn_) { rnnoise_destroy(rnn_); rnn_ = nullptr; }
  if (up_) { speex_resampler_destroy(up_); up_ = nullptr; }
  if (down_) { speex_resampler_destroy(down_); down_ = nullptr; }
  in48_.clear();
  den48_.clear();
  outq_.clear();
  primed_ = false;
}

void RnnoiseEngine::Reset(int sample_rate_hz) {
  Free();
  rate_ = sample_rate_hz;
  if (sample_rate_hz <= 0) { passthrough_ = true; return; }
  passthrough_ = false;
  native48_ = (sample_rate_hz == 48000);
  rnn_ = rnnoise_create(nullptr);
  if (!rnn_) { passthrough_ = true; return; }
  if (!native48_) {
    int err = 0;
    up_ = speex_resampler_init(1, sample_rate_hz, 48000, kQuality, &err);
    down_ = speex_resampler_init(1, 48000, sample_rate_hz, kQuality, &err);
    if (!up_ || !down_) { Free(); passthrough_ = true; return; }
  }
}

void RnnoiseEngine::ProcessInPlace(float* data, int n) {
  if (passthrough_ || n <= 0 || data == nullptr) return;

  if (native48_) {
    // Sin resample: acumular y procesar en frames de 480.
    in48_.insert(in48_.end(), data, data + n);
    while (in48_.size() >= static_cast<size_t>(kRnnFrame)) {
      float out[kRnnFrame];
      last_vad_ = rnnoise_process_frame(rnn_, out, in48_.data());
      if (vad_preserve_) {
        // Suelo de supresión ALINEADO (in y out son el MISMO frame → sin
        // desfase → sin doblado). RNNoise reduce ruido pero la salida nunca
        // baja del kDry de la señal original → NUNCA corta la voz. No depende
        // del VAD (poco fiable en este path).
        constexpr float kDry = 0.30f;  // suelo: máx supresión ~ -10.5 dB
        const float* in = in48_.data();
        for (int i = 0; i < kRnnFrame; ++i) {
          out[i] = kDry * in[i] + (1.0f - kDry) * out[i];
        }
      }
      outq_.insert(outq_.end(), out, out + kRnnFrame);
      in48_.erase(in48_.begin(), in48_.begin() + kRnnFrame);
    }
  } else {
    // 1) up: rate -> 48k (streaming).
    int up_cap = static_cast<int>(static_cast<long>(n) * 48000 / rate_) + 32;
    scratch_.resize(up_cap);
    spx_uint32_t ilen = static_cast<spx_uint32_t>(n);
    spx_uint32_t olen = static_cast<spx_uint32_t>(up_cap);
    speex_resampler_process_float(up_, 0, data, &ilen, scratch_.data(), &olen);
    in48_.insert(in48_.end(), scratch_.data(), scratch_.data() + olen);

    // 2) RNNoise en frames de 480.
    while (in48_.size() >= static_cast<size_t>(kRnnFrame)) {
      float out[kRnnFrame];
      last_vad_ = rnnoise_process_frame(rnn_, out, in48_.data());
      den48_.insert(den48_.end(), out, out + kRnnFrame);
      in48_.erase(in48_.begin(), in48_.begin() + kRnnFrame);
    }

    // 3) down: 48k -> rate (consume lo que haya de den48_).
    if (!den48_.empty()) {
      int dn_cap =
          static_cast<int>(static_cast<long>(den48_.size()) * rate_ / 48000) + 32;
      scratch_.resize(dn_cap);
      spx_uint32_t ilen2 = static_cast<spx_uint32_t>(den48_.size());
      spx_uint32_t olen2 = static_cast<spx_uint32_t>(dn_cap);
      speex_resampler_process_float(down_, 0, den48_.data(), &ilen2,
                                    scratch_.data(), &olen2);
      den48_.erase(den48_.begin(), den48_.begin() + ilen2);
      outq_.insert(outq_.end(), scratch_.data(), scratch_.data() + olen2);
    }
  }

  // Priming: en la primera llamada inyecta 2*n ceros al frente de la cola de
  // salida para cubrir el retardo del arranque de la cadena (resamplers +
  // buffering de 480) y no hacer underrun nunca más. Añade ~2*10ms de latencia.
  if (!primed_) {
    outq_.insert(outq_.begin(), static_cast<size_t>(n) * 2, 0.0f);
    primed_ = true;
  }

  // Emitir n muestras desde la cola.
  if (static_cast<int>(outq_.size()) >= n) {
    std::copy(outq_.begin(), outq_.begin() + n, data);
    outq_.erase(outq_.begin(), outq_.begin() + n);
  } else {
    // Underrun (no debería ocurrir tras el priming): rellena lo disponible + 0.
    int have = static_cast<int>(outq_.size());
    std::copy(outq_.begin(), outq_.end(), data);
    for (int i = have; i < n; ++i) data[i] = 0.0f;
    outq_.clear();
  }
}

}  // namespace chatpapol

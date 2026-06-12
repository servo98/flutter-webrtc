// rnnoise_processor.cc — ver rnnoise_processor.h.
#include "rnnoise_processor.h"

namespace chatpapol {

void RnnoiseProcessor::Initialize(int sample_rate_hz, int num_channels) {
  std::lock_guard<std::mutex> lock(mu_);
  rate_ = sample_rate_hz;
  chans_ = num_channels > 0 ? num_channels : 1;
  engines_.clear();
  engines_.reserve(chans_);
  for (int c = 0; c < chans_; ++c) {
    auto e = std::make_unique<RnnoiseEngine>();
    e->Reset(sample_rate_hz);
    engines_.push_back(std::move(e));
  }
}

void RnnoiseProcessor::Reset(int new_rate) {
  std::lock_guard<std::mutex> lock(mu_);
  rate_ = new_rate;
  for (auto& e : engines_) e->Reset(new_rate);
}

void RnnoiseProcessor::Process(int num_bands, int num_frames, int buffer_size,
                               float* buffer) {
  std::lock_guard<std::mutex> lock(mu_);
  if (num_frames <= 0 || buffer == nullptr || engines_.empty()) return;
  // Layout deinterleaved: banda 0 contiene chans_ bloques de num_frames. Solo
  // procesamos la banda 0 (voz 0-8kHz); bandas altas se dejan intactas.
  for (int c = 0; c < chans_ && c < static_cast<int>(engines_.size()); ++c) {
    long off = static_cast<long>(c) * num_frames;
    if (off + num_frames > buffer_size) break;  // guard del buffer real
    engines_[c]->ProcessInPlace(buffer + off, num_frames);
  }
}

void RnnoiseProcessor::Release() {
  std::lock_guard<std::mutex> lock(mu_);
  engines_.clear();
  rate_ = 0;
  chans_ = 0;
}

}  // namespace chatpapol

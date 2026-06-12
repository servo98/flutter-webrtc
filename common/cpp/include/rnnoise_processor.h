// rnnoise_processor.h — adapta RnnoiseEngine a la interfaz CustomProcessing del
// capture post-processing de webrtc-sdk libwebrtc. Un RnnoiseEngine por canal.
#ifndef CHATPAPOL_RNNOISE_PROCESSOR_H_
#define CHATPAPOL_RNNOISE_PROCESSOR_H_

#include <memory>
#include <mutex>
#include <vector>

#include "rnnoise_engine.h"
#include "rtc_audio_processing.h"  // libwebrtc::RTCAudioProcessing::CustomProcessing

namespace chatpapol {

class RnnoiseProcessor
    : public libwebrtc::RTCAudioProcessing::CustomProcessing {
 public:
  RnnoiseProcessor() = default;
  ~RnnoiseProcessor() override = default;

  void Initialize(int sample_rate_hz, int num_channels) override;
  void Process(int num_bands, int num_frames, int buffer_size,
               float* buffer) override;
  void Reset(int new_rate) override;
  void Release() override;

 private:
  std::mutex mu_;
  int rate_ = 0;
  int chans_ = 0;
  std::vector<std::unique_ptr<RnnoiseEngine>> engines_;  // uno por canal
};

}  // namespace chatpapol

#endif  // CHATPAPOL_RNNOISE_PROCESSOR_H_

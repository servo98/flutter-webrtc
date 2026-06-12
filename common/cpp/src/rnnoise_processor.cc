// rnnoise_processor.cc — ver rnnoise_processor.h.
#include "rnnoise_processor.h"

#include <cstdlib>
#include <cstring>

#include "voicefx.h"  // C ABI de los efectos (no en el header público)

namespace chatpapol {

namespace {

// Parsea la spec "wet;gain;node|node" donde node = "type,bypass,pid=val&pid=val".
// Usa strtod/strtol (sin excepciones). Tolerante: campos vacíos => defaults.
void ParseSpec(const std::string& spec, float* wet, float* gain,
               std::vector<VfxNodeSpec>* nodes) {
  *wet = 1.0f;
  *gain = 1.0f;
  nodes->clear();

  size_t p1 = spec.find(';');
  if (p1 == std::string::npos) return;
  size_t p2 = spec.find(';', p1 + 1);
  if (p2 == std::string::npos) return;

  *wet = static_cast<float>(std::atof(spec.substr(0, p1).c_str()));
  *gain = static_cast<float>(std::atof(spec.substr(p1 + 1, p2 - p1 - 1).c_str()));

  const std::string nodesBlob = spec.substr(p2 + 1);
  if (nodesBlob.empty()) return;

  size_t start = 0;
  while (start <= nodesBlob.size()) {
    size_t bar = nodesBlob.find('|', start);
    std::string node = nodesBlob.substr(
        start, bar == std::string::npos ? std::string::npos : bar - start);
    if (!node.empty()) {
      VfxNodeSpec ns;
      // type
      size_t c1 = node.find(',');
      if (c1 != std::string::npos) {
        ns.type = static_cast<int>(std::strtol(node.c_str(), nullptr, 10));
        size_t c2 = node.find(',', c1 + 1);
        if (c2 != std::string::npos) {
          ns.bypass = std::strtol(node.substr(c1 + 1, c2 - c1 - 1).c_str(),
                                  nullptr, 10) != 0;
          const std::string pblob = node.substr(c2 + 1);
          size_t ps = 0;
          while (ps <= pblob.size()) {
            size_t amp = pblob.find('&', ps);
            std::string kv = pblob.substr(
                ps, amp == std::string::npos ? std::string::npos : amp - ps);
            size_t eq = kv.find('=');
            if (eq != std::string::npos) {
              int pid = static_cast<int>(std::strtol(kv.c_str(), nullptr, 10));
              float val = static_cast<float>(std::atof(kv.substr(eq + 1).c_str()));
              ns.params.emplace_back(pid, val);
            }
            if (amp == std::string::npos) break;
            ps = amp + 1;
          }
        }
        nodes->push_back(std::move(ns));
      }
    }
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
}

}  // namespace

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
  fx_dirty_ = true;  // geometría cambió: recrear cadenas voicefx
}

void RnnoiseProcessor::Reset(int new_rate) {
  std::lock_guard<std::mutex> lock(mu_);
  rate_ = new_rate;
  for (auto& e : engines_) e->Reset(new_rate);
  fx_dirty_ = true;
}

void RnnoiseProcessor::Process(int num_bands, int num_frames, int buffer_size,
                               float* buffer) {
  std::lock_guard<std::mutex> lock(mu_);
  if (num_frames <= 0 || buffer == nullptr) return;
  if (num_bands < 1) num_bands = 1;
  const int chans = chans_ > 0 ? chans_ : 1;

  // 1) RNNoise sobre la banda 0 (offset c*num_frames), por canal.
  if (rnnoise_on_ && !engines_.empty()) {
    for (int c = 0; c < chans && c < static_cast<int>(engines_.size()); ++c) {
      long off = static_cast<long>(c) * num_frames;
      if (off + num_frames > buffer_size) break;
      engines_[c]->ProcessInPlace(buffer + off, num_frames);
    }
  }

  // 2) voicefx sobre la banda 0 + cero de bandas altas (voz 16k fullband).
  const bool fx_active = fx_on_ && !fx_parsed_.empty();
  if (!fx_active) return;

  const int band_rate = rate_ > 0 ? rate_ / num_bands : 16000;
  if (fx_dirty_ || fx_rate_ != band_rate || fx_frames_ < num_frames ||
      static_cast<int>(fx_chains_.size()) != chans) {
    RebuildFxLocked(band_rate, num_frames);
  }
  if (static_cast<int>(fx_scratch_.size()) < num_frames) {
    fx_scratch_.resize(num_frames);
  }

  constexpr float kInv = 1.0f / 32768.0f;
  for (int c = 0; c < chans && c < static_cast<int>(fx_chains_.size()); ++c) {
    long off = static_cast<long>(c) * num_frames;  // banda 0
    if (off + num_frames > buffer_size) break;
    VfxChain* ch = fx_chains_[c];
    if (!ch) continue;
    float* b0 = buffer + off;
    for (int i = 0; i < num_frames; ++i) fx_scratch_[i] = b0[i] * kInv;
    vfx_process(ch, fx_scratch_.data(), fx_scratch_.data(), num_frames);
    for (int i = 0; i < num_frames; ++i) {
      float v = fx_scratch_[i] * 32768.0f;
      if (v > 32767.0f) v = 32767.0f;
      else if (v < -32768.0f) v = -32768.0f;
      b0[i] = v;
    }
  }

  // Cero de las bandas altas (1..num_bands-1) → la salida queda como la banda 0
  // (16kHz). Para mono (caso del mic) el offset banda b = b*num_frames.
  for (int b = 1; b < num_bands; ++b) {
    for (int c = 0; c < chans; ++c) {
      long off = (static_cast<long>(b) * chans + c) * num_frames;
      if (off + num_frames > buffer_size) break;
      std::memset(buffer + off, 0, sizeof(float) * static_cast<size_t>(num_frames));
    }
  }
}

void RnnoiseProcessor::Release() {
  std::lock_guard<std::mutex> lock(mu_);
  engines_.clear();
  DestroyFx();
  rate_ = 0;
  chans_ = 0;
}

void RnnoiseProcessor::SetRnnoise(bool on) {
  std::lock_guard<std::mutex> lock(mu_);
  rnnoise_on_ = on;
}

void RnnoiseProcessor::SetVoiceFx(bool enabled, const std::string& spec) {
  std::lock_guard<std::mutex> lock(mu_);
  fx_on_ = enabled;
  ParseSpec(spec, &fx_wet_, &fx_gain_, &fx_parsed_);
  fx_dirty_ = true;
}

bool RnnoiseProcessor::active() {
  std::lock_guard<std::mutex> lock(mu_);
  return rnnoise_on_ || (fx_on_ && !fx_parsed_.empty());
}

void RnnoiseProcessor::RebuildFxLocked(int band_rate, int frames) {
  DestroyFx();
  const int chans = chans_ > 0 ? chans_ : 1;
  const int maxFrames = frames < 480 ? 480 : frames;  // holgura
  const int sr = band_rate > 0 ? band_rate : 16000;
  fx_chains_.assign(chans, nullptr);
  for (int c = 0; c < chans; ++c) {
    VfxChain* ch = vfx_create(sr, maxFrames);
    fx_chains_[c] = ch;
    if (!ch) continue;
    vfx_clear(ch);
    vfx_set_master(ch, fx_wet_, fx_gain_);
    for (const auto& ns : fx_parsed_) {
      int idx = vfx_add(ch, ns.type);
      if (idx < 0) continue;
      for (const auto& pv : ns.params) vfx_set_param(ch, idx, pv.first, pv.second);
      if (ns.bypass) vfx_set_bypass(ch, idx, 1);
    }
  }
  fx_rate_ = band_rate;
  fx_frames_ = maxFrames;
  fx_dirty_ = false;
}

void RnnoiseProcessor::DestroyFx() {
  for (VfxChain* ch : fx_chains_) {
    if (ch) vfx_destroy(ch);
  }
  fx_chains_.clear();
}

}  // namespace chatpapol

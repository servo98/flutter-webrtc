// rnnoise_processor.cc — ver rnnoise_processor.h.
#include "rnnoise_processor.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "voicefx.h"  // C ABI de los efectos (no en el header público)

namespace {
// [chatpapol diag] Escribe muestras FloatS16 mono como WAV PCM16. Para el modo
// diagnóstico (medir el audio real fuera de la app, sin adivinar).
void WriteWavPcm16(const std::string& path, const std::vector<float>& s16,
                   int rate) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return;
  const uint32_t n = static_cast<uint32_t>(s16.size());
  const uint32_t dataBytes = n * 2;
  const uint32_t sr = rate > 0 ? static_cast<uint32_t>(rate) : 48000;
  auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
  auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
  f.write("RIFF", 4); w32(36 + dataBytes); f.write("WAVE", 4);
  f.write("fmt ", 4); w32(16); w16(1); w16(1);
  w32(sr); w32(sr * 2); w16(2); w16(16);
  f.write("data", 4); w32(dataBytes);
  for (float v : s16) {
    int iv = static_cast<int>(v < 0 ? v - 0.5f : v + 0.5f);
    if (iv > 32767) iv = 32767; else if (iv < -32768) iv = -32768;
    int16_t s = static_cast<int16_t>(iv);
    f.write(reinterpret_cast<char*>(&s), 2);
  }
}
}  // namespace

// [chatpapol] Monitor local ("escucharme"): reproduce el micro ya procesado en
// los altavoces vía WinMM waveOut (winmm.lib ya está enlazado). Buffers cortos
// (10ms) reciclados por WHDR_DONE; si ninguno está libre, DROPEA el frame en
// vez de bloquear el hilo de audio (preferible un micro-glitch a un hang). Es
// para PROBAR los efectos: usar AURICULARES (si no, eco/realimentación).
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

// Definición del estado del monitor (forward-declarado en el header, PIMPL).
struct VfxMonitorState {
  int rate = 0;       // común a todas las plataformas (lo lee EmitMonitorLocked)
  bool opened = false;
#ifdef _WIN32
  HWAVEOUT hwo = nullptr;
  static constexpr int kN = 24;  // ~240ms de holgura con buffers de 10ms
  WAVEHDR hdr[kN] = {};
  std::vector<int16_t> buf[kN];
  int next = 0;

  bool Open(int sample_rate, int max_frames) {
    Close();
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) !=
        MMSYSERR_NOERROR) {
      hwo = nullptr;
      return false;
    }
    for (int i = 0; i < kN; ++i) {
      buf[i].assign(max_frames > 0 ? max_frames : 480, 0);
      hdr[i] = WAVEHDR{};
    }
    rate = sample_rate;
    next = 0;
    opened = true;
    return true;
  }

  void Push(const float* s, int n) {
    if (!opened || !hwo || n <= 0) return;
    WAVEHDR* h = &hdr[next];
    if (h->dwFlags & WHDR_PREPARED) {
      if (!(h->dwFlags & WHDR_DONE)) return;  // aún sonando → dropea (no bloquea)
      waveOutUnprepareHeader(hwo, h, sizeof(WAVEHDR));
    }
    std::vector<int16_t>& b = buf[next];
    if (static_cast<int>(b.size()) < n) b.resize(n);
    for (int i = 0; i < n; ++i) {
      float v = s[i];
      if (v > 32767.0f) v = 32767.0f;
      else if (v < -32768.0f) v = -32768.0f;
      b[i] = static_cast<int16_t>(v);
    }
    *h = WAVEHDR{};
    h->lpData = reinterpret_cast<LPSTR>(b.data());
    h->dwBufferLength = static_cast<DWORD>(n * sizeof(int16_t));
    if (waveOutPrepareHeader(hwo, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return;
    waveOutWrite(hwo, h, sizeof(WAVEHDR));
    next = (next + 1) % kN;
  }

  void Close() {
    if (hwo) {
      waveOutReset(hwo);
      for (int i = 0; i < kN; ++i) {
        if (hdr[i].dwFlags & WHDR_PREPARED)
          waveOutUnprepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
      }
      waveOutClose(hwo);
      hwo = nullptr;
    }
    opened = false;
  }

  ~VfxMonitorState() { Close(); }
#else
  // No-Windows: monitor no soportado todavía (no-op).
  bool Open(int, int) { return false; }
  void Push(const float*, int) {}
  void Close() {}
#endif
};

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

// [chatpapol 48k] Inicializa el procesador para la ruta de micro kCustom:
// 48 kHz mono, 1 motor RNNoise a 48k (su rate nativo => sin resample). Reutiliza
// Initialize() con la geometría fija de la ruta custom.
void RnnoiseProcessor::InitializeCustom48() {
  Initialize(48000, 1);
  // Resetea el supresor espectral para no arrastrar estado (mínimos/envolventes)
  // de una captura anterior → sin corte al arrancar.
  ns48_.Reset();
}

// [chatpapol 48k] Procesa un bloque de [num_frames] muestras mono a 48 kHz
// (escala FloatS16) IN-PLACE, fuera del APM:
//   1) RNNoise sobre el buffer completo (48k nativo: sin resample).
//   2) cadena voicefx creada a 48k (band_rate = 48000; sin cero de bandas
//      altas porque no hay band-split: el buffer YA es fullband 48k).
//   3) monitor 'escucharme' a 48k (EmitMonitorLocked abre waveOut a rate_).
// Corre en el hilo de audio del capturador; mismo contrato realtime que Process.
void RnnoiseProcessor::ProcessCustom48(float* data, int num_frames) {
  std::lock_guard<std::mutex> lock(mu_);
  if (num_frames <= 0 || data == nullptr) return;
  // Defensa: si nunca se inicializó la geometría custom, hazlo aquí (48k/mono).
  if (rate_ != 48000 || engines_.empty()) {
    rate_ = 48000;
    if (engines_.empty()) {
      auto e = std::make_unique<RnnoiseEngine>();
      e->Reset(48000);
      engines_.push_back(std::move(e));
      chans_ = 1;
      fx_dirty_ = true;
    }
  }

  // [diag] crudo (entrada del path 48k, antes de procesar).
  if (dump_on_) {
    dump_rate_ = 48000;
    DumpAppendLocked(dump_raw_, data, num_frames);
  }

  const bool fx_active = fx_on_ && !fx_parsed_.empty();
  // Passthrough BIT-EXACT cuando no hay NADA activo. Arregla "el 48k falla
  // incluso sin filtros": el viejo AGC+gate se aplicaba SIEMPRE (machacaba el
  // audio aunque todo estuviera off). Ahora, sin procesado, el audio pasa intacto.
  if (input_gain_ == 1.0f && ns_level_ <= 0 && !fx_active && !monitor_on_ &&
      !dump_on_) {
    return;
  }

  // ORDEN de la cadena (teoría): supresión de ruido → efectos/EQ → ganancia →
  // LIMITADOR (último). La ganancia va DESPUÉS de limpiar (no amplifica ruido)
  // y el limitador evita el clipping duro ("truena"). AEC iría antes pero el
  // path kCustom no pasa por el APM (por eso pide auriculares).

  // 1) Supresión de ruido ESPECTRAL propia (Wiener + MCRA, ver spectral_ns.cc).
  //    Limpia el ruido DURANTE el habla SIN agachar la voz. num_frames debe ser
  //    480 (hop); si no, Process() es no-op seguro.
  ns48_.SetLevel(ns_level_);
  if (ns_level_ > 0) ns48_.Process(data, num_frames);

  // 3) voicefx a 48k (sin band-split, sin memset de bandas altas).
  if (fx_active) {
    const int band_rate = 48000;
    if (fx_dirty_ || fx_rate_ != band_rate || fx_frames_ < num_frames ||
        fx_chains_.empty()) {
      RebuildFxLocked(band_rate, num_frames);
    }
    if (static_cast<int>(fx_scratch_.size()) < num_frames) {
      fx_scratch_.resize(num_frames);
    }
    if (!fx_chains_.empty() && fx_chains_[0]) {
      VfxChain* ch = fx_chains_[0];
      constexpr float kInv = 1.0f / 32768.0f;
      for (int i = 0; i < num_frames; ++i) fx_scratch_[i] = data[i] * kInv;
      vfx_process(ch, fx_scratch_.data(), fx_scratch_.data(), num_frames);
      for (int i = 0; i < num_frames; ++i) {
        float v = fx_scratch_[i] * 32768.0f;
        if (v > 32767.0f) v = 32767.0f;
        else if (v < -32768.0f) v = -32768.0f;
        data[i] = v;
      }
    }
  }

  // 3) Ganancia (boost) + LIMITADOR suave como ÚLTIMA etapa. La ganancia va
  //    aquí (tras limpiar/efectos) y el limitador (soft-knee con tanh sobre el
  //    85% de escala) evita el clipping duro que "truena". Corre siempre (cuando
  //    algo está activo) para atrapar también los picos de NS/efectos.
  {
    const float g = input_gain_;
    constexpr float kCeil = 32767.0f;
    constexpr float kKnee = 0.85f * kCeil;
    for (int i = 0; i < num_frames; ++i) {
      float v = data[i] * g;
      const float a = v < 0 ? -v : v;
      if (a > kKnee) {
        const float over = (a - kKnee) / (kCeil - kKnee);
        const float comp = kKnee + (kCeil - kKnee) * std::tanh(over);
        v = v < 0 ? -comp : comp;
      }
      data[i] = v;
    }
  }

  // 4) Monitor: el mismo buffer 48k ya procesado (rate_=48000 => waveOut 48k).
  if (monitor_on_) EmitMonitorLocked(num_frames, data);
  // [diag] procesado (salida del path 48k).
  if (dump_on_) DumpAppendLocked(dump_out_, data, num_frames);
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

  // [diag] crudo: banda 0 del canal 0 (post-APM, antes de RNNoise/efectos).
  if (dump_on_) {
    dump_rate_ = (rate_ > 0 && num_bands > 0) ? rate_ / num_bands : 16000;
    DumpAppendLocked(dump_raw_, buffer, num_frames);
  }

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
  if (!fx_active) {
    // Sin efectos: monitor reproduce la banda 0 del canal 0 (post-RNNoise).
    if (monitor_on_) EmitMonitorLocked(num_frames, buffer);
    if (dump_on_) DumpAppendLocked(dump_out_, buffer, num_frames);
    return;
  }

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

  // Monitor: banda 0 del canal 0 ya con el efecto aplicado.
  if (monitor_on_) EmitMonitorLocked(num_frames, buffer);
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

void RnnoiseProcessor::SetInputGain(float gain) {
  std::lock_guard<std::mutex> lock(mu_);
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 4.0f) gain = 4.0f;  // tope de seguridad
  input_gain_ = gain;
}

void RnnoiseProcessor::SetNsLevel(int level) {
  std::lock_guard<std::mutex> lock(mu_);
  if (level < 0) level = 0;
  if (level > 2) level = 2;
  ns_level_ = level;
}

void RnnoiseProcessor::SetVoiceFx(bool enabled, const std::string& spec) {
  std::lock_guard<std::mutex> lock(mu_);
  fx_on_ = enabled;
  ParseSpec(spec, &fx_wet_, &fx_gain_, &fx_parsed_);
  fx_dirty_ = true;
}

RnnoiseProcessor::RnnoiseProcessor() = default;

RnnoiseProcessor::~RnnoiseProcessor() {
  // monitor_ (unique_ptr) se destruye solo → VfxMonitorState::~ cierra waveOut.
  DestroyFx();
}

void RnnoiseProcessor::SetMonitor(bool on) {
  std::lock_guard<std::mutex> lock(mu_);
  monitor_on_ = on;
  if (on) {
    if (!monitor_) monitor_ = std::make_unique<VfxMonitorState>();
  } else if (monitor_) {
    monitor_->Close();  // libera waveOut; el objeto se reusa si se reactiva
  }
}

// Reproduce [num_frames] muestras (banda 0, canal 0, ya procesadas, escala
// FloatS16) en el monitor. Abre/reabre el reproductor si cambió el rate. Corre
// en el hilo de audio bajo mu_; nunca bloquea (Push dropea si va saturado).
void RnnoiseProcessor::EmitMonitorLocked(int num_frames, const float* band0) {
  if (!monitor_on_ || !monitor_ || num_frames <= 0 || rate_ <= 0) return;
  if (!monitor_->opened || monitor_->rate != rate_) {
    if (!monitor_->Open(rate_, num_frames)) return;
  }
  monitor_->Push(band0, num_frames);
}

bool RnnoiseProcessor::active() {
  std::lock_guard<std::mutex> lock(mu_);
  return rnnoise_on_ || monitor_on_ || dump_on_ ||
         (fx_on_ && !fx_parsed_.empty());
}

void RnnoiseProcessor::StartDump(const std::string& dir) {
  std::lock_guard<std::mutex> lock(mu_);
  dump_dir_ = dir;
  dump_raw_.clear();
  dump_out_.clear();
  dump_raw_.reserve(48000 * 30);
  dump_out_.reserve(48000 * 30);
  dump_rate_ = 0;
  dump_on_ = true;
}

void RnnoiseProcessor::StopDump() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!dump_on_) return;
  dump_on_ = false;
  WriteWavPcm16(dump_dir_ + "/diag-raw.wav", dump_raw_, dump_rate_);
  WriteWavPcm16(dump_dir_ + "/diag-out.wav", dump_out_, dump_rate_);
  dump_raw_.clear();
  dump_out_.clear();
  dump_raw_.shrink_to_fit();
  dump_out_.shrink_to_fit();
}

// requiere mu_ tomado. Cap ~30s para no crecer sin límite.
void RnnoiseProcessor::DumpAppendLocked(std::vector<float>& buf,
                                        const float* data, int n) {
  const int cap = (dump_rate_ > 0 ? dump_rate_ : 48000) * 30;
  if (static_cast<int>(buf.size()) > cap) return;
  buf.insert(buf.end(), data, data + n);
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

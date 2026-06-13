// [chatpapol 48k — Stage 2] Captura de micrófono Windows vía WASAPI shared-mode.
// Pide directamente PCM 48 kHz / mono / int16 con AUTOCONVERTPCM+SRC_DEFAULT_
// QUALITY, así el resampleo lo hace Windows y no añadimos dependencia de SRC.
// Mismo stack COM/AvSetMmThreadCharacteristics que application_loopback_capturer.
#ifdef _WIN32

#include "mic_capturer.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <chrono>
#include <cstring>
#include <iostream>

namespace flutter_webrtc_plugin {

struct MicCapturer::PlatformState {
  IAudioClient* audio_client = nullptr;
  IAudioCaptureClient* capture_client = nullptr;
  WAVEFORMATEX* fmt = nullptr;  // el que aceptamos (48k mono s16)
  HANDLE buffer_ready = nullptr;
};

namespace {

// Busca el endpoint de captura cuyo id/nombre amistoso coincide con device_id.
// device_id vacío -> endpoint por defecto (eCapture/eConsole).
IMMDevice* FindCaptureDevice(IMMDeviceEnumerator* en,
                             const std::string& device_id) {
  if (device_id.empty()) {
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)))
      return dev;
    return nullptr;
  }
  IMMDeviceCollection* coll = nullptr;
  if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll)))
    return nullptr;
  UINT count = 0;
  coll->GetCount(&count);
  IMMDevice* match = nullptr;
  for (UINT i = 0; i < count && !match; ++i) {
    IMMDevice* dev = nullptr;
    if (FAILED(coll->Item(i, &dev)) || !dev) continue;
    // Compara contra el id del endpoint.
    LPWSTR wid = nullptr;
    if (SUCCEEDED(dev->GetId(&wid)) && wid) {
      char buf[512] = {0};
      WideCharToMultiByte(CP_UTF8, 0, wid, -1, buf, sizeof(buf), nullptr,
                          nullptr);
      CoTaskMemFree(wid);
      if (device_id == buf) match = dev;
    }
    // (El match por nombre amistoso requería functiondiscoverykeys_devpkey.h,
    //  que choca con las property keys de mmdeviceapi.h → C2374. Se compara solo
    //  por endpoint-id; el caso por defecto usa device_id vacío de todos modos.)
    if (dev != match) dev->Release();
  }
  coll->Release();
  return match;
}

}  // namespace

MicCapturer::MicCapturer() : plat_(new PlatformState()) {}
MicCapturer::~MicCapturer() {
  Stop();
  delete plat_;
  plat_ = nullptr;
}

bool MicCapturer::Start(scoped_refptr<RTCAudioSource> source,
                        const std::string& device_id) {
  if (running_.load()) return true;
  source_ = source;
  device_id_ = device_id;

  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool co_inited = SUCCEEDED(co);

  IMMDeviceEnumerator* en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&en)))) {
    if (co_inited) CoUninitialize();
    return false;
  }
  IMMDevice* dev = FindCaptureDevice(en, device_id);
  en->Release();
  if (!dev) {
    if (co_inited) CoUninitialize();
    return false;
  }

  HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(&plat_->audio_client));
  dev->Release();
  if (FAILED(hr) || !plat_->audio_client) {
    if (co_inited) CoUninitialize();
    return false;
  }

  // Formato deseado: 48 kHz, mono, 16-bit PCM. Con AUTOCONVERTPCM el motor de
  // audio de Windows hace el SRC desde el formato nativo del micro.
  WAVEFORMATEX want = {};
  want.wFormatTag = WAVE_FORMAT_PCM;
  want.nChannels = static_cast<WORD>(kMicChannels);
  want.nSamplesPerSec = static_cast<DWORD>(kMicSampleRate);
  want.wBitsPerSample = 16;
  want.nBlockAlign =
      static_cast<WORD>(want.nChannels * want.wBitsPerSample / 8);
  want.nAvgBytesPerSec = want.nSamplesPerSec * want.nBlockAlign;
  want.cbSize = 0;

  const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  hr = plat_->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                       /*hnsBufferDuration=*/200000,  // 20 ms
                                       0, &want, nullptr);
  if (FAILED(hr)) {
    std::cerr << "[MicCapturer] Initialize failed: 0x" << std::hex << hr
              << "\n";
    Stop();
    if (co_inited) CoUninitialize();
    return false;
  }
  // Guarda una copia del formato aceptado.
  plat_->fmt = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(want)));
  if (plat_->fmt) std::memcpy(plat_->fmt, &want, sizeof(want));

  plat_->buffer_ready = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (!plat_->buffer_ready ||
      FAILED(plat_->audio_client->SetEventHandle(plat_->buffer_ready)) ||
      FAILED(plat_->audio_client->GetService(
          IID_PPV_ARGS(&plat_->capture_client)))) {
    Stop();
    if (co_inited) CoUninitialize();
    return false;
  }

  // Ring de 500 ms.
  ring_capacity_frames_ = 50 * kMicFramesPer10ms;
  ring_buf_.assign(ring_capacity_frames_, int16_t{0});
  ring_write_frame_ = ring_read_frame_ = ring_frames_avail_ = 0;
  feed_.assign(kMicFramesPer10ms, int16_t{0});

  if (FAILED(plat_->audio_client->Start())) {
    Stop();
    if (co_inited) CoUninitialize();
    return false;
  }

  running_.store(true);
  capture_thread_ = std::thread(&MicCapturer::CaptureThread, this);
  feeder_thread_ = std::thread(&MicCapturer::FeederThread, this);
  // CoUninitialize del hilo que llamó Start; los hilos hacen su propio CoInit.
  if (co_inited) CoUninitialize();
  return true;
}

void MicCapturer::Stop() {
  if (running_.exchange(false)) {
    if (plat_ && plat_->buffer_ready) SetEvent(plat_->buffer_ready);
    if (capture_thread_.joinable()) capture_thread_.join();
    if (feeder_thread_.joinable()) feeder_thread_.join();
  } else {
    // Aun así, por si Start falló a medias, intenta unir hilos no arrancados.
    if (capture_thread_.joinable()) capture_thread_.join();
    if (feeder_thread_.joinable()) feeder_thread_.join();
  }
  if (plat_) {
    if (plat_->audio_client) {
      plat_->audio_client->Stop();
      plat_->audio_client->Release();
      plat_->audio_client = nullptr;
    }
    if (plat_->capture_client) {
      plat_->capture_client->Release();
      plat_->capture_client = nullptr;
    }
    if (plat_->fmt) {
      CoTaskMemFree(plat_->fmt);
      plat_->fmt = nullptr;
    }
    if (plat_->buffer_ready) {
      CloseHandle(plat_->buffer_ready);
      plat_->buffer_ready = nullptr;
    }
  }
  source_ = nullptr;
}

void MicCapturer::CaptureThread() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD task_index = 0;
  HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);

  while (running_.load()) {
    DWORD wr = WaitForSingleObject(plat_->buffer_ready, 200);
    if (!running_.load()) break;
    if (wr == WAIT_TIMEOUT) continue;
    if (wr != WAIT_OBJECT_0) break;

    UINT32 packet = 0;
    while (SUCCEEDED(plat_->capture_client->GetNextPacketSize(&packet)) &&
           packet > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(plat_->capture_client->GetBuffer(&data, &frames, &flags,
                                                  nullptr, nullptr)))
        break;
      if (frames > 0) {
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          static thread_local std::vector<int16_t> zeros;
          zeros.assign(frames, int16_t{0});
          RingWrite(zeros.data(), frames);
        } else {
          // Formato ya es int16 mono 48k (AUTOCONVERTPCM).
          RingWrite(reinterpret_cast<const int16_t*>(data), frames);
        }
      }
      plat_->capture_client->ReleaseBuffer(frames);
    }
  }

  if (task) AvRevertMmThreadCharacteristics(task);
  CoUninitialize();
}

}  // namespace flutter_webrtc_plugin

#endif  // _WIN32

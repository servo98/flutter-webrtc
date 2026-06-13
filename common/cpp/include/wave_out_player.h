// wave_out_player.h — [chatpapol] reproductor PCM mono int16 vía WinMM waveOut.
//
// Extraído de la struct VfxMonitorState que vivía en rnnoise_processor.cc (el
// monitor local "escucharme"). Reutilizado por per_user_eq.cc como salida propia
// del EQ por-usuario. MISMA semántica que el original:
//   - mono int16, WAVE_MAPPER (dispositivo por defecto del SO).
//   - buffers cortos reciclados por WHDR_DONE; si ninguno libre → DROPEA (no
//     bloquea el hilo de audio: preferible un micro-glitch a un hang).
//   - Push() acepta float en escala int16 (±32768) y lo satura a int16.
//   - No-op en plataformas no-Windows (Linux/macOS): Open() devuelve false.
//
// INCIERTO (heredado del monitor): WAVE_MAPPER ignora la selección de
// dispositivo de salida de la app (sale por el default del SO). Es una
// limitación conocida; migrar a WASAPI render es trabajo futuro (v2).
//
// Header-only con guardas _WIN32 para no arrastrar windows.h a quien no lo use:
// el cuerpo Windows solo se compila en TUs que ya incluyen <windows.h> ANTES
// de este header (per_user_eq.cc lo hace). En no-Windows el cuerpo es no-op puro.
#ifndef CHATPAPOL_WAVE_OUT_PLAYER_H_
#define CHATPAPOL_WAVE_OUT_PLAYER_H_

#include <cstdint>
#include <vector>

namespace chatpapol {

#ifdef _WIN32
// Requiere que el TU que incluye este header haya hecho:
//   #include <windows.h>
//   #include <mmsystem.h>
// (igual que rnnoise_processor.cc). No los incluimos aquí para no forzar
// windows.h en headers públicos que arrastren este por transitividad.
struct WaveOutPlayer {
  int rate = 0;
  bool opened = false;
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

  ~WaveOutPlayer() { Close(); }
};
#else
// No-Windows: salida por waveOut no soportada (no-op). El EQ por-usuario no
// produce audio en Linux/macOS por esta ruta — ver gating en Dart.
struct WaveOutPlayer {
  int rate = 0;
  bool opened = false;
  bool Open(int, int) { return false; }
  void Push(const float*, int) {}
  void Close() {}
};
#endif

}  // namespace chatpapol

#endif  // CHATPAPOL_WAVE_OUT_PLAYER_H_

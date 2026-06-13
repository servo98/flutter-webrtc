/* ============================================================================
 * voicefx.h — ChatPapol real-time voice-effects engine (C ABI).
 *
 * Mono float32 PCM block processor. Designed to sit in the capture path:
 *
 *     mic -> RNNoise (flutter_webrtc fork, other module) -> VoiceFX -> encode
 *
 * FRAME CONTRACT (authoritative copy lives in ../CONTRACT.md):
 *   - Sample rate: fixed at vfx_create() time. Canonical rate is 48000 Hz.
 *   - Channels: mono only.
 *   - Format: float32 PCM, nominal range [-1.0, +1.0].
 *   - Block size: numFrames <= maxFrames (canonical block: 480 = 10 ms).
 *   - In-place allowed: vfx_process(in == out) MUST work.
 *   - vfx_process() is REALTIME-SAFE: no malloc/free, no locks, no I/O.
 *     All buffers are pre-allocated in vfx_create()/vfx_add().
 *
 * THREADING CONTRACT:
 *   - vfx_process() runs on the audio (real-time) thread.
 *   - All other functions run on a single control thread (Dart main isolate).
 *   - vfx_set_param / vfx_set_bypass / vfx_set_master are safe to call while
 *     vfx_process() runs concurrently (implementation: atomics + smoothing).
 *   - vfx_add / vfx_clear / vfx_destroy must ALSO be safe w.r.t. a concurrent
 *     vfx_process() (implementation: lock-free chain pointer swap); they are
 *     NOT re-entrant among themselves (single control thread assumed).
 *
 * Versioning: bump VOICEFX_ABI_VERSION on any breaking change to this file.
 * ==========================================================================*/

#ifndef CHATPAPOL_VOICEFX_H_
#define CHATPAPOL_VOICEFX_H_

#define VOICEFX_ABI_VERSION 3

/* Maximum number of effect nodes in one chain. vfx_add() fails beyond this. */
#define VFX_MAX_NODES 16

#if defined(_WIN32)
  #if defined(VOICEFX_BUILD)
    #define VFX_EXPORT __declspec(dllexport)
  #else
    #define VFX_EXPORT __declspec(dllimport)
  #endif
#else
  #define VFX_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque effect chain handle. One instance per audio stream. */
typedef struct VfxChain VfxChain;

/* ----------------------------------------------------------------------------
 * Effect types. Characters/rooms are built by CHAINING these primitives.
 * Values are stable ABI — never renumber, only append.
 * --------------------------------------------------------------------------*/
typedef enum VfxEffectType {
  VFX_REVERB     = 0, /* Freeverb-style room reverb (cave/church/room).      */
  VFX_DELAY      = 1, /* Feedback delay / echo.                              */
  VFX_BIQUAD     = 2, /* 2nd-order filter: LP/HP/BP/notch (radio/phone).     */
  VFX_RINGMOD    = 3, /* Ring modulator (robot).                             */
  VFX_DISTORTION = 4, /* tanh waveshaper drive (megaphone/demon grit).       */
  VFX_PITCH      = 5, /* Pitch shift + independent formant shift.            */
  VFX_NOISE      = 6, /* Additive noise bed (radio hiss / ambience).         */
  VFX_TREMOLO    = 7, /* Amplitude LFO wobble (old-voice).                   */
  VFX_CHORUS     = 8, /* Modulated short delay (sci-fi shimmer).             */
  VFX_COMP       = 9, /* Compressor + noise gate (mastering dynamics).       */
  VFX_BITCRUSH   = 10, /* Bit quantization + sample&hold (8-bit/digital).    */
  VFX_VIBRATO    = 11, /* Modulated short delay, no dry (pitch wobble).      */
  VFX_FLANGER    = 12, /* Short comb + LFO + feedback (jet/resonant robot).  */

  VFX_EFFECT_TYPE_COUNT = 13
} VfxEffectType;

/* Filter modes for VFX_P_BIQUAD_TYPE (passed as float, compared as int).
 * 4/5/6 are EQ modes that additionally use VFX_P_BIQUAD_GAIN_DB. */
typedef enum VfxBiquadType {
  VFX_BIQUAD_LOWPASS   = 0,
  VFX_BIQUAD_HIGHPASS  = 1,
  VFX_BIQUAD_BANDPASS  = 2,
  VFX_BIQUAD_NOTCH     = 3,
  VFX_BIQUAD_PEAKING   = 4, /* parametric bell, boost/cut at FREQ (uses GAIN_DB). */
  VFX_BIQUAD_LOWSHELF  = 5, /* low shelf below FREQ (uses GAIN_DB).               */
  VFX_BIQUAD_HIGHSHELF = 6  /* high shelf above FREQ (uses GAIN_DB).              */
} VfxBiquadType;

/* ----------------------------------------------------------------------------
 * Parameter ids. GLOBAL stable namespace: each effect gets a block of 100 so
 * new params can be appended without renumbering. A paramId is only valid for
 * the node whose effect type owns it; vfx_set_param() ignores mismatches.
 *
 * Every param documents: range, default, unit.
 * --------------------------------------------------------------------------*/
typedef enum VfxParamId {
  /* -- VFX_REVERB (100..199) ---------------------------------------------- */
  VFX_P_REVERB_ROOMSIZE  = 100, /* 0.0..1.0   default 0.50  normalized room size (1.0 ~ cathedral/cave). */
  VFX_P_REVERB_DAMP      = 101, /* 0.0..1.0   default 0.50  high-frequency damping in the tail (1.0 = darkest). */
  VFX_P_REVERB_WET       = 102, /* 0.0..1.0   default 0.33  reverb wet level mixed against dry. */

  /* -- VFX_DELAY (200..299) ------------------------------------------------ */
  VFX_P_DELAY_TIME_MS    = 200, /* 1.0..2000.0 default 350.0 ms, delay time. */
  VFX_P_DELAY_FEEDBACK   = 201, /* 0.0..0.95  default 0.35  repeat feedback gain (clamped at 0.95 for stability). */
  VFX_P_DELAY_MIX        = 202, /* 0.0..1.0   default 0.50  wet mix (0 = dry only, 1 = echo only). */

  /* -- VFX_BIQUAD (300..399) ----------------------------------------------- */
  VFX_P_BIQUAD_TYPE      = 300, /* 0..6       default 0     VfxBiquadType (LP/HP/BP/NOTCH/PEAK/LOWSHELF/HIGHSHELF). */
  VFX_P_BIQUAD_FREQ      = 301, /* 20.0..20000.0 default 1000.0 Hz, cutoff/center frequency. */
  VFX_P_BIQUAD_Q         = 302, /* 0.1..10.0  default 0.707 resonance / bandwidth quality. */
  VFX_P_BIQUAD_GAIN_DB   = 303, /* -15.0..+15.0 default 0.0 dB, boost/cut for PEAK/LOWSHELF/HIGHSHELF (ignored by LP/HP/BP/NOTCH). */

  /* -- VFX_RINGMOD (400..499) ---------------------------------------------- */
  VFX_P_RINGMOD_FREQ     = 400, /* 1.0..2000.0 default 30.0 Hz, sine carrier frequency. */
  VFX_P_RINGMOD_MIX      = 401, /* 0.0..1.0   default 1.0   wet mix (1 = full ring-mod). */

  /* -- VFX_DISTORTION (500..599) ------------------------------------------- */
  VFX_P_DIST_DRIVE       = 500, /* 1.0..50.0  default 8.0   linear pre-gain into tanh() shaper. */
  VFX_P_DIST_MIX         = 501, /* 0.0..1.0   default 1.0   wet mix against the clean signal. */

  /* -- VFX_PITCH (600..699) ------------------------------------------------ */
  VFX_P_PITCH_SEMITONES  = 600, /* -12.0..+12.0 default 0.0  semitones of pitch shift. */
  VFX_P_PITCH_FORMANT    = 601, /* 0.5..2.0   default 1.0   formant (spectral envelope) ratio; <1 bigger/deeper, >1 smaller/brighter. */

  /* -- VFX_NOISE (700..799) ------------------------------------------------ */
  VFX_P_NOISE_LEVEL      = 700, /* 0.0..1.0   default 0.05  linear noise amplitude added to the signal. */
  VFX_P_NOISE_COLOR      = 701, /* 0.0..1.0   default 0.50  spectral tilt: 0 = white, 1 = dark (pink/brown). */

  /* -- VFX_TREMOLO (800..899) ---------------------------------------------- */
  VFX_P_TREMOLO_RATE     = 800, /* 0.1..20.0  default 5.0   Hz, LFO rate. */
  VFX_P_TREMOLO_DEPTH    = 801, /* 0.0..1.0   default 0.5   modulation depth (1 = full gating). */

  /* -- VFX_CHORUS (900..999) ----------------------------------------------- */
  VFX_P_CHORUS_RATE      = 900, /* 0.05..5.0  default 0.8   Hz, modulation LFO rate. */
  VFX_P_CHORUS_DEPTH     = 901, /* 0.0..1.0   default 0.4   normalized depth (maps to 0..~8 ms of delay sweep). */
  VFX_P_CHORUS_MIX       = 902, /* 0.0..1.0   default 0.5   wet mix. */

  /* -- VFX_COMP (1000..1099) ----------------------------------------------- */
  VFX_P_COMP_GATE_THRESH  = 1000, /* -80.0..0.0  default -45.0 dBFS, gate opens above this (peak). */
  VFX_P_COMP_GATE_RELEASE = 1001, /* 10.0..300.0 default 120.0 ms, gate close (release) time. */
  VFX_P_COMP_RATIO        = 1002, /* 1.0..20.0   default 3.0   compression ratio (N:1). */
  VFX_P_COMP_THRESH       = 1003, /* -40.0..0.0  default -18.0 dBFS, compressor knee threshold (RMS). */
  VFX_P_COMP_ATTACK       = 1004, /* 1.0..50.0   default 10.0  ms, compressor attack time. */
  VFX_P_COMP_RELEASE      = 1005, /* 20.0..300.0 default 100.0 ms, compressor release time. */
  VFX_P_COMP_MAKEUP       = 1006, /* 0.0..24.0   default 0.0   dB, output makeup gain after compression. */

  /* -- VFX_BITCRUSH (1100..1199) ------------------------------------------- */
  VFX_P_CRUSH_BITS       = 1100, /* 1.0..16.0  default 8.0   bit depth (quantization levels = 2^bits). */
  VFX_P_CRUSH_DOWNSAMPLE = 1101, /* 1.0..32.0  default 1.0   integer sample&hold factor (1 = no decimation). */
  VFX_P_CRUSH_MIX        = 1102, /* 0.0..1.0   default 1.0   wet mix against the clean signal. */

  /* -- VFX_VIBRATO (1200..1299) -------------------------------------------- */
  VFX_P_VIBRATO_RATE        = 1200, /* 0.1..12.0 default 5.5  Hz, pitch-wobble LFO rate. */
  VFX_P_VIBRATO_DEPTH_CENTS = 1201, /* 0.0..50.0 default 20.0 cents of pitch deviation (delay-modulation depth). */

  /* -- VFX_FLANGER (1300..1399) -------------------------------------------- */
  VFX_P_FLANGER_RATE     = 1300, /* 0.05..2.0  default 0.4   Hz, sweep LFO rate. */
  VFX_P_FLANGER_DEPTH    = 1301, /* 0.0..1.0   default 0.6   normalized sweep depth. */
  VFX_P_FLANGER_FEEDBACK = 1302, /* 0.0..0.9   default 0.5   comb feedback gain (resonance). */
  VFX_P_FLANGER_MIX      = 1303  /* 0.0..1.0   default 0.5   wet mix. */
} VfxParamId;

/* ----------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------*/

/* Reports the ABI version compiled into the BINARY. The Dart side must check
 * vfx_abi_version() == VOICEFX_ABI_VERSION at load time and refuse mismatches. */
VFX_EXPORT int vfx_abi_version(void);

/* Creates an empty chain.
 *   sampleRate: Hz, e.g. 48000 (canonical). Must be > 0.
 *   maxFrames:  largest numFrames that will ever be passed to vfx_process()
 *               (canonical: 480). Used to pre-allocate all scratch buffers.
 * Returns NULL on invalid args or allocation failure. */
VFX_EXPORT VfxChain* vfx_create(int sampleRate, int maxFrames);

/* Destroys the chain and frees all resources. NULL is a safe no-op.
 * The caller guarantees no vfx_process() call is in flight or will follow. */
VFX_EXPORT void vfx_destroy(VfxChain* chain);

/* ----------------------------------------------------------------------------
 * Chain editing (control thread)
 * --------------------------------------------------------------------------*/

/* Removes all effect nodes. Master wet/gain are NOT reset. */
VFX_EXPORT void vfx_clear(VfxChain* chain);

/* Appends an effect node (VfxEffectType) at the end of the chain with its
 * default params and bypass = 0.
 * Returns the new node index (0-based, processing order) or -1 on error
 * (unknown type, chain full at VFX_MAX_NODES, or allocation failure).
 * NOTE: there is no remove/reorder primitive — the Dart engine rebuilds the
 * chain (vfx_clear + vfx_add*) to implement those operations. */
VFX_EXPORT int vfx_add(VfxChain* chain, int effectType);

/* Sets one parameter on one node. Values outside the documented range are
 * CLAMPED. Invalid nodeIndex or a paramId not owned by that node's effect
 * type is silently ignored. Safe while processing runs. */
VFX_EXPORT void vfx_set_param(VfxChain* chain, int nodeIndex, int paramId,
                              float value);

/* bypass != 0 makes the node pass audio through untouched (state is kept
 * warm so un-bypassing does not click). Safe while processing runs. */
VFX_EXPORT void vfx_set_bypass(VfxChain* chain, int nodeIndex, int bypass);

/* Master output stage, applied AFTER the whole chain:
 *   out = dry * (1 - wetMix) + processed * wetMix, then * outGain.
 *   wetMix:  0.0..1.0, default 1.0 (full effect).
 *   outGain: 0.0..4.0 linear, default 1.0.
 * Out-of-range values are clamped. Safe while processing runs. */
VFX_EXPORT void vfx_set_master(VfxChain* chain, float wetMix, float outGain);

/* ----------------------------------------------------------------------------
 * Processing (audio thread)
 * --------------------------------------------------------------------------*/

/* Processes numFrames mono float32 samples from `in` into `out`.
 *   - in == out is allowed (in-place).
 *   - numFrames must be in 1..maxFrames (as passed to vfx_create()).
 *   - REALTIME-SAFE: no allocation, no locks, no syscalls.
 *   - An empty or fully-bypassed chain copies in -> out (plus master stage).
 *   - Output is soft-clamped to [-1, +1] at the very end of the master stage. */
VFX_EXPORT void vfx_process(VfxChain* chain, const float* in, float* out,
                            int numFrames);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CHATPAPOL_VOICEFX_H_ */

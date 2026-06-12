/* ============================================================================
 * voicefx.cpp — C ABI implementation (include/voicefx.h, ABI v1).
 *
 * Ownership model:
 *   - VfxChain owns the effect Nodes (control thread side).
 *   - The audio thread never walks the control-side array; it reads an
 *     immutable Snapshot (node pointer list) published through an atomic.
 *
 * Lock-free chain swap (vfx_add / vfx_clear vs concurrent vfx_process):
 *   - control: build new Snapshot -> atomically exchange `active` -> spin
 *     until the audio thread is provably not inside the OLD snapshot
 *     (`inUse != old`) -> free the old snapshot (and nodes, on clear).
 *   - audio: load `active`, publish it in `inUse`, re-check `active` until
 *     stable, process, then publish inUse = NULL. The audio thread never
 *     blocks, allocates or takes locks; only the control thread spins.
 *
 * Param changes (vfx_set_param / set_bypass / set_master) write atomics on
 * live nodes; the audio thread smooths them per block (see effects.hpp), so
 * they are click-free and need no swap.
 * ==========================================================================*/

#include "../include/voicefx.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include "effects.hpp"

#if defined(__SSE2__) || defined(_M_X64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#define VFX_HAS_SSE 1
#endif

namespace {

/* One effect node. `bypass` is the only field the control thread mutates
 * after publication, and it is atomic. */
struct Node {
  vfx::Effect* fx = nullptr;
  int type = -1;
  std::atomic<int> bypass{0};

  ~Node() { delete fx; }
};

/* Immutable list of node pointers, published to the audio thread. */
struct Snapshot {
  Node* nodes[VFX_MAX_NODES] = {};
  int count = 0;
};

/* Soft clamp to [-1, +1]: identity below the knee (0.95), then a smooth tanh
 * saturation that tops out exactly at 1.0. C1-continuous, RT-safe. */
inline float softClamp(float x) {
  constexpr float kKnee = 0.95f;
  const float ax = std::fabs(x);
  if (ax <= kKnee) return x;
  const float y =
      kKnee + (1.0f - kKnee) * std::tanh((ax - kKnee) / (1.0f - kKnee));
  return x < 0.0f ? -y : y;
}

/* Scoped FTZ/DAZ on x86 so feedback tails never hit denormal slow paths.
 * (effects also call vfx::undenorm() in their feedback paths as a portable
 * fallback for non-SSE targets.) */
struct FtzGuard {
#ifdef VFX_HAS_SSE
  unsigned int csr_;
  FtzGuard() : csr_(_mm_getcsr()) {
    _mm_setcsr(csr_ | 0x8040u); /* FTZ (0x8000) | DAZ (0x0040) */
  }
  ~FtzGuard() { _mm_setcsr(csr_); }
#else
  FtzGuard() {}
#endif
};

} /* namespace */

/* The opaque handle from the header. */
struct VfxChain {
  int sampleRate = 0;
  int maxFrames = 0;

  /* Pre-allocated scratch: `wet` is the processing bus, `warm` is a throwaway
   * copy used to keep bypassed nodes' state running (no click on un-bypass). */
  std::vector<float> wet;
  std::vector<float> warm;

  /* Published chain (audio thread reads), see file header for the protocol. */
  std::atomic<Snapshot*> active{nullptr};
  std::atomic<Snapshot*> inUse{nullptr};

  /* Control-side mirror of the same Node pointers (owned here). */
  Node* nodes[VFX_MAX_NODES] = {};
  int count = 0;

  /* Master stage (atomic targets + per-block smoothing on the audio side). */
  vfx::Param wetMix;
  vfx::Param outGain;
};

namespace {

/* Control thread: publish `next`, wait out the audio thread, free the old
 * snapshot struct (NOT the nodes — node lifetime is handled by the caller). */
void swapActive(VfxChain* c, Snapshot* next) {
  Snapshot* old = c->active.exchange(next, std::memory_order_acq_rel);
  /* The audio callback finishes a 10 ms block in well under a millisecond;
   * a plain busy-wait on the control thread is fine and avoids <thread>. */
  while (c->inUse.load(std::memory_order_seq_cst) == old) {
    /* spin */
  }
  delete old;
}

} /* namespace */

extern "C" {

int vfx_abi_version(void) { return VOICEFX_ABI_VERSION; }

VfxChain* vfx_create(int sampleRate, int maxFrames) {
  if (sampleRate <= 0 || maxFrames <= 0) return nullptr;
  VfxChain* c = nullptr;
  try {
    c = new VfxChain();
    c->sampleRate = sampleRate;
    c->maxFrames = maxFrames;
    c->wet.assign(static_cast<size_t>(maxFrames), 0.0f);
    c->warm.assign(static_cast<size_t>(maxFrames), 0.0f);
    c->wetMix.init(1.0f);
    c->outGain.init(1.0f);
    c->active.store(new Snapshot(), std::memory_order_release);
  } catch (...) {
    delete c;
    return nullptr;
  }
  return c;
}

void vfx_destroy(VfxChain* chain) {
  if (!chain) return;
  /* Caller guarantees no vfx_process is in flight or will follow. */
  delete chain->active.load(std::memory_order_acquire);
  for (int i = 0; i < chain->count; ++i) delete chain->nodes[i];
  delete chain;
}

void vfx_clear(VfxChain* chain) {
  if (!chain) return;
  Snapshot* next;
  try {
    next = new Snapshot();
  } catch (...) {
    return;
  }
  swapActive(chain, next);
  /* Audio thread can no longer reach the nodes; safe to free them. */
  for (int i = 0; i < chain->count; ++i) {
    delete chain->nodes[i];
    chain->nodes[i] = nullptr;
  }
  chain->count = 0;
  /* Master wet/gain intentionally NOT reset (contract). */
}

int vfx_add(VfxChain* chain, int effectType) {
  if (!chain) return -1;
  if (chain->count >= VFX_MAX_NODES) return -1;

  Node* nd = nullptr;
  Snapshot* next = nullptr;
  try {
    vfx::Effect* fx = vfx::createEffect(effectType);
    if (!fx) return -1; /* unknown type */
    nd = new Node();
    nd->fx = fx;
    nd->type = effectType;
    nd->fx->prepare(chain->sampleRate, chain->maxFrames);
    next = new Snapshot();
  } catch (...) {
    delete nd;
    delete next;
    return -1;
  }

  for (int i = 0; i < chain->count; ++i) next->nodes[i] = chain->nodes[i];
  next->nodes[chain->count] = nd;
  next->count = chain->count + 1;

  chain->nodes[chain->count] = nd;
  chain->count += 1;

  swapActive(chain, next);
  return chain->count - 1;
}

void vfx_set_param(VfxChain* chain, int nodeIndex, int paramId, float value) {
  if (!chain || nodeIndex < 0 || nodeIndex >= chain->count) return;
  Node* nd = chain->nodes[nodeIndex];
  /* paramId must belong to this node's effect type; otherwise ignore. */
  if (vfx::paramOwnerType(paramId) != nd->type) return;
  nd->fx->setParam(paramId, value); /* clamps to the documented range */
}

void vfx_set_bypass(VfxChain* chain, int nodeIndex, int bypass) {
  if (!chain || nodeIndex < 0 || nodeIndex >= chain->count) return;
  chain->nodes[nodeIndex]->bypass.store(bypass ? 1 : 0,
                                        std::memory_order_relaxed);
}

void vfx_set_master(VfxChain* chain, float wetMix, float outGain) {
  if (!chain) return;
  chain->wetMix.set(vfx::clampf(wetMix, 0.0f, 1.0f));
  chain->outGain.set(vfx::clampf(outGain, 0.0f, 4.0f));
}

void vfx_process(VfxChain* chain, const float* in, float* out, int numFrames) {
  if (!chain || !in || !out || numFrames <= 0) return;
  if (numFrames > chain->maxFrames) numFrames = chain->maxFrames; /* defensive */
  const size_t n = static_cast<size_t>(numFrames);

  FtzGuard ftz; /* flush denormals for the whole callback */
  (void)ftz;

  /* Acquire a stable snapshot and advertise it (see file-header protocol). */
  Snapshot* snap = chain->active.load(std::memory_order_acquire);
  chain->inUse.store(snap, std::memory_order_seq_cst);
  while (snap != chain->active.load(std::memory_order_seq_cst)) {
    snap = chain->active.load(std::memory_order_acquire);
    chain->inUse.store(snap, std::memory_order_seq_cst);
  }

  /* Wet bus = sanitized input (NaN/Inf from upstream become silence). */
  float* wet = chain->wet.data();
  for (size_t i = 0; i < n; ++i) {
    const float v = in[i];
    wet[i] = std::isfinite(v) ? v : 0.0f;
  }

  /* Run the chain in series. Bypassed nodes still process a throwaway copy
   * so their internal state (delay lines, reverb tails, PV frames) stays
   * warm and un-bypassing does not click. */
  for (int k = 0; k < snap->count; ++k) {
    Node* nd = snap->nodes[k];
    if (nd->bypass.load(std::memory_order_relaxed)) {
      std::memcpy(chain->warm.data(), wet, sizeof(float) * n);
      nd->fx->process(chain->warm.data(), numFrames);
    } else {
      nd->fx->process(wet, numFrames);
    }
  }

  /* Master stage: dry/wet crossfade, output gain, soft clamp. `in` is still
   * intact here even when in == out, because we only write `out` now. */
  const float wm = chain->wetMix.next(vfx::kBlockSmooth);
  const float og = chain->outGain.next(vfx::kBlockSmooth);
  for (size_t i = 0; i < n; ++i) {
    float dry = in[i];
    if (!std::isfinite(dry)) dry = 0.0f;
    float y = (dry * (1.0f - wm) + wet[i] * wm) * og;
    if (!std::isfinite(y)) y = 0.0f;
    out[i] = softClamp(y);
  }

  chain->inUse.store(nullptr, std::memory_order_release);
}

} /* extern "C" */

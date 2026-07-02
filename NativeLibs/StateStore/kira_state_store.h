// Ambient key -> pointer slots for UI state that must survive per-frame widget rebuilds.
//
// The Kira UI `@State` property wrapper stores each wrapped property's backing struct behind a
// stable string key ("Owner.property"). Kira has no mutable globals by design; this tiny store is
// the one process-global anchor the wrapper implementation hangs its `nativeState` allocations
// on. Values are opaque pointers owned by the Kira runtime — the store never allocates, frees, or
// interprets them.
//
// Single-threaded by contract (the UI frame loop); no locking.

#ifndef KIRA_STATE_STORE_H
#define KIRA_STATE_STORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 when a value was stored under `key`, else 0.
int32_t kira_state_slot_has(const char* key);

// The pointer stored under `key`, or NULL when absent.
void* kira_state_slot_get(const char* key);

// Store `value` under `key` (replaces an existing entry). The key is copied; the value is not.
void kira_state_slot_put(const char* key, void* value);

// Remove every entry (test isolation / editor session reset). Values are NOT freed — they are
// Kira-runtime-owned.
void kira_state_slot_reset(void);

// Number of live entries (introspection / tests).
int32_t kira_state_slot_count(void);

#ifdef __cplusplus
}
#endif

#endif

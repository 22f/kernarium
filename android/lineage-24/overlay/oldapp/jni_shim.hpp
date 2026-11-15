// SPDX-License-Identifier: Apache-2.0
//
// The old-application layer's JNI interposer.
//
// install_jni_wrappers() replaces slots of the table every JNIEnv of the process
// shares (ART's, or CheckJNI's). Calls from application code get Dalvik-era
// behaviour: a byte[] result is a read-only shadow block with Dalvik's object
// layout, and local references an application leaks are released when ART's table
// is about to overflow. Calls from the platform's own libraries pass through to
// the original functions untouched.
//
// Nothing here includes an Android header. The platform is injected (Platform), so
// the host tests drive the wrappers against a fake JNI table.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jni_abi.hpp"

namespace oldapp {

struct Platform {
    // Whether the code at `return_address` belongs to the application. The first call from a
    // library the platform has not seen yet is also where the glue notices it.
    bool (*is_app_caller)(const void* return_address);
    // The layer's per-thread state: a zeroed block, kept in a thread-specific slot.
    void* (*thread_state)(size_t size);
    void (*log)(const char* message, long a, long b);
    // The table lives in read-only memory; these open and close the pages around the writes.
    bool (*make_writable)(void* address, size_t length);
    bool (*make_read_only)(void* address, size_t length);
};

struct Counters {
    uint32_t shadows_created;
    uint32_t shadows_live;
    uint32_t shadows_unwrapped;  // temporary Java arrays made for a shadow passed to the runtime
    uint32_t probes;
    uint32_t relief_rounds;
    uint32_t references_released;
};

// `table` is the shared function table (kSlotCount entries), `shadow_region` the memory of the byte[]
// blocks (null: no shadows), `relief` whether to release leaked local references when the table is
// nearly full. `env` is the installing thread's environment, used to probe the runtime once. Returns
// false, changing nothing, when the table cannot be written.
bool install_jni_wrappers(void** table, const Platform& platform, void* shadow_region, size_t shadow_size,
                          bool relief, jni::Env* env);

// The invoke interface: only the "a library may have loaded" notification (is_app_caller is asked
// for the caller) on GetEnv and the attach calls.
bool install_vm_wrappers(void** vm_table, const Platform& platform);

bool is_shadow_block(const void* object);
Counters counters();

// A thread is ending: its shadow blocks go back to the arena. `state` is what Platform::thread_state
// returned for it (the caller frees the memory). Calls no JNI function.
void release_thread_state(void* state);

// For the tests: forget the installation (the caller restores the table).
void reset_for_test();

}  // namespace oldapp

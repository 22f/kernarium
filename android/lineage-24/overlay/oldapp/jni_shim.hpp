// SPDX-License-Identifier: Apache-2.0
//
// The old-application layer's JNI interposer.
//
// install_jni_wrappers() replaces slots of the table every JNIEnv of the process
// shares (ART's, or CheckJNI's). A byte[] that application code gets back from Java
// becomes a shadow: a block with Dalvik's object layout (length at +8, bytes at +16)
// that stands for the real array, which the block keeps a global reference to. Every
// JNI function that takes an object turns a shadow back into that real array for any
// caller, so identity (IsSameObject, NewGlobalRef, ...) and writes (SetByteArrayRegion,
// Release...Elements) keep their meaning. Calls from the platform's own libraries get
// no shadows.
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
    // The table lives in read-only memory. make_writable opens the pages of [address, address +
    // length) and restores every page it opened when one cannot be opened (it then returns
    // false); make_read_only restores them and returns whether every restore succeeded.
    bool (*make_writable)(void* address, size_t length);
    bool (*make_read_only)(void* address, size_t length);
    // The kinds of the parameters of a method the layer did not see being looked up
    // (Z B C S I J F D, or L for every reference), by reflection on the original table.
    // `hint` is a class the method can be asked about. Returns false when it cannot say.
    bool (*describe_method)(jni::Env* env, void* const* original_table, void* method_id, bool is_static,
                            jni::jobject hint, char* kinds, unsigned capacity, unsigned* count);
    // A monotonic clock in milliseconds (it may wrap): the rate limit of the diagnosis lines. Null: no limit.
    uint32_t (*now_ms)();
    // The memory of a block's baseline (the bytes as of its last synchronization): outside the low arena, any
    // address. Without them nothing is intercepted.
    void* (*alloc_baseline)(size_t bytes);
    void (*free_baseline)(void* baseline);
};

struct Counters {
    uint32_t shadows_created;
    uint32_t shadows_live;
    uint32_t shadows_declined;   // a result that stayed the real reference: the thread's or the region's limit
    uint32_t conversions;        // shadows turned back into the real array for the runtime
    uint32_t shadows_released;   // blocks of ended threads that were freed later
    uint32_t peak_thread_blocks; // the most live blocks any one thread has held
};

// `table` is the shared function table (kSlotCount entries), `shadow_region` the memory of the byte[]
// blocks (null: nothing to intercept). Returns false, changing nothing, when the table cannot be
// written.
bool install_jni_wrappers(void** table, const Platform& platform, void* shadow_region, size_t shadow_size);

// The invoke interface: only the "a library may have loaded" notification (is_app_caller is asked for the
// caller) on GetEnv and the attach calls.
bool install_vm_wrappers(void** vm_table, const Platform& platform);

bool is_shadow_block(const void* object);
Counters counters();

// A thread is ending: its shadow blocks are queued, and freed (their global references deleted) by the
// next wrapped call that has an environment. `state` is what Platform::thread_state returned for it
// (the caller frees the memory). Calls no JNI function.
void release_thread_state(void* state);

// For the tests: forget the installation (the caller restores the table).
void reset_for_test();

}  // namespace oldapp

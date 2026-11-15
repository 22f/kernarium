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
    // The static native-method thunks (arm32 assembly): the entry of thunk `index`, which jumps to the function the
    // layer registered for it (thunk_enter/thunk_leave below) with a frame record pushed. Null or zero: no thunks, and
    // the blocks of a native method end only with their references.
    void* (*thunk_address)(unsigned index);
    unsigned thunk_capacity;
};

struct Counters {
    uint32_t shadows_created;
    uint32_t shadows_live;
    uint32_t shadows_declined;   // a result that stayed the real reference: the thread's or the region's limit
    uint32_t conversions;        // shadows turned back into the real array for the runtime
    uint32_t shadows_released;   // blocks of ended threads that were freed later
    uint32_t peak_thread_blocks; // the most live blocks any one thread has held
    uint32_t ended_by_return;    // references ended by the return of a native method
    uint32_t max_ended_by_return;  // the most references one return has ended (a native method that made many)
    uint32_t ended_by_pop;       // ... by PopLocalFrame
    uint32_t ended_by_detach;    // ... by DetachCurrentThread
    uint32_t natives_wrapped;    // distinct native functions that got a thunk
    uint32_t natives_unwrapped;  // application natives that got none (the thunks were all in use)
};

// `table` is the shared function table (kSlotCount entries), `shadow_region` the memory of the byte[]
// blocks (null: nothing to intercept). Returns false, changing nothing, when the table cannot be
// written.
bool install_jni_wrappers(void** table, const Platform& platform, void* shadow_region, size_t shadow_size);

// The invoke interface: the "a library may have loaded" notification (is_app_caller is asked for the caller) on
// GetEnv and the attach calls, and the end of a thread's references at DetachCurrentThread.
bool install_vm_wrappers(void** vm_table, const Platform& platform);

// ---- the end of a native method

// A native method of the application's own library gets a thunk: its entry (an address of thunk_address) pushes a
// frame record, sets the return address to a stub and jumps to the function, so the blocks the method made end when it
// returns, as Dalvik's local references did. thunk_for() is the function itself where there is no thunk (not
// an application function, none left, or the layer's shadows are off).
void* thunk_for(void* function);

struct ThunkEntry {
    void* target;      // the function to jump to
    uint32_t pushed;   // 1: a record was pushed, and the return stub must run thunk_leave
};
// The thunk's entry and the return stub, from the assembly (oldapp_thunks.S) and the host tests. `sp` is the stack
// pointer the native method is entered and left with.
ThunkEntry thunk_enter(unsigned index, void* return_address, void* sp);
void* thunk_leave(void* sp);  // the return address thunk_enter was given

bool is_shadow_block(const void* object);
Counters counters();

// A thread is ending: its shadow blocks are queued, and freed (their global references deleted) by the
// next wrapped call that has an environment. `state` is what Platform::thread_state returned for it
// (the caller frees the memory). Calls no JNI function.
void release_thread_state(void* state);

// For the tests: forget the installation (the caller restores the table).
void reset_for_test();

}  // namespace oldapp

// What the assembly calls: thunk_enter with the two words of a 64-bit result (the target in r0, the pushed flag in
// r1), and thunk_leave.
extern "C" uint64_t oldapp_thunk_enter(unsigned index, void* return_address, void* sp);
extern "C" void* oldapp_thunk_leave(void* sp);

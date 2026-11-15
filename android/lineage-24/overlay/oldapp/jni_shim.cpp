// SPDX-License-Identifier: Apache-2.0

#include "jni_shim.hpp"

#include <stdarg.h>

#include "low_arena.hpp"

namespace oldapp {
namespace {

using namespace jni;

constexpr int kMaxParams = 255;      // the JVM's limit on parameter slots
constexpr int kInlineParams = 12;    // kinds kept in the registry entry; the rest go to a pool
constexpr uint32_t kKindPoolBytes = 32768;
constexpr uint32_t kMethodSlots = 8192;
constexpr uint32_t kFieldSlots = 2048;
constexpr unsigned kProbeLimit = 32;
constexpr unsigned kMaxBlocks = 512;  // Dalvik's limit on local references; a thread has no more live references
constexpr unsigned kMaxFrames = 64;   // frame records of a thread (PushLocalFrame and native invocations)
constexpr unsigned kMaxThunks = 1024; // native functions that can get a thunk
constexpr unsigned kRegisterChunk = 32;  // RegisterNatives methods copied at a time
constexpr uint32_t kDalvikArrayClass = 0x5a324241u;
constexpr uint32_t kBlockLive = 0xb10c11feu;
constexpr uint32_t kBlockEnded = 0xe4ded10cu;  // the references ended, the array is not collected yet
constexpr uint32_t kBlockDead = 0xdeadb10cu;
constexpr unsigned kEndedBuckets = 4096;        // the ended index, by the array's identity hash
constexpr unsigned kMaxEnded = 16384;           // ended blocks: well below ART's 51200 weak global references
constexpr unsigned kSweepStep = 32;             // ended blocks tested at a creation after a collection
constexpr uint32_t kCollectIntervalMs = 10000;  // between two Runtime.gc() calls made for room
constexpr size_t kBlockTail = 16;  // zeroes behind the data: raw readers overrun by a few bytes
constexpr uint32_t kChunk = 4096;  // the unit of the block's synchronization with its array

template <class T>
struct IsVoid {
    static constexpr bool value = false;
};
template <>
struct IsVoid<void> {
    static constexpr bool value = true;
};
template <class T>
struct IsObject {
    static constexpr bool value = false;
};
template <>
struct IsObject<jobject> {
    static constexpr bool value = true;
};

// The Dalvik (Android 1.5 to 3.2) layout of an array object: Object {clazz, lock}, length, and the
// contents on the next 8-byte boundary.
struct DalvikArray {
    uint32_t clazz;
    uint32_t lock;
    uint32_t length;
    uint32_t pad;
    uint8_t data[8];
};
static_assert(offsetof(DalvikArray, length) == 8, "Dalvik ArrayObject length offset");
static_assert(offsetof(DalvikArray, data) == 16, "Dalvik ArrayObject contents offset");

// The layer's own header in front of the Dalvik object.
struct BlockHeader {
    uint32_t magic;
    uint32_t capacity;   // the length of the real array: the only length the layer trusts
    jobject global;      // a global reference to the real array (a live block; null once it has ended)
    BlockHeader* next;   // the queue of blocks that wait for an environment
    uint8_t* baseline;   // the bytes as of the last synchronization (outside the low arena)
    uint32_t refs;       // the references the application holds to this block
    jobject weak;        // an ended block: a weak global reference to the array
    uint32_t hash;       // the identity hash of the array, for the index of the ended blocks
    BlockHeader* bucket_next;  // the ended index: the chain of the hash's bucket, the newest first
    BlockHeader* list_next;    // the ended list, in the order the blocks ended (the collection sweeps)
    BlockHeader* list_prev;
};
constexpr size_t kBlockHead = (sizeof(BlockHeader) + 15) & ~size_t{15};

struct MethodInfo {
    void* id;
    uint8_t nparams;
    uint8_t usable;  // false: the kinds are not known (pool exhausted, or the method could not be described)
    uint8_t returns_bytes;
    char kinds[kInlineParams];
    const char* more;  // kinds from kInlineParams on
};

// A frame of the thread's local references: the entries below `height` belong to outer frames.
enum FrameKind : uint8_t { kFramePush = 1, kFrameNative = 2 };
struct FrameRecord {
    uint16_t height;
    uint8_t kind;
    void* lr;  // kFrameNative: the return address into the runtime
    void* sp;  // kFrameNative: the stack pointer the native method was entered with
};

// The references the application holds to byte[] blocks, in the order it got them: one entry for each result, so a
// block the same array was returned as twice has two entries (`refs` counts them), as Dalvik had two local references.
struct ThreadState {
    BlockHeader* blocks[kMaxBlocks];
    unsigned count;
    uint32_t bytes;  // the capacity of the live blocks
    Env* env;        // this thread's environment, as of the last entry made (the ends of references need it)
    FrameRecord frames[kMaxFrames];
    unsigned frame_count;
    unsigned frame_overflow;  // PushLocalFrame calls past kMaxFrames: their pops end nothing
};

struct Globals {
    Platform platform;
    void* orig[kSlotCount];
    void* orig_vm[kVmSlotCount];
    bool installed;
    bool vm_installed;
    bool shadows_on;
    Arena shadow_arena;
    const uint8_t* shadow_lo;
    const uint8_t* shadow_hi;
    BlockHeader* pending;  // blocks of ended threads, waiting for an environment
    uint32_t declined_logged;  // the first declined shadow of the process has been logged
    uint32_t peak_logged;      // the highest block count that has been logged, and when
    uint32_t peak_logged_ms;
    uint32_t lock;
    uint32_t thunk_lock;
    void* thunk_target[kMaxThunks];
    unsigned thunk_count;
    unsigned thunk_capacity;
    uint32_t thunk_none_logged;   // the first application native that got no thunk has been logged
    uint32_t ended_logged;        // the first block that ended at a return has been logged
    uint32_t ended_max_logged;    // the highest number of references one return has ended that has been logged, and when
    uint32_t ended_max_logged_ms;
    // The ended blocks (see "ended blocks" below); everything here is under the ended lock.
    uint32_t ended_lock;       // the spin lock, where the platform gives no mutex
    BlockHeader* ended_bucket[kEndedBuckets];
    BlockHeader ended_head;    // the anchor of the circular list of the ended blocks
    BlockHeader* sweep_cursor; // the next ended block a sweep tests
    bool sweeping;             // a collection was seen: the list is being tested, kSweepStep blocks at a creation
    jobject sentinel;          // a weak reference to an array nothing holds: a collection clears it
    unsigned ended_count;
    uint64_t ended_bytes;
    uint32_t ids_state;        // 0 not looked up, 1 found, 2 not found
    jclass system_class;       // global references to the classes of the calls below
    jmethodID hash_method;     // System.identityHashCode(Object)
    jclass runtime_class;
    jmethodID runtime_get;     // Runtime.getRuntime()
    jmethodID runtime_gc;      // Runtime.gc()
    uint32_t collect_ms;       // when Runtime.gc() was last asked for
    uint32_t collect_asked;    // it has been asked for
    uint32_t collect_logged;
    uint32_t kind_pool_used;
    char kind_pool[kKindPoolBytes];
    MethodInfo methods[kMethodSlots];
    void* byte_fields[kFieldSlots];
    Counters counters;
};

Globals g;

void lock_registry() {
    while (__atomic_exchange_n(&g.lock, 1u, __ATOMIC_ACQUIRE) != 0) {
    }
}
void unlock_registry() { __atomic_store_n(&g.lock, 0u, __ATOMIC_RELEASE); }

template <class F>
F original(int slot) {
    return reinterpret_cast<F>(g.orig[slot]);
}

bool from_app(const void* return_address) { return g.platform.is_app_caller(return_address); }

inline bool is_shadow(const void* object) {
    const uint8_t* p = static_cast<const uint8_t*>(object);
    return p >= g.shadow_lo && p < g.shadow_hi;
}

inline uint32_t live_shadows() { return __atomic_load_n(&g.counters.shadows_live, __ATOMIC_RELAXED); }

ThreadState* thread() { return static_cast<ThreadState*>(g.platform.thread_state(sizeof(ThreadState))); }

// ---- shadow blocks

BlockHeader* header_of(const void* block) {
    return reinterpret_cast<BlockHeader*>(const_cast<uint8_t*>(static_cast<const uint8_t*>(block)) - kBlockHead);
}

// The header of a block that is live, otherwise null (a freed block, or an address that is not a block).
BlockHeader* live_header(const void* object) {
    if (!is_shadow(object)) {
        return nullptr;
    }
    BlockHeader* header = header_of(object);
    return header->magic == kBlockLive ? header : nullptr;
}

DalvikArray* array_of(BlockHeader* header) {
    return reinterpret_cast<DalvikArray*>(reinterpret_cast<uint8_t*>(header) + kBlockHead);
}

// What the runtime is handed in place of `object`: the real array of a shadow (null for a block that has
// been freed), anything else unchanged.
jobject conv(jobject object) {
    if (!is_shadow(object)) {
        return object;
    }
    BlockHeader* header = live_header(object);
    __atomic_fetch_add(&g.counters.conversions, 1u, __ATOMIC_RELAXED);
    return header != nullptr ? header->global : nullptr;
}

// A block with no reference left is not freed: see end_block (ended blocks, below).
void end_block(Env* env, BlockHeader* header);

// Whether a JNI call may follow: not with an exception pending (almost no function may run then).
bool jni_usable(Env* env) { return !original<jboolean (*)(Env*)>(slot::ExceptionCheck)(env); }

// A live block that cannot become an ended one (no weak reference to give): its global reference and its memory go.
void destroy_block(Env* env, BlockHeader* header) {
    header->magic = kBlockDead;
    original<void (*)(Env*, jobject)>(slot::DeleteGlobalRef)(env, header->global);
    if (header->baseline != nullptr) {
        g.platform.free_baseline(header->baseline);
    }
    g.shadow_arena.release(header);
    __atomic_fetch_sub(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
}

// Blocks that wait for an environment without a pending exception: a thread that ended, or an end with an exception
// pending.
void queue_pending(BlockHeader* header) {
    BlockHeader* head = __atomic_load_n(&g.pending, __ATOMIC_RELAXED);
    do {
        header->next = head;
    } while (!__atomic_compare_exchange_n(&g.pending, &head, header, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

// A reference of the thread ended: the block ends with its last reference. A thread that has made entries has an
// environment; without one (it cannot be), the block waits for the next wrapped call that has one.
void release_entry(Env* env, ThreadState* state, BlockHeader* header) {
    if (--header->refs > 0) {
        return;
    }
    state->bytes -= header->capacity;
    if (env != nullptr) {
        end_block(env, header);
        return;
    }
    queue_pending(header);
}

// Ends the entries above `height`, the newest first; returns how many.
unsigned end_entries(ThreadState* state, unsigned height) {
    unsigned ended = 0;
    while (state->count > height) {
        release_entry(state->env, state, state->blocks[--state->count]);
        ++ended;
    }
    return ended;
}

// The blocks that wait for an environment: with one, and no exception pending, their ends are finished.
void drain_pending(Env* env) {
    if (!jni_usable(env)) {
        return;  // the next call without an exception finishes them
    }
    BlockHeader* list = __atomic_exchange_n(&g.pending, static_cast<BlockHeader*>(nullptr), __ATOMIC_ACQ_REL);
    while (list != nullptr) {
        BlockHeader* next = list->next;
        end_block(env, list);
        __atomic_fetch_add(&g.counters.shadows_released, 1u, __ATOMIC_RELAXED);
        list = next;
    }
}

// Every wrapped function that has an environment starts here.
inline bool enter(Env* env, const void* return_address) {
    if (__atomic_load_n(&g.pending, __ATOMIC_RELAXED) != nullptr) {
        drain_pending(env);
    }
    return from_app(return_address);
}
inline void enter_any(Env* env) {
    if (__atomic_load_n(&g.pending, __ATOMIC_RELAXED) != nullptr) {
        drain_pending(env);
    }
}

// ---- the block and its array are one memory

// On Dalvik the application's pointer and Java saw the same bytes. Here a block and its array are two copies, kept
// alike by synchronizing them in chunks of kChunk bytes. Besides the block each one keeps a baseline: the bytes
// as of the last synchronization, in memory outside the low arena (it needs no low address). A chunk whose block
// differs from its baseline was written through the raw pointer since then, and that comparison is exact. At a
// synchronization point such a chunk goes to the array (when both sides changed a chunk, the application's bytes
// win); any other chunk takes the array's bytes, which Java may have written, into baseline and block. A chunk
// neither side touched costs two comparisons and a copy in, and writes nothing to the array or the block.
// A run of adjacent chunks that are alike is one region call.
// The synchronization points: before a call that is passed the block, as an argument or as the receiver (the
// application's chunks go to the array), after that call, at the start of Get...Elements, Get...Critical and
// GetByteArrayRegion (the chunks of the range), before and after SetByteArrayRegion, and at a Release. A
// committing release (mode 0 or COMMIT) applies the same rule; JNI_ABORT discards the working copy: the block takes
// the array's bytes. A call that left an exception pending is not followed by a synchronization: almost no JNI
// function may run then. Only the header's capacity is trusted, never the length word of the Dalvik object, which
// the application can overwrite.
inline unsigned chunk_count(size_t bytes) { return static_cast<unsigned>((bytes + kChunk - 1) / kChunk); }

inline uint8_t* data_of(BlockHeader* header) { return array_of(header)->data; }
inline uint32_t chunk_offset(unsigned chunk) { return chunk * kChunk; }
inline uint32_t chunk_end(const BlockHeader* header, unsigned chunk) {
    const uint64_t end = static_cast<uint64_t>(chunk + 1) * kChunk;
    return end < header->capacity ? static_cast<uint32_t>(end) : header->capacity;
}

// Whether the application wrote the chunk through the raw pointer since the last synchronization.
bool chunk_dirty(BlockHeader* header, unsigned chunk) {
    const uint32_t offset = chunk_offset(chunk);
    return __builtin_memcmp(data_of(header) + offset, header->baseline + offset, chunk_end(header, chunk) - offset) != 0;
}

// The array's bytes (already in the baseline over [from, to)) reach the block where they differ from it.
void adopt_baseline(BlockHeader* header, uint32_t from, uint32_t to) {
    for (uint32_t at = from; at < to; at += kChunk) {
        const uint32_t length = to - at < kChunk ? to - at : kChunk;
        if (__builtin_memcmp(data_of(header) + at, header->baseline + at, length) != 0) {
            __builtin_memcpy(data_of(header) + at, header->baseline + at, length);
        }
    }
}

// The chunks that overlap [start, end) (inside the block): the application's chunks go to the array and, when
// `pull` is set, the others take the array's bytes over the range. `pull` clear is the part a call needs before it
// runs.
void sync_bytes(Env* env, BlockHeader* header, uint32_t start, uint32_t end, bool pull) {
    if (end <= start || header->capacity == 0) {
        return;
    }
    const unsigned last = (end - 1) / kChunk;
    unsigned chunk = start / kChunk;
    bool dirty = chunk_dirty(header, chunk);
    while (chunk <= last) {
        const unsigned first = chunk;
        const bool run_dirty = dirty;
        bool next_dirty = false;
        while (chunk < last) {  // the run of chunks that are alike; the first chunk that is not is compared once
            next_dirty = chunk_dirty(header, chunk + 1);
            if (next_dirty != run_dirty) {
                break;
            }
            ++chunk;
        }
        if (run_dirty) {
            const uint32_t from = chunk_offset(first), to = chunk_end(header, chunk);
            original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(
                env, header->global, static_cast<jsize>(from), static_cast<jsize>(to - from),
                reinterpret_cast<const jbyte*>(data_of(header) + from));
            __builtin_memcpy(header->baseline + from, data_of(header) + from, to - from);
        } else if (pull) {
            uint32_t from = chunk_offset(first), to = chunk_end(header, chunk);
            from = from < start ? start : from;
            to = to > end ? end : to;
            original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
                env, header->global, static_cast<jsize>(from), static_cast<jsize>(to - from),
                reinterpret_cast<jbyte*>(header->baseline + from));
            adopt_baseline(header, from, to);
        }
        if (chunk == last) {
            break;
        }
        ++chunk;
        dirty = next_dirty;
    }
}
void sync_all(Env* env, BlockHeader* header) { sync_bytes(env, header, 0, header->capacity, true); }

// Before a call that is passed the block: what the application wrote through the pointer reaches the array. Nothing
// is read back, and a clean chunk costs no array write.
void push_dirty(Env* env, BlockHeader* header) { sync_bytes(env, header, 0, header->capacity, false); }

// A JNI_ABORT: the block is given the array's bytes back, whatever the application wrote.
void abort_block(Env* env, BlockHeader* header) {
    if (header->capacity == 0) {
        return;
    }
    original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
        env, header->global, 0, static_cast<jsize>(header->capacity), reinterpret_cast<jbyte*>(header->baseline));
    adopt_baseline(header, 0, header->capacity);
}

// The same rules through the real array's critical pair, the only JNI calls that CheckJNI allows inside a critical
// region (the application may be in one for another array when it asks for this block's critical pointer). The pair
// is released with mode 0 only when a chunk went to the array.
void sync_critical(Env* env, BlockHeader* header) {
    if (header->capacity == 0) {
        return;
    }
    void* pinned = original<void* (*)(Env*, jarray, jboolean*)>(slot::GetPrimitiveArrayCritical)(env, header->global,
                                                                                               nullptr);
    if (pinned == nullptr) {
        return;
    }
    bool wrote = false;
    for (unsigned c = 0, n = chunk_count(header->capacity); c < n; ++c) {
        const uint32_t offset = chunk_offset(c), length = chunk_end(header, c) - offset;
        if (chunk_dirty(header, c)) {
            __builtin_memcpy(static_cast<uint8_t*>(pinned) + offset, data_of(header) + offset, length);
            __builtin_memcpy(header->baseline + offset, data_of(header) + offset, length);
            wrote = true;
        } else {
            __builtin_memcpy(header->baseline + offset, static_cast<const uint8_t*>(pinned) + offset, length);
            adopt_baseline(header, offset, offset + length);
        }
    }
    original<void (*)(Env*, jarray, void*, jint)>(slot::ReleasePrimitiveArrayCritical)(
        env, header->global, pinned, wrote ? 0 : kReleaseAbort);
}
void abort_critical(Env* env, BlockHeader* header) {
    if (header->capacity == 0) {
        return;
    }
    void* pinned = original<void* (*)(Env*, jarray, jboolean*)>(slot::GetPrimitiveArrayCritical)(env, header->global,
                                                                                               nullptr);
    if (pinned == nullptr) {
        return;
    }
    __builtin_memcpy(header->baseline, pinned, header->capacity);
    adopt_baseline(header, 0, header->capacity);
    original<void (*)(Env*, jarray, void*, jint)>(slot::ReleasePrimitiveArrayCritical)(env, header->global, pinned,
                                                                                      kReleaseAbort);
}

template <class R, class F>
R call_synced(Env* env, BlockHeader* const* touched, unsigned count, F&& call) {
    if (count != 0 && jni_usable(env)) {
        for (unsigned i = 0; i < count; ++i) {
            if (touched[i]->magic == kBlockLive) {
                push_dirty(env, touched[i]);
            }
        }
    }
    const auto after = [&] {
        if (count != 0 && jni_usable(env)) {
            for (unsigned i = 0; i < count; ++i) {
                if (touched[i]->magic == kBlockLive) {
                    sync_all(env, touched[i]);
                }
            }
        }
    };
    if constexpr (IsVoid<R>::value) {
        call();
        after();
    } else {
        R result = call();
        after();
        return result;
    }
}

// ---- ended blocks
//
// A block whose references have all ended (a native method returned, a frame was popped, DeleteLocalRef) is not freed.
// Its array may be held by Java still, and the application may keep the pointer: on Dalvik the object stayed where it
// was for as long as it was reachable. So the block stays at its address with its bytes and its baseline until the array
// is collected. At the end, what the application wrote through the pointer goes to the array, and the block's strong
// reference gives way to a weak one. The ended blocks are in one list, in the order they ended, and in an index by the
// array's identity hash. The same array obtained again gets its block back, wherever the pointer was kept, so Java's
// later writes and the application's later raw writes meet in it (point 3: the merge of the synchronization).
//
// How the layer learns of a collection: IsSameObject(weak, NULL), the JNI specification's test for a cleared
// reference. A sentinel, a weak reference to an array nothing holds, says when to look: every collection clears it, since
// it is younger than the last one; the first creation of a block after that starts testing the ended blocks, kSweepStep
// at each creation. The tests run only at the creation of a block, never at a native return, so a call that makes no
// block pays nothing. Room is made on demand: a full sweep, then a Runtime.gc() (at most every kCollectIntervalMs), then
// a full sweep again. The sentinel is a hint: that full sweep never reads it.
//
// The index, the list, the cursor and the frees are under one process-wide lock. The JNI calls made under it
// (IsSameObject, New/DeleteWeakGlobalRef, NewGlobalRef) go to the runtime's own functions and never re-enter the layer;
// Runtime.gc() runs outside the lock. Nothing here runs with an exception pending, and the critical wrappers never
// start it, so no call here is made inside a critical region.

void lock_ended() {
    if (g.platform.mutex_lock != nullptr) {
        g.platform.mutex_lock();
        return;
    }
    while (__atomic_exchange_n(&g.ended_lock, 1u, __ATOMIC_ACQUIRE) != 0) {
    }
}
void unlock_ended() {
    if (g.platform.mutex_unlock != nullptr) {
        g.platform.mutex_unlock();
        return;
    }
    __atomic_store_n(&g.ended_lock, 0u, __ATOMIC_RELEASE);
}
struct EndedLock {
    EndedLock() { lock_ended(); }
    ~EndedLock() { unlock_ended(); }
    EndedLock(const EndedLock&) = delete;
    EndedLock& operator=(const EndedLock&) = delete;
};

void publish_ended() {
    __atomic_store_n(&g.counters.shadows_ended, g.ended_count, __ATOMIC_RELAXED);
    __atomic_store_n(&g.counters.ended_bytes, static_cast<uint32_t>(g.ended_bytes), __ATOMIC_RELAXED);
}

// Under the lock.
void ended_add(BlockHeader* block) {
    block->list_prev = g.ended_head.list_prev;
    block->list_next = &g.ended_head;
    g.ended_head.list_prev->list_next = block;
    g.ended_head.list_prev = block;
    BlockHeader** chain = &g.ended_bucket[block->hash % kEndedBuckets];
    block->bucket_next = *chain;  // the newest first
    *chain = block;
    ++g.ended_count;
    g.ended_bytes += block->capacity;
    publish_ended();
}
void ended_remove(BlockHeader* block) {
    if (g.sweep_cursor == block) {
        g.sweep_cursor = block->list_next;
    }
    block->list_prev->list_next = block->list_next;
    block->list_next->list_prev = block->list_prev;
    BlockHeader** chain = &g.ended_bucket[block->hash % kEndedBuckets];
    while (*chain != block) {
        chain = &(*chain)->bucket_next;
    }
    *chain = block->bucket_next;
    --g.ended_count;
    g.ended_bytes -= block->capacity;
    publish_ended();
}

// The array of an ended block is gone: the block, its baseline and its weak reference go.
void reclaim_ended(Env* env, BlockHeader* block) {
    ended_remove(block);
    block->magic = kBlockDead;
    original<void (*)(Env*, jobject)>(slot::DeleteWeakGlobalRef)(env, block->weak);
    if (block->baseline != nullptr) {
        g.platform.free_baseline(block->baseline);
    }
    g.shadow_arena.release(block);
    __atomic_fetch_add(&g.counters.blocks_collected, 1u, __ATOMIC_RELAXED);
}

bool cleared(Env* env, jobject weak) {
    return original<jboolean (*)(Env*, jobject, jobject)>(slot::IsSameObject)(env, weak, nullptr) != 0;
}

// Tests the ended blocks from the cursor, `limit` of them (0: all, from the first); reclaims the ones whose arrays are
// gone. The sweep ends where the list does.
void sweep(Env* env, unsigned limit) {
    BlockHeader* node = limit == 0 ? g.ended_head.list_next : g.sweep_cursor;
    unsigned tested = 0;
    while (node != &g.ended_head && (limit == 0 || tested < limit)) {
        BlockHeader* next = node->list_next;
        ++tested;
        if (cleared(env, node->weak)) {
            reclaim_ended(env, node);
        }
        node = next;
    }
    g.sweep_cursor = node;
    if (node == &g.ended_head) {
        g.sweeping = false;
        g.sweep_cursor = g.ended_head.list_next;
    }
}

void renew_sentinel(Env* env) {
    if (g.sentinel != nullptr) {
        original<void (*)(Env*, jobject)>(slot::DeleteWeakGlobalRef)(env, g.sentinel);
        g.sentinel = nullptr;
    }
    jobject array = original<jobject (*)(Env*, jsize)>(slot::NewByteArray)(env, 0);
    if (array == nullptr) {
        original<void (*)(Env*)>(slot::ExceptionClear)(env);
        return;
    }
    g.sentinel = original<jobject (*)(Env*, jobject)>(slot::NewWeakGlobalRef)(env, array);
    original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, array);
}

// At the creation of a block, under the lock: whether a collection ran since the last look, and then the next
// kSweepStep ended blocks.
void collection_step(Env* env) {
    if (g.ended_count == 0) {
        return;
    }
    if (!g.sweeping) {
        if (g.sentinel != nullptr && !cleared(env, g.sentinel)) {
            return;
        }
        renew_sentinel(env);
        g.sweeping = true;
        g.sweep_cursor = g.ended_head.list_next;
    }
    sweep(env, kSweepStep);
}

// System.identityHashCode and Runtime.gc(), looked up once (through the runtime's own table).
bool find_ids(Env* env) {
    const auto find_class = original<jclass (*)(Env*, const char*)>(slot::FindClass);
    const auto global = original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef);
    const auto delete_local = original<void (*)(Env*, jobject)>(slot::DeleteLocalRef);
    const auto static_method = original<jmethodID (*)(Env*, jclass, const char*, const char*)>(slot::GetStaticMethodID);
    const auto method = original<jmethodID (*)(Env*, jclass, const char*, const char*)>(slot::GetMethodID);
    jclass system = find_class(env, "java/lang/System");
    jclass runtime = system != nullptr ? find_class(env, "java/lang/Runtime") : nullptr;
    if (system != nullptr && runtime != nullptr) {
        g.system_class = static_cast<jclass>(global(env, system));
        g.runtime_class = static_cast<jclass>(global(env, runtime));
        g.hash_method = static_method(env, g.system_class, "identityHashCode", "(Ljava/lang/Object;)I");
        g.runtime_get = static_method(env, g.runtime_class, "getRuntime", "()Ljava/lang/Runtime;");
        g.runtime_gc = method(env, g.runtime_class, "gc", "()V");
    }
    if (system != nullptr) delete_local(env, system);
    if (runtime != nullptr) delete_local(env, runtime);
    const bool ok = g.system_class != nullptr && g.runtime_class != nullptr && g.hash_method != nullptr &&
                    g.runtime_get != nullptr && g.runtime_gc != nullptr;
    if (!ok) {
        original<void (*)(Env*)>(slot::ExceptionClear)(env);
    }
    return ok;
}
bool ensure_ids(Env* env) {
    uint32_t state = __atomic_load_n(&g.ids_state, __ATOMIC_ACQUIRE);
    if (state == 0) {
        EndedLock lock;
        if (g.ids_state == 0) {
            __atomic_store_n(&g.ids_state, find_ids(env) ? 1u : 2u, __ATOMIC_RELEASE);
        }
        state = g.ids_state;
    }
    return state == 1;
}

// The identity hash of the array `local` (a real reference). False where it cannot be had: the array is then no
// ended block's again.
bool identity_hash(Env* env, jobject local, uint32_t* hash) {
    if (!ensure_ids(env)) {
        return false;
    }
    jvalue argument;
    argument.j = 0;
    argument.l = local;
    const jint value = original<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(static_call_slot(kInt, kArray))(
        env, g.system_class, g.hash_method, &argument);
    if (!jni_usable(env)) {
        original<void (*)(Env*)>(slot::ExceptionClear)(env);
        return false;
    }
    *hash = static_cast<uint32_t>(value);
    return true;
}

// The end of a block's last reference. What the application wrote through the pointer goes to the array, the strong
// reference gives way to a weak one, and the block waits in the index. With an exception pending none of that may run
// (NewWeakGlobalRef and SetByteArrayRegion are outside the functions CheckJNI allows then): the block waits for the next
// call without one, still strong.
void end_block(Env* env, BlockHeader* header) {
    if (!jni_usable(env)) {
        queue_pending(header);
        return;
    }
    push_dirty(env, header);
    jobject weak = original<jobject (*)(Env*, jobject)>(slot::NewWeakGlobalRef)(env, header->global);
    if (weak == nullptr) {
        original<void (*)(Env*)>(slot::ExceptionClear)(env);
        destroy_block(env, header);  // no weak reference to give: the block goes, as with no layer to keep it
        return;
    }
    original<void (*)(Env*, jobject)>(slot::DeleteGlobalRef)(env, header->global);
    header->global = nullptr;
    header->weak = weak;
    {
        EndedLock lock;
        header->magic = kBlockEnded;
        ended_add(header);
        if (g.sentinel == nullptr) {
            renew_sentinel(env);
        }
    }
    __atomic_fetch_sub(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g.counters.blocks_ended, 1u, __ATOMIC_RELAXED);
}

// The ended block for the array `local` (identity hash `hash`), turned live again with a new global reference; null if
// there is none. Two ended blocks can exist for one array (it was live on two threads): the newest is found first.
// `no_global` is set when the global reference could not be had.
BlockHeader* take_ended(Env* env, jobject local, uint32_t hash, size_t bytes, bool* no_global) {
    for (BlockHeader* block = g.ended_bucket[hash % kEndedBuckets]; block != nullptr; block = block->bucket_next) {
        if (block->hash != hash || block->capacity != bytes ||
            original<jboolean (*)(Env*, jobject, jobject)>(slot::IsSameObject)(env, local, block->weak) == 0) {
            continue;
        }
        jobject global = original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef)(env, local);
        if (global == nullptr) {
            *no_global = true;
            return nullptr;
        }
        ended_remove(block);
        original<void (*)(Env*, jobject)>(slot::DeleteWeakGlobalRef)(env, block->weak);
        block->weak = nullptr;
        block->global = global;
        block->magic = kBlockLive;
        return block;
    }
    return nullptr;
}

// Asks the runtime to collect, at most every kCollectIntervalMs: true if it did. The ended blocks only wait for a
// collection, and a pause once in a while is better than a result that stays the real reference.
bool ask_for_collection(Env* env) {
    const uint32_t now = g.platform.now_ms != nullptr ? g.platform.now_ms() : 0;
    if (g.platform.now_ms != nullptr && __atomic_load_n(&g.collect_asked, __ATOMIC_RELAXED) != 0 &&
        now - __atomic_load_n(&g.collect_ms, __ATOMIC_RELAXED) < kCollectIntervalMs) {
        return false;
    }
    if (!ensure_ids(env)) {
        return false;
    }
    __atomic_store_n(&g.collect_ms, now, __ATOMIC_RELAXED);
    __atomic_store_n(&g.collect_asked, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g.counters.collections_asked, 1u, __ATOMIC_RELAXED);
    if (g.platform.log != nullptr && __atomic_exchange_n(&g.collect_logged, 1u, __ATOMIC_RELAXED) == 0) {
        g.platform.log("a byte[] block did not fit: the runtime was asked to collect; ended blocks and bytes waiting:",
                       static_cast<long>(g.counters.shadows_ended), static_cast<long>(g.counters.ended_bytes));
    }
    jobject runtime = original<jobject (*)(Env*, jclass, jmethodID, const jvalue*)>(static_call_slot(kObject, kArray))(
        env, g.runtime_class, g.runtime_get, nullptr);
    if (runtime != nullptr) {
        original<void (*)(Env*, jobject, jmethodID, const jvalue*)>(call_slot(kVoid, kArray))(env, runtime, g.runtime_gc,
                                                                                               nullptr);
        original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, runtime);
    }
    if (!jni_usable(env)) {
        original<void (*)(Env*)>(slot::ExceptionClear)(env);
    }
    return true;
}

// The diagnosis lines of the log (the sessions of real games read them): the first result that stayed the real
// reference, with the blocks and bytes the thread held, and the most blocks any thread has held, at most every
// kPeakLogMs while it grows.
constexpr uint32_t kPeakLogMs = 30000;

void note_declined(const ThreadState* state, const char* why) {
    ++g.counters.shadows_declined;
    if (g.platform.log != nullptr && __atomic_exchange_n(&g.declined_logged, 1u, __ATOMIC_RELAXED) == 0) {
        g.platform.log(why, state != nullptr ? static_cast<long>(state->count) : 0,
                       state != nullptr ? static_cast<long>(state->bytes) : 0);
        // The ended blocks that still wait for a collection are a reason a result may not fit.
        g.platform.log("ended byte[] blocks waiting for their arrays' collection when the first result was not made a "
                       "block; blocks and bytes:",
                       static_cast<long>(g.counters.shadows_ended), static_cast<long>(g.counters.ended_bytes));
    }
}

void note_peak(uint32_t blocks) {
    uint32_t peak = __atomic_load_n(&g.counters.peak_thread_blocks, __ATOMIC_RELAXED);
    while (blocks > peak && !__atomic_compare_exchange_n(&g.counters.peak_thread_blocks, &peak, blocks, true,
                                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    if (blocks > peak) {
        peak = blocks;
    }
    uint32_t logged = __atomic_load_n(&g.peak_logged, __ATOMIC_RELAXED);
    if (g.platform.log == nullptr || peak == logged) {
        return;
    }
    const uint32_t now = g.platform.now_ms != nullptr ? g.platform.now_ms() : 0;
    if (logged != 0 && g.platform.now_ms != nullptr &&
        now - __atomic_load_n(&g.peak_logged_ms, __ATOMIC_RELAXED) < kPeakLogMs) {
        return;
    }
    if (__atomic_compare_exchange_n(&g.peak_logged, &logged, peak, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g.peak_logged_ms, now, __ATOMIC_RELAXED);
        g.platform.log("most live byte[] blocks one thread has held; blocks live in the process:", peak,
                       static_cast<long>(live_shadows()));
    }
}

// The most recent blocks of a thread that are looked at for the same array before a new block is made.
constexpr unsigned kReuseWindow = 8;

// The memory of one block: the arena and the baseline, unless the ended blocks are at their cap. Null with a reason.
struct Room {
    uint8_t* raw;
    uint8_t* baseline;
    const char* why;
};
Room allocate_block(size_t need, size_t bytes) {
    Room room{nullptr, nullptr, nullptr};
    if (__atomic_load_n(&g.counters.shadows_ended, __ATOMIC_RELAXED) >= kMaxEnded) {
        room.why = "the first byte[] result stayed a real reference (too many ended blocks); blocks and bytes held:";
        return room;
    }
    room.raw = static_cast<uint8_t*>(g.shadow_arena.allocate(need));
    if (room.raw == nullptr) {
        room.why = "the first byte[] result stayed a real reference (no room in the region); blocks and bytes held:";
        return room;
    }
    if (bytes > 0) {
        room.baseline = static_cast<uint8_t*>(g.platform.alloc_baseline(bytes));
        if (room.baseline == nullptr) {
            g.shadow_arena.release(room.raw);
            room.raw = nullptr;
            room.why = "the first byte[] result stayed a real reference (no memory for the baseline); blocks and bytes held:";
        }
    }
    return room;
}

// A shadow for the real local reference `local`, which it replaces: the block holds a global reference to the
// array, the local reference is deleted. An array the thread got a block for among its most recent ones is the same
// block again (counted once more, one DeleteLocalRef for each); so is an array whose block has ended and is not
// collected: the application gets the block it may still hold the pointer of. Null when the thread already has the
// limit or the region is full: the caller keeps the real reference, as it would without the layer.
jobject make_shadow(Env* env, jobject local) {
    ThreadState* state = thread();
    if (state == nullptr) {
        note_declined(state, "the first byte[] result stayed a real reference (no thread state); blocks and bytes held:");
        return nullptr;
    }
    const jsize length = original<jsize (*)(Env*, jarray)>(slot::GetArrayLength)(env, local);
    if (length < 0) {
        return nullptr;
    }
    const size_t bytes = static_cast<size_t>(length);
    if (state->count >= kMaxBlocks) {
        note_declined(state, "the first byte[] result stayed a real reference (thread limit); blocks and bytes held:");
        return nullptr;
    }
    for (unsigned i = 0; i < kReuseWindow && i < state->count; ++i) {
        BlockHeader* known = state->blocks[state->count - 1 - i];
        if (known->capacity == bytes &&
            original<jboolean (*)(Env*, jobject, jobject)>(slot::IsSameObject)(env, local, known->global) != 0) {
            ++known->refs;
            state->blocks[state->count++] = known;
            state->env = env;
            original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, local);
            sync_all(env, known);  // what Java wrote meanwhile shows, and what the application wrote reaches the array
            return array_of(known);
        }
    }
    if (__atomic_load_n(&g.pending, __ATOMIC_RELAXED) != nullptr) {
        drain_pending(env);  // blocks that waited for an environment: their ends come before this lookup
    }
    // The ended blocks: a sweep step if a collection ran, then the array's own block if it ended and is not collected.
    // Where the identity hash cannot be had, the array is made a new block.
    uint32_t hash = 0;
    const bool hashed = identity_hash(env, local, &hash);
    if (__atomic_load_n(&g.counters.shadows_ended, __ATOMIC_RELAXED) != 0) {
        BlockHeader* again = nullptr;
        bool no_global = false;
        {
            EndedLock lock;
            collection_step(env);
            if (hashed && g.ended_count != 0) {
                again = take_ended(env, local, hash, bytes, &no_global);
            }
        }
        if (no_global) {
            note_declined(state, "the first byte[] result stayed a real reference (no global reference); blocks and bytes held:");
            return nullptr;
        }
        if (again != nullptr) {
            again->refs = 1;
            state->blocks[state->count++] = again;
            state->env = env;
            state->bytes += static_cast<uint32_t>(bytes);
            original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, local);
            __atomic_fetch_add(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g.counters.blocks_revived, 1u, __ATOMIC_RELAXED);
            sync_all(env, again);  // Java's writes since it ended and the application's raw writes since meet here
            note_peak(state->count);
            return array_of(again);
        }
    }
    const size_t need = kBlockHead + offsetof(DalvikArray, data) + bytes + kBlockTail;
    Room room = allocate_block(need, bytes);
    if (room.raw == nullptr && __atomic_load_n(&g.counters.shadows_ended, __ATOMIC_RELAXED) != 0) {
        // Ended blocks hold the room: those whose arrays are gone are freed; if that is not enough, the runtime is
        // asked to collect (at most every kCollectIntervalMs), outside the lock, and they are looked at again.
        {
            EndedLock lock;
            sweep(env, 0);
        }
        room = allocate_block(need, bytes);
        if (room.raw == nullptr && ask_for_collection(env)) {
            {
                EndedLock lock;
                sweep(env, 0);
            }
            room = allocate_block(need, bytes);
        }
    }
    if (room.raw == nullptr) {
        note_declined(state, room.why);
        return nullptr;
    }
    jobject global = original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef)(env, local);
    if (global == nullptr) {
        if (room.baseline != nullptr) {
            g.platform.free_baseline(room.baseline);
        }
        g.shadow_arena.release(room.raw);
        note_declined(state, "the first byte[] result stayed a real reference (no global reference); blocks and bytes held:");
        return nullptr;
    }
    BlockHeader* header = reinterpret_cast<BlockHeader*>(room.raw);
    header->magic = kBlockLive;
    header->capacity = static_cast<uint32_t>(bytes);
    header->global = global;
    header->next = nullptr;
    header->baseline = room.baseline;
    header->refs = 1;
    header->weak = nullptr;
    header->hash = hash;
    header->bucket_next = nullptr;
    header->list_next = nullptr;
    header->list_prev = nullptr;
    DalvikArray* block = array_of(header);
    block->clazz = kDalvikArrayClass;
    block->lock = 0;
    block->length = static_cast<uint32_t>(bytes);
    block->pad = 0;
    if (bytes > 0) {
        original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
            env, local, 0, length, reinterpret_cast<jbyte*>(block->data));
        __builtin_memcpy(room.baseline, block->data, bytes);
    }
    __builtin_memset(block->data + bytes, 0, kBlockTail);
    state->blocks[state->count++] = header;
    state->env = env;
    state->bytes += static_cast<uint32_t>(bytes);
    original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, local);
    __atomic_fetch_add(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
    ++g.counters.shadows_created;
    note_peak(state->count);
    return block;
}

// What an application gets for a reference-returning call: a shadow when the declared type is byte[].
jobject post_result(Env* env, bool bytes, jobject reference) {
    if (reference == nullptr || !bytes || !g.shadows_on) {
        return reference;
    }
    jobject shadow = make_shadow(env, reference);
    return shadow != nullptr ? shadow : reference;
}

// ---- signatures

unsigned hash_of(const void* id, unsigned modulus) {
    return static_cast<unsigned>((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(id) >> 2) * 2654435761u) %
                                 modulus);
}

char kind_at(const MethodInfo& info, unsigned index) {
    return index < kInlineParams ? info.kinds[index] : info.more[index - kInlineParams];
}

// The parameter kinds of a method signature into `kinds` (kMaxParams + 1 entries); false when it is malformed.
bool parse_signature(const char* sig, char* kinds, unsigned* count, bool* returns_bytes) {
    if (sig == nullptr || sig[0] != '(') {
        return false;
    }
    const char* p = sig + 1;
    unsigned n = 0;
    while (*p != '\0' && *p != ')') {
        char kind;
        switch (*p) {
            case 'Z': case 'B': case 'C': case 'S': case 'I': case 'J': case 'F': case 'D':
                kind = *p++;
                break;
            case 'L':
                while (*p != '\0' && *p != ';') {
                    ++p;
                }
                if (*p == '\0') {
                    return false;
                }
                ++p;
                kind = 'L';
                break;
            case '[':
                while (*p == '[') {
                    ++p;
                }
                if (*p == 'L') {
                    while (*p != '\0' && *p != ';') {
                        ++p;
                    }
                }
                if (*p == '\0') {
                    return false;
                }
                ++p;
                kind = 'L';
                break;
            default:
                return false;
        }
        if (n >= kMaxParams) {
            return false;
        }
        kinds[n++] = kind;
    }
    if (*p != ')') {
        return false;
    }
    ++p;
    *count = n;
    *returns_bytes = p[0] == '[' && p[1] == 'B' && p[2] == '\0';
    return true;
}

// Stores an entry for `id`; false when the registry or the pool is full.
bool store_method(void* id, const char* kinds, unsigned count, bool returns_bytes, bool usable = true) {
    lock_registry();
    MethodInfo parsed = {};
    parsed.nparams = static_cast<uint8_t>(count);
    parsed.returns_bytes = returns_bytes;
    parsed.usable = usable;
    for (unsigned i = 0; i < count && i < kInlineParams; ++i) {
        parsed.kinds[i] = kinds[i];
    }
    if (count > kInlineParams) {
        const unsigned extra = count - kInlineParams;
        if (g.kind_pool_used + extra > kKindPoolBytes) {
            parsed.usable = 0;  // the kinds are not kept: the call is forwarded unchanged
        } else {
            char* pool = g.kind_pool + g.kind_pool_used;
            for (unsigned i = 0; i < extra; ++i) {
                pool[i] = kinds[kInlineParams + i];
            }
            g.kind_pool_used += extra;
            parsed.more = pool;
        }
    }
    bool stored = false;
    unsigned index = hash_of(id, kMethodSlots);
    for (unsigned probe = 0; probe < kProbeLimit; ++probe, index = (index + 1) % kMethodSlots) {
        MethodInfo& entry = g.methods[index];
        if (entry.id == id) {
            stored = true;  // already known
            break;
        }
        if (entry.id == nullptr) {
            entry = parsed;
            entry.id = nullptr;
            __atomic_store_n(&entry.id, id, __ATOMIC_RELEASE);
            stored = true;
            break;
        }
    }
    unlock_registry();
    return stored;
}

void register_method(void* id, const char* sig) {
    if (id == nullptr) {
        return;
    }
    char kinds[kMaxParams + 1];
    unsigned count = 0;
    bool returns_bytes = false;
    if (parse_signature(sig, kinds, &count, &returns_bytes)) {
        store_method(id, kinds, count, returns_bytes);
    }
}

const MethodInfo* find_method(const void* id) {
    if (id == nullptr) {
        return nullptr;
    }
    unsigned index = hash_of(id, kMethodSlots);
    for (unsigned probe = 0; probe < kProbeLimit; ++probe, index = (index + 1) % kMethodSlots) {
        const MethodInfo& entry = g.methods[index];
        void* seen = __atomic_load_n(&entry.id, __ATOMIC_ACQUIRE);
        if (seen == id) {
            return &entry;
        }
        if (seen == nullptr) {
            return nullptr;
        }
    }
    return nullptr;
}

// A method ID the layer did not see being looked up (FromReflectedMethod, or one the registry dropped):
// asks the platform, once, what its parameters are. `object_or_class` is the receiver (an instance call) or
// the class (a static call or a constructor).
const MethodInfo* learn_method(Env* env, void* id, bool is_static, jobject object_or_class) {
    if (id == nullptr || object_or_class == nullptr || g.platform.describe_method == nullptr ||
        original<jboolean (*)(Env*)>(slot::ExceptionCheck)(env) != 0) {
        return nullptr;  // an exception is pending: no JNI function may be called
    }
    jobject hint = object_or_class;
    jobject owned = nullptr;
    if (!is_static) {
        owned = original<jobject (*)(Env*, jobject)>(slot::GetObjectClass)(env, object_or_class);
        hint = owned;
        if (hint == nullptr) {
            return nullptr;
        }
    }
    char kinds[kMaxParams + 1];
    unsigned count = 0;
    const bool described = g.platform.describe_method(env, g.orig, id, is_static, hint, kinds, kMaxParams, &count);
    if (owned != nullptr) {
        original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, owned);
    }
    if (!described) {
        store_method(id, kinds, 0, false, false);  // remember that it cannot be described: no new attempt
        return nullptr;
    }
    if (!store_method(id, kinds, count, false)) {
        return nullptr;
    }
    return find_method(id);
}

void register_byte_field(void* id, const char* sig) {
    if (id == nullptr || sig == nullptr || !(sig[0] == '[' && sig[1] == 'B' && sig[2] == '\0')) {
        return;
    }
    lock_registry();
    unsigned index = hash_of(id, kFieldSlots);
    for (unsigned probe = 0; probe < kProbeLimit; ++probe, index = (index + 1) % kFieldSlots) {
        if (g.byte_fields[index] == id) {
            break;
        }
        if (g.byte_fields[index] == nullptr) {
            __atomic_store_n(&g.byte_fields[index], id, __ATOMIC_RELEASE);
            break;
        }
    }
    unlock_registry();
}

bool is_byte_field(const void* id) {
    if (id == nullptr) {
        return false;
    }
    unsigned index = hash_of(id, kFieldSlots);
    for (unsigned probe = 0; probe < kProbeLimit; ++probe, index = (index + 1) % kFieldSlots) {
        void* seen = __atomic_load_n(&g.byte_fields[index], __ATOMIC_ACQUIRE);
        if (seen == id) {
            return true;
        }
        if (seen == nullptr) {
            return false;
        }
    }
    return false;
}

// ---- the Call families

enum class Kind { kInstance, kNonvirtual, kStatic, kConstructor };

template <class R, Kind K>
R call_original_v(int slot, Env* e, jobject l0, jobject l1, jmethodID m, va_list ap) {
    if constexpr (K == Kind::kNonvirtual) {
        return reinterpret_cast<R (*)(Env*, jobject, jobject, jmethodID, va_list)>(g.orig[slot])(e, l0, l1, m, ap);
    } else {
        return reinterpret_cast<R (*)(Env*, jobject, jmethodID, va_list)>(g.orig[slot])(e, l0, m, ap);
    }
}

template <class R, Kind K>
R call_original_a(int slot, Env* e, jobject l0, jobject l1, jmethodID m, const jvalue* a) {
    if constexpr (K == Kind::kNonvirtual) {
        return reinterpret_cast<R (*)(Env*, jobject, jobject, jmethodID, const jvalue*)>(g.orig[slot])(e, l0, l1, m, a);
    } else {
        return reinterpret_cast<R (*)(Env*, jobject, jmethodID, const jvalue*)>(g.orig[slot])(e, l0, m, a);
    }
}

template <class R>
struct Finisher {
    static R run(Env*, const MethodInfo*, R result, bool) { return result; }
};
template <>
struct Finisher<jobject> {
    static jobject run(Env* e, const MethodInfo* info, jobject result, bool app) {
        return app ? post_result(e, info != nullptr && info->returns_bytes, result) : result;
    }
};

template <class R, class F>
R complete(Env* e, const MethodInfo* info, bool app, F&& call) {
    if constexpr (IsVoid<R>::value) {
        call();
    } else {
        return Finisher<R>::run(e, info, call(), app);
    }
}

// The blocks a call is passed, each once: as the receiver or as an argument.
void add_touched(BlockHeader* header, BlockHeader** touched, unsigned* touched_count) {
    if (header == nullptr) {
        return;
    }
    for (unsigned i = 0; i < *touched_count; ++i) {
        if (touched[i] == header) {
            return;
        }
    }
    touched[(*touched_count)++] = header;
}

// Whether any argument is a shadow; `pack` receives the arguments with every shadow replaced by its real array.
bool collect_va(const MethodInfo& info, va_list ap, jvalue* pack, BlockHeader** touched, unsigned* touched_count) {
    va_list copy;
    va_copy(copy, ap);
    bool changed = false;
    for (unsigned i = 0; i < info.nparams; ++i) {
        pack[i].j = 0;
        switch (kind_at(info, i)) {
            case 'Z': pack[i].z = static_cast<jboolean>(va_arg(copy, int)); break;
            case 'B': pack[i].b = static_cast<jbyte>(va_arg(copy, int)); break;
            case 'C': pack[i].c = static_cast<jchar>(va_arg(copy, int)); break;
            case 'S': pack[i].s = static_cast<jshort>(va_arg(copy, int)); break;
            case 'I': pack[i].i = va_arg(copy, jint); break;
            case 'J': pack[i].j = va_arg(copy, jlong); break;
            case 'F': pack[i].f = static_cast<jfloat>(va_arg(copy, double)); break;
            case 'D': pack[i].d = va_arg(copy, double); break;
            default: {
                jobject object = va_arg(copy, jobject);
                if (is_shadow(object)) {
                    changed = true;
                    add_touched(live_header(object), touched, touched_count);
                }
                pack[i].l = conv(object);
            }
        }
    }
    va_end(copy);
    return changed;
}

bool collect_array(const MethodInfo& info, const jvalue* in, jvalue* pack, BlockHeader** touched,
                   unsigned* touched_count) {
    bool changed = false;
    for (unsigned i = 0; i < info.nparams; ++i) {
        pack[i] = in[i];
        if (kind_at(info, i) == 'L' && is_shadow(in[i].l)) {
            add_touched(live_header(in[i].l), touched, touched_count);
            pack[i].l = conv(in[i].l);
            changed = true;
        }
    }
    return changed;
}

// A call is passed blocks as its arguments and, for an instance or nonvirtual call, as its receiver (clone(), or any
// method without parameters, reads the array through the receiver). The blocks are synchronized around the call.
template <class R, Kind K, int SlotV, int SlotA>
R run_va(Env* e, jobject l0, jobject l1, jmethodID m, va_list ap, bool app) {
    const uint32_t live = live_shadows();
    const MethodInfo* info = ((IsObject<R>::value && app) || live != 0) ? find_method(m) : nullptr;
    if (live != 0) {
        constexpr bool kHasReceiver = K == Kind::kInstance || K == Kind::kNonvirtual;
        jobject receiver = (K == Kind::kStatic || K == Kind::kConstructor) ? l0 : conv(l0);
        if (info == nullptr) {
            info = learn_method(e, m, K == Kind::kStatic, receiver);
        }
        const unsigned params = info != nullptr && info->usable ? info->nparams : 0;
        BlockHeader** touched = static_cast<BlockHeader**>(__builtin_alloca((params + 1) * sizeof(BlockHeader*)));
        unsigned touched_count = 0;
        if (kHasReceiver && is_shadow(l0)) {
            add_touched(live_header(l0), touched, &touched_count);
        }
        if (params > 0) {
            jvalue* pack = static_cast<jvalue*>(__builtin_alloca(params * sizeof(jvalue)));
            if (collect_va(*info, ap, pack, touched, &touched_count)) {
                return complete<R>(e, info, app, [&] {
                    return call_synced<R>(e, touched, touched_count,
                                          [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, pack); });
                });
            }
        }
        if (touched_count != 0) {
            return complete<R>(e, info, app, [&] {
                return call_synced<R>(e, touched, touched_count,
                                      [&] { return call_original_v<R, K>(SlotV, e, receiver, l1, m, ap); });
            });
        }
        return complete<R>(e, info, app, [&] { return call_original_v<R, K>(SlotV, e, receiver, l1, m, ap); });
    }
    return complete<R>(e, info, app, [&] { return call_original_v<R, K>(SlotV, e, l0, l1, m, ap); });
}

template <class R, Kind K, int SlotV, int SlotA>
R run_array(Env* e, jobject l0, jobject l1, jmethodID m, const jvalue* args, bool app) {
    const uint32_t live = live_shadows();
    const MethodInfo* info = ((IsObject<R>::value && app) || live != 0) ? find_method(m) : nullptr;
    if (live != 0) {
        constexpr bool kHasReceiver = K == Kind::kInstance || K == Kind::kNonvirtual;
        jobject receiver = (K == Kind::kStatic || K == Kind::kConstructor) ? l0 : conv(l0);
        if (info == nullptr) {
            info = learn_method(e, m, K == Kind::kStatic, receiver);
        }
        const unsigned params = info != nullptr && info->usable ? info->nparams : 0;
        BlockHeader** touched = static_cast<BlockHeader**>(__builtin_alloca((params + 1) * sizeof(BlockHeader*)));
        unsigned touched_count = 0;
        if (kHasReceiver && is_shadow(l0)) {
            add_touched(live_header(l0), touched, &touched_count);
        }
        if (params > 0) {
            jvalue* pack = static_cast<jvalue*>(__builtin_alloca(params * sizeof(jvalue)));
            if (collect_array(*info, args, pack, touched, &touched_count)) {
                return complete<R>(e, info, app, [&] {
                    return call_synced<R>(e, touched, touched_count,
                                          [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, pack); });
                });
            }
        }
        if (touched_count != 0) {
            return complete<R>(e, info, app, [&] {
                return call_synced<R>(e, touched, touched_count,
                                      [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, args); });
            });
        }
        return complete<R>(e, info, app, [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, args); });
    }
    return complete<R>(e, info, app, [&] { return call_original_a<R, K>(SlotA, e, l0, l1, m, args); });
}

template <class R, int T>
struct Instance {
    static constexpr int kSlotV = call_slot(static_cast<Type>(T), kVa);
    static constexpr int kSlotA = call_slot(static_cast<Type>(T), kArray);
    static R va(Env* e, jobject o, jmethodID m, va_list ap) {
        return run_va<R, Kind::kInstance, kSlotV, kSlotA>(e, o, nullptr, m, ap, enter(e, __builtin_return_address(0)));
    }
    static R array(Env* e, jobject o, jmethodID m, const jvalue* a) {
        return run_array<R, Kind::kInstance, kSlotV, kSlotA>(e, o, nullptr, m, a, enter(e, __builtin_return_address(0)));
    }
    static R variadic(Env* e, jobject o, jmethodID m, ...) {
        const bool app = enter(e, __builtin_return_address(0));
        va_list ap;
        va_start(ap, m);
        if constexpr (IsVoid<R>::value) {
            run_va<R, Kind::kInstance, kSlotV, kSlotA>(e, o, nullptr, m, ap, app);
            va_end(ap);
        } else {
            R result = run_va<R, Kind::kInstance, kSlotV, kSlotA>(e, o, nullptr, m, ap, app);
            va_end(ap);
            return result;
        }
    }
};

template <class R, int T>
struct Nonvirtual {
    static constexpr int kSlotV = nonvirtual_slot(static_cast<Type>(T), kVa);
    static constexpr int kSlotA = nonvirtual_slot(static_cast<Type>(T), kArray);
    static R va(Env* e, jobject o, jclass c, jmethodID m, va_list ap) {
        return run_va<R, Kind::kNonvirtual, kSlotV, kSlotA>(e, o, c, m, ap, enter(e, __builtin_return_address(0)));
    }
    static R array(Env* e, jobject o, jclass c, jmethodID m, const jvalue* a) {
        return run_array<R, Kind::kNonvirtual, kSlotV, kSlotA>(e, o, c, m, a, enter(e, __builtin_return_address(0)));
    }
    static R variadic(Env* e, jobject o, jclass c, jmethodID m, ...) {
        const bool app = enter(e, __builtin_return_address(0));
        va_list ap;
        va_start(ap, m);
        if constexpr (IsVoid<R>::value) {
            run_va<R, Kind::kNonvirtual, kSlotV, kSlotA>(e, o, c, m, ap, app);
            va_end(ap);
        } else {
            R result = run_va<R, Kind::kNonvirtual, kSlotV, kSlotA>(e, o, c, m, ap, app);
            va_end(ap);
            return result;
        }
    }
};

// Static calls and constructors share the shape (jclass, jmethodID, arguments); `SlotVarargs` is the first slot
// of the family (Call...Method).
template <class R, int SlotVarargs, Kind K = Kind::kStatic>
struct Static {
    static constexpr int kSlotV = SlotVarargs + 1;
    static constexpr int kSlotA = SlotVarargs + 2;
    static R va(Env* e, jclass c, jmethodID m, va_list ap) {
        return run_va<R, K, kSlotV, kSlotA>(e, c, nullptr, m, ap, enter(e, __builtin_return_address(0)));
    }
    static R array(Env* e, jclass c, jmethodID m, const jvalue* a) {
        return run_array<R, K, kSlotV, kSlotA>(e, c, nullptr, m, a, enter(e, __builtin_return_address(0)));
    }
    static R variadic(Env* e, jclass c, jmethodID m, ...) {
        const bool app = enter(e, __builtin_return_address(0));
        va_list ap;
        va_start(ap, m);
        if constexpr (IsVoid<R>::value) {
            run_va<R, K, kSlotV, kSlotA>(e, c, nullptr, m, ap, app);
            va_end(ap);
        } else {
            R result = run_va<R, K, kSlotV, kSlotA>(e, c, nullptr, m, ap, app);
            va_end(ap);
            return result;
        }
    }
};

template <class R, int T>
void install_family(void** table) {
    using I = Instance<R, T>;
    using N = Nonvirtual<R, T>;
    using S = Static<R, static_call_slot(static_cast<Type>(T), kVarargs)>;
    table[call_slot(static_cast<Type>(T), kVarargs)] = reinterpret_cast<void*>(&I::variadic);
    table[call_slot(static_cast<Type>(T), kVa)] = reinterpret_cast<void*>(&I::va);
    table[call_slot(static_cast<Type>(T), kArray)] = reinterpret_cast<void*>(&I::array);
    table[nonvirtual_slot(static_cast<Type>(T), kVarargs)] = reinterpret_cast<void*>(&N::variadic);
    table[nonvirtual_slot(static_cast<Type>(T), kVa)] = reinterpret_cast<void*>(&N::va);
    table[nonvirtual_slot(static_cast<Type>(T), kArray)] = reinterpret_cast<void*>(&N::array);
    table[static_call_slot(static_cast<Type>(T), kVarargs)] = reinterpret_cast<void*>(&S::variadic);
    table[static_call_slot(static_cast<Type>(T), kVa)] = reinterpret_cast<void*>(&S::va);
    table[static_call_slot(static_cast<Type>(T), kArray)] = reinterpret_cast<void*>(&S::array);
}

// ---- registration and fields

jmethodID w_GetMethodID(Env* e, jclass c, const char* name, const char* sig) {
    enter_any(e);
    jmethodID id = original<jmethodID (*)(Env*, jclass, const char*, const char*)>(slot::GetMethodID)(e, c, name, sig);
    register_method(id, sig);
    return id;
}
jmethodID w_GetStaticMethodID(Env* e, jclass c, const char* name, const char* sig) {
    enter_any(e);
    jmethodID id =
        original<jmethodID (*)(Env*, jclass, const char*, const char*)>(slot::GetStaticMethodID)(e, c, name, sig);
    register_method(id, sig);
    return id;
}
jfieldID w_GetFieldID(Env* e, jclass c, const char* name, const char* sig) {
    enter_any(e);
    jfieldID id = original<jfieldID (*)(Env*, jclass, const char*, const char*)>(slot::GetFieldID)(e, c, name, sig);
    register_byte_field(id, sig);
    return id;
}
jfieldID w_GetStaticFieldID(Env* e, jclass c, const char* name, const char* sig) {
    enter_any(e);
    jfieldID id =
        original<jfieldID (*)(Env*, jclass, const char*, const char*)>(slot::GetStaticFieldID)(e, c, name, sig);
    register_byte_field(id, sig);
    return id;
}

jobject w_GetObjectField(Env* e, jobject o, jfieldID f) {
    const bool app = enter(e, __builtin_return_address(0));
    jobject result = original<jobject (*)(Env*, jobject, jfieldID)>(get_field_slot(kObject))(e, conv(o), f);
    return app ? post_result(e, is_byte_field(f), result) : result;
}
jobject w_GetStaticObjectField(Env* e, jclass c, jfieldID f) {
    const bool app = enter(e, __builtin_return_address(0));
    jobject result = original<jobject (*)(Env*, jclass, jfieldID)>(get_static_field_slot(kObject))(e, c, f);
    return app ? post_result(e, is_byte_field(f), result) : result;
}
void w_SetObjectField(Env* e, jobject o, jfieldID f, jobject value) {
    enter_any(e);
    original<void (*)(Env*, jobject, jfieldID, jobject)>(set_field_slot(kObject))(e, conv(o), f, conv(value));
}
void w_SetStaticObjectField(Env* e, jclass c, jfieldID f, jobject value) {
    enter_any(e);
    original<void (*)(Env*, jclass, jfieldID, jobject)>(set_static_field_slot(kObject))(e, c, f, conv(value));
}
void w_SetObjectArrayElement(Env* e, jobject array, jsize index, jobject value) {
    enter_any(e);
    original<void (*)(Env*, jobject, jsize, jobject)>(slot::SetObjectArrayElement)(e, array, index, conv(value));
}
jobject w_NewObjectArray(Env* e, jsize length, jclass c, jobject initial) {
    enter_any(e);
    return original<jobject (*)(Env*, jsize, jclass, jobject)>(slot::NewObjectArray)(e, length, c, conv(initial));
}

// ---- references and arrays that may be handed a shadow

jobject w_GetObjectClass(Env* e, jobject o) {
    enter_any(e);
    return original<jobject (*)(Env*, jobject)>(slot::GetObjectClass)(e, conv(o));
}
jboolean w_IsInstanceOf(Env* e, jobject o, jclass c) {
    enter_any(e);
    return original<jboolean (*)(Env*, jobject, jclass)>(slot::IsInstanceOf)(e, conv(o), c);
}
jobject w_NewGlobalRef(Env* e, jobject o) {
    enter_any(e);
    return original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef)(e, conv(o));
}
jobject w_NewWeakGlobalRef(Env* e, jobject o) {
    enter_any(e);
    return original<jobject (*)(Env*, jobject)>(slot::NewWeakGlobalRef)(e, conv(o));
}
jobject w_NewLocalRef(Env* e, jobject o) {
    enter_any(e);
    return original<jobject (*)(Env*, jobject)>(slot::NewLocalRef)(e, conv(o));
}
jboolean w_IsSameObject(Env* e, jobject a, jobject b) {
    enter_any(e);
    return original<jboolean (*)(Env*, jobject, jobject)>(slot::IsSameObject)(e, conv(a), conv(b));
}
jint w_GetObjectRefType(Env* e, jobject o) {
    enter_any(e);
    if (live_header(o) != nullptr) {
        return kRefTypeLocal;
    }
    return original<jint (*)(Env*, jobject)>(slot::GetObjectRefType)(e, conv(o));
}
jint w_MonitorEnter(Env* e, jobject o) {
    enter_any(e);
    return original<jint (*)(Env*, jobject)>(slot::MonitorEnter)(e, conv(o));
}
jint w_MonitorExit(Env* e, jobject o) {
    enter_any(e);
    return original<jint (*)(Env*, jobject)>(slot::MonitorExit)(e, conv(o));
}
// The entry of the block that is the newest: DeleteLocalRef takes one reference of the block, and the frames above
// the entry it leaves are one entry shorter.
void w_DeleteLocalRef(Env* e, jobject o) {
    enter_any(e);
    if (is_shadow(o)) {
        BlockHeader* header = live_header(o);
        ThreadState* state = thread();
        if (header == nullptr || state == nullptr) {
            return;
        }
        for (unsigned i = state->count; i > 0; --i) {
            if (state->blocks[i - 1] == header) {
                const unsigned index = i - 1;
                for (unsigned k = index; k + 1 < state->count; ++k) {
                    state->blocks[k] = state->blocks[k + 1];
                }
                --state->count;
                for (unsigned f = 0; f < state->frame_count; ++f) {
                    if (state->frames[f].height > index) {
                        --state->frames[f].height;
                    }
                }
                release_entry(e, state, header);  // the block goes with its last reference
                return;
            }
        }
        return;  // another thread's block: it goes with its thread
    }
    original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(e, o);
}

// PushLocalFrame opens a frame of the thread's references: PopLocalFrame ends the entries made in it.
jint w_PushLocalFrame(Env* e, jint capacity) {
    enter_any(e);
    const jint result = original<jint (*)(Env*, jint)>(slot::PushLocalFrame)(e, capacity);
    if (result != JNI_OK) {
        return result;
    }
    ThreadState* state = thread();
    if (state == nullptr) {
        return result;
    }
    if (state->frame_count < kMaxFrames) {
        state->frames[state->frame_count++] = FrameRecord{static_cast<uint16_t>(state->count), kFramePush, nullptr, nullptr};
    } else {
        ++state->frame_overflow;
    }
    return result;
}
// A shadow lives in no reference table, so popping the frame cannot take it: the original pop runs with no result
// and the same block is returned, which gets an entry in the outer frame as the real result would. The entries made
// in the frame end with it.
jobject w_PopLocalFrame(Env* e, jobject result) {
    enter_any(e);
    const bool shadow_result = is_shadow(result);
    BlockHeader* keep = shadow_result ? live_header(result) : nullptr;
    jobject returned = original<jobject (*)(Env*, jobject)>(slot::PopLocalFrame)(e, shadow_result ? nullptr : result);
    ThreadState* state = thread();
    if (state != nullptr) {
        if (state->frame_overflow > 0) {
            --state->frame_overflow;  // the pop of a frame no record was made for
        } else if (state->frame_count > 0 && state->frames[state->frame_count - 1].kind == kFramePush) {
            const unsigned height = state->frames[--state->frame_count].height;
            const bool outer_entry = keep != nullptr && height < kMaxBlocks;
            if (outer_entry) {
                ++keep->refs;  // taken before the frame's entries end, so the block survives them
            }
            const unsigned ended = end_entries(state, height);
            if (ended != 0) {
                __atomic_fetch_add(&g.counters.ended_by_pop, ended, __ATOMIC_RELAXED);
            }
            if (outer_entry) {
                state->blocks[state->count++] = keep;
                state->env = e;
            }
        }
    }
    if (shadow_result) {
        return live_header(result) != nullptr ? result : nullptr;
    }
    return returned;
}

// RegisterNatives: the functions of the application's libraries are registered through their thunks.
jint w_RegisterNatives(Env* e, jclass c, const NativeMethod* methods, jint count) {
    enter_any(e);
    auto call = original<jint (*)(Env*, jclass, const NativeMethod*, jint)>(slot::RegisterNatives);
    if (methods == nullptr || count <= 0 || g.platform.thunk_address == nullptr) {
        return call(e, c, methods, count);
    }
    NativeMethod chunk[kRegisterChunk];
    for (jint done = 0; done < count;) {
        const jint n = count - done < static_cast<jint>(kRegisterChunk) ? count - done : static_cast<jint>(kRegisterChunk);
        for (jint i = 0; i < n; ++i) {
            chunk[i] = methods[done + i];
            chunk[i].function = thunk_for(chunk[i].function);
        }
        const jint result = call(e, c, chunk, n);
        if (result != JNI_OK) {
            return result;
        }
        done += n;
    }
    return JNI_OK;
}

jsize w_GetArrayLength(Env* e, jarray a) {
    enter_any(e);
    if (is_shadow(a)) {
        BlockHeader* header = live_header(a);
        return header != nullptr ? static_cast<jsize>(header->capacity) : 0;
    }
    return original<jsize (*)(Env*, jarray)>(slot::GetArrayLength)(e, a);
}
// The pointer of a shadow is a copy, and says so (JNI_TRUE): the block is separate storage that is kept alike at the
// synchronization points, so code that commits before it lets Java read the array through another reference (a global
// reference, a pinned pointer) must be told to. A real reference keeps the runtime's own flag.
jbyte* w_GetByteArrayElements(Env* e, jarray a, jboolean* is_copy) {
    enter_any(e);
    if (is_shadow(a)) {
        if (is_copy != nullptr) {
            *is_copy = 1;
        }
        BlockHeader* header = live_header(a);
        if (header == nullptr) {
            return nullptr;
        }
        sync_all(e, header);
        return reinterpret_cast<jbyte*>(array_of(header)->data);
    }
    return original<jbyte* (*)(Env*, jarray, jboolean*)>(slot::GetByteArrayElements)(e, a, is_copy);
}
// The block is the working copy. A committing release (mode 0 or COMMIT) synchronizes like any other point: the
// chunks the application wrote go to the array and the others keep Java's bytes; JNI_ABORT discards the working
// copy (the block takes the array's bytes). A buffer other than the block (an application that copied it) is the
// application's write into the block first. The critical pair may run inside another array's critical region, where
// only the critical pair of the real array may be called.
void release_block(Env* e, jarray a, const void* elements, jint mode, bool critical) {
    BlockHeader* header = live_header(a);
    if (header == nullptr) {
        return;
    }
    if (mode == kReleaseAbort) {
        if (critical) {
            abort_critical(e, header);
        } else {
            abort_block(e, header);
        }
        return;
    }
    if (elements != nullptr && elements != data_of(header) && header->capacity > 0) {
        __builtin_memcpy(data_of(header), elements, header->capacity);
    }
    if (critical) {
        sync_critical(e, header);
    } else {
        sync_all(e, header);
    }
}
void w_ReleaseByteArrayElements(Env* e, jarray a, jbyte* elements, jint mode) {
    enter_any(e);
    if (is_shadow(a)) {
        release_block(e, a, elements, mode, false);
        return;
    }
    original<void (*)(Env*, jarray, jbyte*, jint)>(slot::ReleaseByteArrayElements)(e, a, elements, mode);
}
// The critical entries make no other JNI call than the real array's critical pair, and do not drain the blocks of
// ended threads (that deletes global references, which a critical region forbids).
void* w_GetPrimitiveArrayCritical(Env* e, jarray a, jboolean* is_copy) {
    if (is_shadow(a)) {
        if (is_copy != nullptr) {
            *is_copy = 1;  // a copy, as for Get...Elements
        }
        BlockHeader* header = live_header(a);
        if (header == nullptr) {
            return nullptr;
        }
        sync_critical(e, header);
        return data_of(header);
    }
    return original<void* (*)(Env*, jarray, jboolean*)>(slot::GetPrimitiveArrayCritical)(e, a, is_copy);
}
void w_ReleasePrimitiveArrayCritical(Env* e, jarray a, void* elements, jint mode) {
    if (is_shadow(a)) {
        release_block(e, a, elements, mode, true);
        return;
    }
    original<void (*)(Env*, jarray, void*, jint)>(slot::ReleasePrimitiveArrayCritical)(e, a, elements, mode);
}
void w_GetByteArrayRegion(Env* e, jarray a, jsize start, jsize length, jbyte* buffer) {
    enter_any(e);
    if (is_shadow(a)) {
        BlockHeader* header = live_header(a);
        if (header == nullptr) {
            return;
        }
        if (start >= 0 && length >= 0 && static_cast<uint64_t>(start) + static_cast<uint64_t>(length) <= header->capacity) {
            sync_bytes(e, header, static_cast<uint32_t>(start), static_cast<uint32_t>(start) + static_cast<uint32_t>(length),
                       true);
            __builtin_memcpy(buffer, data_of(header) + start, static_cast<size_t>(length));
            return;
        }
        // A range the block does not have: the runtime raises its exception for the real array.
        original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(e, header->global, start, length,
                                                                                         buffer);
        return;
    }
    original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(e, a, start, length, buffer);
}
void w_SetByteArrayRegion(Env* e, jarray a, jsize start, jsize length, const jbyte* buffer) {
    enter_any(e);
    if (is_shadow(a)) {
        BlockHeader* header = live_header(a);
        if (header == nullptr) {
            return;
        }
        if (start >= 0 && length >= 0 && static_cast<uint64_t>(start) + static_cast<uint64_t>(length) <= header->capacity) {
            if (length > 0) {
                // What the application wrote through the pointer in these chunks reaches the array first; the range
                // itself is about to be replaced, so nothing is read back.
                sync_bytes(e, header, static_cast<uint32_t>(start), static_cast<uint32_t>(start) + static_cast<uint32_t>(length),
                           false);
                __builtin_memcpy(data_of(header) + start, buffer, static_cast<size_t>(length));
                original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(
                    e, header->global, start, length, buffer);
                __builtin_memcpy(header->baseline + start, buffer, static_cast<size_t>(length));
            }
            return;
        }
        // A range the block does not have: the runtime raises its exception for the real array.
        original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(e, header->global, start,
                                                                                               length, buffer);
        return;
    }
    original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(e, a, start, length, buffer);
}

// ---- the invoke interface: only a "a library may have loaded" notification

void vm_note(const void* return_address) { (void)from_app(return_address); }

jint vm_GetEnv(Vm* vm, void** env, jint version) {
    vm_note(__builtin_return_address(0));
    return reinterpret_cast<jint (*)(Vm*, void**, jint)>(g.orig_vm[vmslot::GetEnv])(vm, env, version);
}
jint vm_AttachCurrentThread(Vm* vm, void** env, void* args) {
    vm_note(__builtin_return_address(0));
    return reinterpret_cast<jint (*)(Vm*, void**, void*)>(g.orig_vm[vmslot::AttachCurrentThread])(vm, env, args);
}
jint vm_AttachCurrentThreadAsDaemon(Vm* vm, void** env, void* args) {
    vm_note(__builtin_return_address(0));
    return reinterpret_cast<jint (*)(Vm*, void**, void*)>(g.orig_vm[vmslot::AttachCurrentThreadAsDaemon])(vm, env, args);
}
// The thread's references end with its attachment (its environment is gone after the call).
jint vm_DetachCurrentThread(Vm* vm) {
    ThreadState* state = g.shadows_on ? thread() : nullptr;
    if (state != nullptr) {
        const unsigned ended = end_entries(state, 0);
        if (ended != 0) {
            __atomic_fetch_add(&g.counters.ended_by_detach, ended, __ATOMIC_RELAXED);
        }
        state->frame_count = 0;
        state->frame_overflow = 0;
        state->env = nullptr;
    }
    return reinterpret_cast<jint (*)(Vm*)>(g.orig_vm[vmslot::DetachCurrentThread])(vm);
}

}  // namespace

bool is_shadow_block(const void* object) { return live_header(object) != nullptr; }

bool is_ended_block(const void* object) {
    if (!is_shadow(object)) {
        return false;
    }
    return header_of(object)->magic == kBlockEnded;
}

void release_thread_state(void* state_pointer) {
    ThreadState* state = static_cast<ThreadState*>(state_pointer);
    if (state == nullptr || !g.shadows_on) {
        return;
    }
    for (unsigned i = 0; i < state->count; ++i) {
        BlockHeader* header = state->blocks[i];
        if (--header->refs > 0) {
            continue;  // the block has more entries: it is queued with its last
        }
        queue_pending(header);
    }
    state->count = 0;
    state->bytes = 0;
    state->frame_count = 0;
    state->frame_overflow = 0;
    state->env = nullptr;
}

Counters counters() { return g.counters; }

// ---- the end of a native method

// The thunk of a native function, or the function itself.
//
// Only a function of the application's own native libraries gets one, decided by the address of the function
// being registered or found (a platform or another package's library, such as WebView, keeps the runtime's
// binding, whoever the caller is). The thunks are a fixed set (the platform's thunk_capacity, 1024): one for
// each distinct function, kept for the life of the process. When they run out, the method is bound with no thunk,
// as when the layer has none: it works as before, and its blocks end by DeleteLocalRef, PopLocalFrame and
// DetachCurrentThread, or count toward the thread's 512 limit (a result beyond it stays the real reference, with its
// log line). One log line says so, once.
void* thunk_for(void* function) {
    if (function == nullptr || !g.installed || !g.shadows_on || g.platform.thunk_address == nullptr ||
        g.thunk_capacity == 0) {
        return function;
    }
    if (g.platform.is_own_library != nullptr ? !g.platform.is_own_library(function) : !from_app(function)) {
        return function;
    }
    while (__atomic_exchange_n(&g.thunk_lock, 1u, __ATOMIC_ACQUIRE) != 0) {
    }
    unsigned index = 0;
    while (index < g.thunk_count && g.thunk_target[index] != function) {
        ++index;
    }
    bool exhausted = false;
    if (index == g.thunk_count) {
        if (g.thunk_count >= g.thunk_capacity) {
            exhausted = true;
        } else {
            __atomic_store_n(&g.thunk_target[index], function, __ATOMIC_RELEASE);
            ++g.thunk_count;
            __atomic_fetch_add(&g.counters.natives_wrapped, 1u, __ATOMIC_RELAXED);
        }
    }
    const unsigned wrapped = g.thunk_count;
    __atomic_store_n(&g.thunk_lock, 0u, __ATOMIC_RELEASE);
    if (exhausted) {
        __atomic_fetch_add(&g.counters.natives_unwrapped, 1u, __ATOMIC_RELAXED);
        if (g.platform.log != nullptr && __atomic_exchange_n(&g.thunk_none_logged, 1u, __ATOMIC_RELAXED) == 0) {
            g.platform.log("an application native method got no thunk (all in use): its blocks end only with their references; "
                           "thunks in use:", static_cast<long>(wrapped), 0);
        }
        return function;
    }
    return g.platform.thunk_address(index);
}

// The diagnosis lines of the thunks: the first return that ended references, and the most one return has ended (a
// native method that makes many blocks in one call is what can reach the thread's limit), at most every kPeakLogMs.
static void note_ended_at_return(unsigned ended) {
    if (g.platform.log == nullptr) {
        return;
    }
    const uint32_t now = g.platform.now_ms != nullptr ? g.platform.now_ms() : 0;
    if (__atomic_exchange_n(&g.ended_logged, 1u, __ATOMIC_RELAXED) == 0) {
        __atomic_store_n(&g.ended_max_logged, ended, __ATOMIC_RELAXED);
        __atomic_store_n(&g.ended_max_logged_ms, now, __ATOMIC_RELAXED);
        g.platform.log("the first byte[] blocks ended at the return of a native method; references ended, native "
                       "methods with a thunk:", static_cast<long>(ended), static_cast<long>(g.thunk_count));
        return;
    }
    uint32_t most = __atomic_load_n(&g.counters.max_ended_by_return, __ATOMIC_RELAXED);
    const uint32_t logged = __atomic_load_n(&g.ended_max_logged, __ATOMIC_RELAXED);
    if (most <= logged ||
        (g.platform.now_ms != nullptr && now - __atomic_load_n(&g.ended_max_logged_ms, __ATOMIC_RELAXED) < kPeakLogMs)) {
        return;
    }
    uint32_t expected = logged;
    if (__atomic_compare_exchange_n(&g.ended_max_logged, &expected, most, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g.ended_max_logged_ms, now, __ATOMIC_RELAXED);
        g.platform.log("the most references one native method call has ended at its return; native methods with a thunk:",
                       static_cast<long>(most), static_cast<long>(g.thunk_count));
    }
}

ThunkEntry thunk_enter(unsigned index, void* return_address, void* sp) {
    void* target = __atomic_load_n(&g.thunk_target[index < kMaxThunks ? index : 0], __ATOMIC_ACQUIRE);
    ThreadState* state = thread();
    if (state == nullptr || state->frame_count >= kMaxFrames) {
        return ThunkEntry{target, 0};  // no record: the method returns to the runtime as it is, its blocks end by themselves
    }
    state->frames[state->frame_count++] =
        FrameRecord{static_cast<uint16_t>(state->count), kFrameNative, return_address, sp};
    return ThunkEntry{target, 1};
}

void* thunk_leave(void* sp) {
    ThreadState* state = thread();
    if (state == nullptr) {
        __builtin_trap();  // the entry made the state: there is no return address to go back to
    }
    // The record of this invocation, the newest one entered with this stack pointer. The records above it are the
    // method's own unmatched PushLocalFrame calls, or invocations that never returned (a longjmp over them).
    unsigned k = state->frame_count;
    while (k > 0 && !(state->frames[k - 1].kind == kFrameNative && state->frames[k - 1].sp == sp)) {
        --k;
    }
    if (k == 0) {
        __builtin_trap();
    }
    const FrameRecord record = state->frames[k - 1];
    state->frame_count = k - 1;
    state->frame_overflow = 0;
    const unsigned ended = end_entries(state, record.height);
    if (ended != 0) {
        __atomic_fetch_add(&g.counters.ended_by_return, ended, __ATOMIC_RELAXED);
        uint32_t most = __atomic_load_n(&g.counters.max_ended_by_return, __ATOMIC_RELAXED);
        while (ended > most && !__atomic_compare_exchange_n(&g.counters.max_ended_by_return, &most, ended, true,
                                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        }
        note_ended_at_return(ended);
    }
    return record.lr;
}

void reset_for_test() { __builtin_memset(&g, 0, sizeof(g)); }

bool install_jni_wrappers(void** table, const Platform& platform, void* shadow_region, size_t shadow_size) {
    if (g.installed) {
        return false;
    }
    g.platform = platform;
    for (int i = 0; i < kSlotCount; ++i) {
        g.orig[i] = table[i];
    }
    g.shadows_on = shadow_region != nullptr && shadow_size >= 4096 && platform.alloc_baseline != nullptr &&
                   platform.free_baseline != nullptr && g.shadow_arena.init(shadow_region, shadow_size);
    if (!g.shadows_on) {
        return true;  // nothing to intercept
    }
    g.ended_head.list_next = &g.ended_head;
    g.ended_head.list_prev = &g.ended_head;
    g.sweep_cursor = &g.ended_head;
    g.shadow_lo = static_cast<const uint8_t*>(shadow_region) + kBlockHead;
    g.shadow_hi = static_cast<const uint8_t*>(shadow_region) + shadow_size;
    if (!platform.make_writable(table, kSlotCount * sizeof(void*))) {
        g.shadows_on = false;
        return false;
    }
    g.installed = true;
    g.thunk_capacity = platform.thunk_address != nullptr
                           ? (platform.thunk_capacity < kMaxThunks ? platform.thunk_capacity : kMaxThunks)
                           : 0;
    table[slot::GetMethodID] = reinterpret_cast<void*>(&w_GetMethodID);
    table[slot::GetStaticMethodID] = reinterpret_cast<void*>(&w_GetStaticMethodID);
    table[slot::GetFieldID] = reinterpret_cast<void*>(&w_GetFieldID);
    table[slot::GetStaticFieldID] = reinterpret_cast<void*>(&w_GetStaticFieldID);
    install_family<jobject, kObject>(table);
    install_family<jboolean, kBoolean>(table);
    install_family<jbyte, kByte>(table);
    install_family<jchar, kChar>(table);
    install_family<jshort, kShort>(table);
    install_family<jint, kInt>(table);
    install_family<jlong, kLong>(table);
    install_family<jfloat, kFloat>(table);
    install_family<jdouble, kDouble>(table);
    install_family<void, kVoid>(table);
    using ObjectConstructor = Static<jobject, slot::NewObject, Kind::kConstructor>;
    table[slot::NewObject] = reinterpret_cast<void*>(&ObjectConstructor::variadic);
    table[slot::NewObjectV] = reinterpret_cast<void*>(&ObjectConstructor::va);
    table[slot::NewObjectA] = reinterpret_cast<void*>(&ObjectConstructor::array);
    table[get_field_slot(kObject)] = reinterpret_cast<void*>(&w_GetObjectField);
    table[get_static_field_slot(kObject)] = reinterpret_cast<void*>(&w_GetStaticObjectField);
    table[set_field_slot(kObject)] = reinterpret_cast<void*>(&w_SetObjectField);
    table[set_static_field_slot(kObject)] = reinterpret_cast<void*>(&w_SetStaticObjectField);
    table[slot::SetObjectArrayElement] = reinterpret_cast<void*>(&w_SetObjectArrayElement);
    table[slot::NewObjectArray] = reinterpret_cast<void*>(&w_NewObjectArray);
    table[slot::GetObjectClass] = reinterpret_cast<void*>(&w_GetObjectClass);
    table[slot::IsInstanceOf] = reinterpret_cast<void*>(&w_IsInstanceOf);
    table[slot::NewGlobalRef] = reinterpret_cast<void*>(&w_NewGlobalRef);
    table[slot::NewWeakGlobalRef] = reinterpret_cast<void*>(&w_NewWeakGlobalRef);
    table[slot::NewLocalRef] = reinterpret_cast<void*>(&w_NewLocalRef);
    table[slot::IsSameObject] = reinterpret_cast<void*>(&w_IsSameObject);
    table[slot::GetObjectRefType] = reinterpret_cast<void*>(&w_GetObjectRefType);
    table[slot::MonitorEnter] = reinterpret_cast<void*>(&w_MonitorEnter);
    table[slot::MonitorExit] = reinterpret_cast<void*>(&w_MonitorExit);
    table[slot::DeleteLocalRef] = reinterpret_cast<void*>(&w_DeleteLocalRef);
    table[slot::PushLocalFrame] = reinterpret_cast<void*>(&w_PushLocalFrame);
    table[slot::PopLocalFrame] = reinterpret_cast<void*>(&w_PopLocalFrame);
    table[slot::RegisterNatives] = reinterpret_cast<void*>(&w_RegisterNatives);
    table[slot::GetArrayLength] = reinterpret_cast<void*>(&w_GetArrayLength);
    table[slot::GetByteArrayElements] = reinterpret_cast<void*>(&w_GetByteArrayElements);
    table[slot::ReleaseByteArrayElements] = reinterpret_cast<void*>(&w_ReleaseByteArrayElements);
    table[slot::GetPrimitiveArrayCritical] = reinterpret_cast<void*>(&w_GetPrimitiveArrayCritical);
    table[slot::ReleasePrimitiveArrayCritical] = reinterpret_cast<void*>(&w_ReleasePrimitiveArrayCritical);
    table[slot::GetByteArrayRegion] = reinterpret_cast<void*>(&w_GetByteArrayRegion);
    table[slot::SetByteArrayRegion] = reinterpret_cast<void*>(&w_SetByteArrayRegion);
    if (!platform.make_read_only(table, kSlotCount * sizeof(void*)) && platform.log != nullptr) {
        platform.log("a page of the JNI function table could not be made read-only again", 0, 0);
    }
    return true;
}

bool install_vm_wrappers(void** vm_table, const Platform& platform) {
    if (g.vm_installed) {
        return false;
    }
    g.platform = platform;
    for (int i = 0; i < kVmSlotCount; ++i) {
        g.orig_vm[i] = vm_table[i];
    }
    if (!platform.make_writable(vm_table, kVmSlotCount * sizeof(void*))) {
        return false;
    }
    g.vm_installed = true;
    vm_table[vmslot::GetEnv] = reinterpret_cast<void*>(&vm_GetEnv);
    vm_table[vmslot::AttachCurrentThread] = reinterpret_cast<void*>(&vm_AttachCurrentThread);
    vm_table[vmslot::AttachCurrentThreadAsDaemon] = reinterpret_cast<void*>(&vm_AttachCurrentThreadAsDaemon);
    vm_table[vmslot::DetachCurrentThread] = reinterpret_cast<void*>(&vm_DetachCurrentThread);
    if (!platform.make_read_only(vm_table, kVmSlotCount * sizeof(void*)) && platform.log != nullptr) {
        platform.log("a page of the JavaVM function table could not be made read-only again", 0, 0);
    }
    return true;
}

}  // namespace oldapp

extern "C" uint64_t oldapp_thunk_enter(unsigned index, void* return_address, void* sp) {
    const oldapp::ThunkEntry entry = oldapp::thunk_enter(index, return_address, sp);
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry.target)) | (static_cast<uint64_t>(entry.pushed) << 32);
}

extern "C" void* oldapp_thunk_leave(void* sp) { return oldapp::thunk_leave(sp); }

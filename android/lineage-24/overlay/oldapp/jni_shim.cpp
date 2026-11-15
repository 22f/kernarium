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
constexpr unsigned kMaxBlocks = 512;  // Dalvik's limit on local references; a thread has no more live shadows
constexpr uint32_t kDalvikArrayClass = 0x5a324241u;
constexpr uint32_t kBlockLive = 0xb10c11feu;
constexpr uint32_t kBlockDead = 0xdeadb10cu;
constexpr size_t kBlockTail = 16;  // zeroes behind the data: raw readers overrun by a few bytes

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
    uint32_t capacity;
    jobject global;      // a global reference to the real array
    BlockHeader* next;   // the queue of an ended thread's blocks
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

struct ThreadState {
    BlockHeader* blocks[kMaxBlocks];
    unsigned count;
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
    uint32_t lock;
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

void free_block(Env* env, BlockHeader* header) {
    header->magic = kBlockDead;
    original<void (*)(Env*, jobject)>(slot::DeleteGlobalRef)(env, header->global);
    g.shadow_arena.release(header);
    __atomic_fetch_sub(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
}

// The blocks of ended threads: their global references need an environment to be deleted.
void drain_pending(Env* env) {
    BlockHeader* list = __atomic_exchange_n(&g.pending, static_cast<BlockHeader*>(nullptr), __ATOMIC_ACQ_REL);
    while (list != nullptr) {
        BlockHeader* next = list->next;
        free_block(env, list);
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

// A shadow for the real local reference `local`, which it replaces: the block holds a global reference to the
// array, the local reference is deleted. Null when the thread already has the limit or the region is full: the
// caller keeps the real reference, as it would without the layer.
jobject make_shadow(Env* env, jobject local) {
    ThreadState* state = thread();
    if (state == nullptr || state->count >= kMaxBlocks) {
        ++g.counters.shadows_declined;
        return nullptr;
    }
    const jsize length = original<jsize (*)(Env*, jarray)>(slot::GetArrayLength)(env, local);
    if (length < 0) {
        return nullptr;
    }
    const size_t bytes = static_cast<size_t>(length);
    const size_t need = kBlockHead + offsetof(DalvikArray, data) + bytes + kBlockTail;
    uint8_t* raw = static_cast<uint8_t*>(g.shadow_arena.allocate(need));
    if (raw == nullptr) {
        ++g.counters.shadows_declined;
        return nullptr;
    }
    jobject global = original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef)(env, local);
    if (global == nullptr) {
        g.shadow_arena.release(raw);
        ++g.counters.shadows_declined;
        return nullptr;
    }
    BlockHeader* header = reinterpret_cast<BlockHeader*>(raw);
    header->magic = kBlockLive;
    header->capacity = static_cast<uint32_t>(bytes);
    header->global = global;
    header->next = nullptr;
    DalvikArray* block = array_of(header);
    block->clazz = kDalvikArrayClass;
    block->lock = 0;
    block->length = static_cast<uint32_t>(bytes);
    block->pad = 0;
    if (bytes > 0) {
        original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
            env, local, 0, length, reinterpret_cast<jbyte*>(block->data));
    }
    __builtin_memset(block->data + bytes, 0, kBlockTail);
    state->blocks[state->count++] = header;
    original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, local);
    __atomic_fetch_add(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
    ++g.counters.shadows_created;
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

// The array's bytes go to the real array: what a conformant Release or SetRegion does with a copy.
void write_back(Env* env, BlockHeader* header, jsize start, jsize length, const void* source) {
    original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(
        env, header->global, start, length, static_cast<const jbyte*>(source));
}

// The block follows the real array. Java writes into the array behind the block (a call it was passed to, another
// thread, another library), and the block, which an application reads through the raw pointer, would not show it.
// So the block is refreshed from the array where the application asks for the array's bytes (Get...Elements,
// Get...Critical, GetByteArrayRegion) and after a call that was passed the block. The converse holds only at a
// release: a write through the raw pointer reaches the array at SetByteArrayRegion or Release (mode 0 or COMMIT), and a
// refresh replaces a raw write that has not been released.
void refresh_block(Env* env, BlockHeader* header) {
    DalvikArray* block = array_of(header);
    if (block->length > 0) {
        original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
            env, header->global, 0, static_cast<jsize>(block->length), reinterpret_cast<jbyte*>(block->data));
    }
}

// The same through the real array's critical pair, the only JNI calls that CheckJNI allows inside a critical
// region (the application may be in one for another array when it asks for this block's critical pointer).
void refresh_block_critical(Env* env, BlockHeader* header) {
    DalvikArray* block = array_of(header);
    if (block->length == 0) {
        return;
    }
    void* pinned = original<void* (*)(Env*, jarray, jboolean*)>(slot::GetPrimitiveArrayCritical)(env, header->global,
                                                                                               nullptr);
    if (pinned != nullptr) {
        __builtin_memcpy(block->data, pinned, block->length);
        original<void (*)(Env*, jarray, void*, jint)>(slot::ReleasePrimitiveArrayCritical)(env, header->global, pinned,
                                                                                          kReleaseAbort);
    }
}

void write_back_critical(Env* env, BlockHeader* header) {
    DalvikArray* block = array_of(header);
    if (block->length == 0) {
        return;
    }
    void* pinned = original<void* (*)(Env*, jarray, jboolean*)>(slot::GetPrimitiveArrayCritical)(env, header->global,
                                                                                               nullptr);
    if (pinned != nullptr) {
        __builtin_memcpy(pinned, block->data, block->length);
        original<void (*)(Env*, jarray, void*, jint)>(slot::ReleasePrimitiveArrayCritical)(env, header->global, pinned, 0);
    }
}

// After a call that was passed these blocks. A call that raised an exception leaves the blocks as they were: almost
// no JNI function may run with an exception pending.
void refresh_touched(Env* env, BlockHeader* const* touched, unsigned count) {
    if (count == 0 || original<jboolean (*)(Env*)>(slot::ExceptionCheck)(env)) {
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (touched[i]->magic == kBlockLive) {
            refresh_block(env, touched[i]);
        }
    }
}

template <class R, class F>
R call_then_refresh(Env* env, BlockHeader* const* touched, unsigned count, F&& call) {
    if constexpr (IsVoid<R>::value) {
        call();
        refresh_touched(env, touched, count);
    } else {
        R result = call();
        refresh_touched(env, touched, count);
        return result;
    }
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
                    BlockHeader* header = live_header(object);
                    if (header != nullptr) {
                        touched[(*touched_count)++] = header;
                    }
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
            BlockHeader* header = live_header(in[i].l);
            if (header != nullptr) {
                touched[(*touched_count)++] = header;
            }
            pack[i].l = conv(in[i].l);
            changed = true;
        }
    }
    return changed;
}

template <class R, Kind K, int SlotV, int SlotA>
R run_va(Env* e, jobject l0, jobject l1, jmethodID m, va_list ap, bool app) {
    const uint32_t live = live_shadows();
    const MethodInfo* info = ((IsObject<R>::value && app) || live != 0) ? find_method(m) : nullptr;
    if (live != 0) {
        jobject receiver = (K == Kind::kStatic || K == Kind::kConstructor) ? l0 : conv(l0);
        if (info == nullptr) {
            info = learn_method(e, m, K == Kind::kStatic, receiver);
        }
        if (info != nullptr && info->usable && info->nparams > 0) {
            jvalue* pack = static_cast<jvalue*>(__builtin_alloca(info->nparams * sizeof(jvalue)));
            BlockHeader** touched = static_cast<BlockHeader**>(__builtin_alloca(info->nparams * sizeof(BlockHeader*)));
            unsigned touched_count = 0;
            if (collect_va(*info, ap, pack, touched, &touched_count)) {
                return complete<R>(e, info, app, [&] {
                    return call_then_refresh<R>(e, touched, touched_count,
                                                [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, pack); });
                });
            }
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
        jobject receiver = (K == Kind::kStatic || K == Kind::kConstructor) ? l0 : conv(l0);
        if (info == nullptr) {
            info = learn_method(e, m, K == Kind::kStatic, receiver);
        }
        if (info != nullptr && info->usable && info->nparams > 0) {
            jvalue* pack = static_cast<jvalue*>(__builtin_alloca(info->nparams * sizeof(jvalue)));
            BlockHeader** touched = static_cast<BlockHeader**>(__builtin_alloca(info->nparams * sizeof(BlockHeader*)));
            unsigned touched_count = 0;
            if (collect_array(*info, args, pack, touched, &touched_count)) {
                return complete<R>(e, info, app, [&] {
                    return call_then_refresh<R>(e, touched, touched_count,
                                                [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, pack); });
                });
            }
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
                state->blocks[i - 1] = state->blocks[--state->count];
                free_block(e, header);
                return;
            }
        }
        return;  // another thread's block: it goes with its thread
    }
    original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(e, o);
}
// A shadow lives in no reference table, so popping the frame cannot take it: the original pop runs with no
// result and the same block is returned.
jobject w_PopLocalFrame(Env* e, jobject result) {
    enter_any(e);
    if (is_shadow(result)) {
        original<jobject (*)(Env*, jobject)>(slot::PopLocalFrame)(e, nullptr);
        return live_header(result) != nullptr ? result : nullptr;
    }
    return original<jobject (*)(Env*, jobject)>(slot::PopLocalFrame)(e, result);
}

jsize w_GetArrayLength(Env* e, jarray a) {
    enter_any(e);
    if (is_shadow(a)) {
        BlockHeader* header = live_header(a);
        return header != nullptr ? static_cast<jsize>(static_cast<const DalvikArray*>(a)->length) : 0;
    }
    return original<jsize (*)(Env*, jarray)>(slot::GetArrayLength)(e, a);
}
jbyte* w_GetByteArrayElements(Env* e, jarray a, jboolean* is_copy) {
    enter_any(e);
    if (is_shadow(a)) {
        if (is_copy != nullptr) {
            *is_copy = 0;
        }
        BlockHeader* header = live_header(a);
        if (header == nullptr) {
            return nullptr;
        }
        refresh_block(e, header);
        return reinterpret_cast<jbyte*>(array_of(header)->data);
    }
    return original<jbyte* (*)(Env*, jarray, jboolean*)>(slot::GetByteArrayElements)(e, a, is_copy);
}
// The block is the working copy: the array gets its bytes on Release (unless JNI_ABORT, which gives the block the
// array's bytes back) and on a commit. The critical pair may run inside another array's critical region, where only
// the critical pair of the real array may be called.
void release_block(Env* e, jarray a, const void* elements, jint mode, bool critical) {
    BlockHeader* header = live_header(a);
    if (header == nullptr) {
        return;
    }
    if (mode == kReleaseAbort) {
        if (critical) {
            refresh_block_critical(e, header);
        } else {
            refresh_block(e, header);
        }
        return;
    }
    DalvikArray* block = array_of(header);
    const void* source = elements != nullptr ? elements : block->data;
    if (source != block->data) {
        __builtin_memcpy(block->data, source, block->length);
    }
    if (block->length > 0) {
        if (critical) {
            write_back_critical(e, header);
        } else {
            write_back(e, header, 0, static_cast<jsize>(block->length), block->data);
        }
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
            *is_copy = 0;
        }
        BlockHeader* header = live_header(a);
        if (header == nullptr) {
            return nullptr;
        }
        refresh_block_critical(e, header);
        return array_of(header)->data;
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
        DalvikArray* block = array_of(header);
        if (start >= 0 && length >= 0 && static_cast<uint32_t>(start) + static_cast<uint32_t>(length) <= block->length) {
            // The range comes from the real array (Java may have written it), into the block and the caller's buffer.
            if (length > 0) {
                original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
                    e, header->global, start, length, reinterpret_cast<jbyte*>(block->data + start));
                __builtin_memcpy(buffer, block->data + start, static_cast<size_t>(length));
            }
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
        DalvikArray* block = array_of(header);
        if (start >= 0 && length >= 0 && static_cast<uint32_t>(start) + static_cast<uint32_t>(length) <= block->length) {
            __builtin_memcpy(block->data + start, buffer, static_cast<size_t>(length));
        }
        write_back(e, header, start, length, buffer);  // the real array takes the write, or raises the range error
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

}  // namespace

bool is_shadow_block(const void* object) { return live_header(object) != nullptr; }

void release_thread_state(void* state_pointer) {
    ThreadState* state = static_cast<ThreadState*>(state_pointer);
    if (state == nullptr || !g.shadows_on) {
        return;
    }
    for (unsigned i = 0; i < state->count; ++i) {
        BlockHeader* header = state->blocks[i];
        BlockHeader* head = __atomic_load_n(&g.pending, __ATOMIC_RELAXED);
        do {
            header->next = head;
        } while (!__atomic_compare_exchange_n(&g.pending, &head, header, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    }
    state->count = 0;
}

Counters counters() { return g.counters; }

void reset_for_test() { __builtin_memset(&g, 0, sizeof(g)); }

bool install_jni_wrappers(void** table, const Platform& platform, void* shadow_region, size_t shadow_size) {
    if (g.installed) {
        return false;
    }
    g.platform = platform;
    for (int i = 0; i < kSlotCount; ++i) {
        g.orig[i] = table[i];
    }
    g.shadows_on = shadow_region != nullptr && shadow_size >= 4096 && g.shadow_arena.init(shadow_region, shadow_size);
    if (!g.shadows_on) {
        return true;  // nothing to intercept
    }
    g.shadow_lo = static_cast<const uint8_t*>(shadow_region) + kBlockHead;
    g.shadow_hi = static_cast<const uint8_t*>(shadow_region) + shadow_size;
    if (!platform.make_writable(table, kSlotCount * sizeof(void*))) {
        g.shadows_on = false;
        return false;
    }
    g.installed = true;
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
    table[slot::PopLocalFrame] = reinterpret_cast<void*>(&w_PopLocalFrame);
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
    if (!platform.make_read_only(vm_table, kVmSlotCount * sizeof(void*)) && platform.log != nullptr) {
        platform.log("a page of the JavaVM function table could not be made read-only again", 0, 0);
    }
    return true;
}

}  // namespace oldapp

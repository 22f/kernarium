// SPDX-License-Identifier: Apache-2.0

#include "jni_shim.hpp"

#include <stdarg.h>

#include "low_arena.hpp"

namespace oldapp {
namespace {

using namespace jni;

constexpr int kMaxParams = 16;
constexpr uint32_t kMethodSlots = 8192;
constexpr uint32_t kFieldSlots = 2048;
constexpr unsigned kProbeLimit = 32;
constexpr unsigned kMaxBlocks = 64;
constexpr size_t kMaxBlockBytes = 16u << 20;
constexpr unsigned kRingSize = 256;
constexpr unsigned kProbeEvery = 64;
constexpr jint kProbeRoom = 80;
constexpr unsigned kKeepNewest = 64;
constexpr uint32_t kDalvikArrayClass = 0x5a324241u;
constexpr uint32_t kBlockLive = 0xb10c11feu;
constexpr uint32_t kBlockDead = 0xdeadb10cu;
constexpr size_t kBlockHead = 16;   // the layer's own header, in front of the Dalvik object
constexpr size_t kBlockTail = 16;   // zeroes behind the data: raw readers overrun by a few bytes

template <class T>
struct IsVoid {
    static constexpr bool value = false;
};
template <>
struct IsVoid<void> {
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

struct BlockHeader {
    uint32_t magic;
    uint32_t capacity;
    uint32_t reserved[2];
};
static_assert(sizeof(BlockHeader) == kBlockHead, "block header");

struct MethodInfo {
    void* id;
    uint8_t nparams;
    uint8_t usable;  // false: more parameters than the layer unpacks
    uint8_t returns_bytes;
    char kinds[kMaxParams];
};

struct ThreadState {
    DalvikArray* blocks[kMaxBlocks];
    unsigned block_count;
    size_t block_bytes;
    jobject ring[kRingSize];
    unsigned ring_head;
    unsigned ring_count;
    unsigned since_probe;
};

struct Globals {
    Platform platform;
    void* orig[kSlotCount];
    void* orig_vm[kVmSlotCount];
    bool installed;
    bool vm_installed;
    bool shadows_on;
    bool relief_on;
    Arena shadow_arena;
    const uint8_t* shadow_lo;
    const uint8_t* shadow_hi;
    uint32_t lock;
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

// ---- signatures

unsigned hash_of(const void* id, unsigned modulus) {
    return static_cast<unsigned>((static_cast<uint32_t>(reinterpret_cast<uintptr_t>(id) >> 2) * 2654435761u) %
                                 modulus);
}

bool parse_signature(const char* sig, MethodInfo* info) {
    if (sig == nullptr || sig[0] != '(') {
        return false;
    }
    const char* p = sig + 1;
    unsigned count = 0;
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
        if (count < kMaxParams) {
            info->kinds[count] = kind;
        }
        ++count;
    }
    if (*p != ')') {
        return false;
    }
    ++p;
    info->nparams = static_cast<uint8_t>(count > kMaxParams ? kMaxParams : count);
    info->usable = count <= kMaxParams;
    info->returns_bytes = p[0] == '[' && p[1] == 'B' && p[2] == '\0';
    return true;
}

void register_method(void* id, const char* sig) {
    if (id == nullptr) {
        return;
    }
    MethodInfo parsed = {};
    if (!parse_signature(sig, &parsed)) {
        return;
    }
    lock_registry();
    unsigned index = hash_of(id, kMethodSlots);
    for (unsigned probe = 0; probe < kProbeLimit; ++probe, index = (index + 1) % kMethodSlots) {
        MethodInfo& entry = g.methods[index];
        if (entry.id == id) {
            break;  // already known
        }
        if (entry.id == nullptr) {
            entry = parsed;
            entry.id = nullptr;
            __atomic_store_n(&entry.id, id, __ATOMIC_RELEASE);
            break;
        }
    }
    unlock_registry();
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

// ---- shadow blocks

BlockHeader* header_of(DalvikArray* block) {
    return reinterpret_cast<BlockHeader*>(reinterpret_cast<uint8_t*>(block) - kBlockHead);
}

void free_block(DalvikArray* block) {
    BlockHeader* header = header_of(block);
    header->magic = kBlockDead;
    g.shadow_arena.release(header);
    __atomic_fetch_sub(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
}

void evict_oldest(ThreadState* state) {
    DalvikArray* block = state->blocks[0];
    state->block_bytes -= block->length;
    for (unsigned i = 1; i < state->block_count; ++i) {
        state->blocks[i - 1] = state->blocks[i];
    }
    --state->block_count;
    free_block(block);
}

void forget_all_blocks(ThreadState* state) {
    while (state->block_count > 0) {
        evict_oldest(state);
    }
}

jobject make_shadow(Env* env, jobject local) {
    const jsize length = original<jsize (*)(Env*, jarray)>(slot::GetArrayLength)(env, local);
    ThreadState* state = thread();
    if (length < 0 || state == nullptr) {
        return nullptr;
    }
    const size_t bytes = static_cast<size_t>(length);
    while (state->block_count > 0 && (state->block_count >= kMaxBlocks || state->block_bytes + bytes > kMaxBlockBytes)) {
        evict_oldest(state);
    }
    const size_t need = kBlockHead + offsetof(DalvikArray, data) + bytes + kBlockTail;
    uint8_t* raw = static_cast<uint8_t*>(g.shadow_arena.allocate(need));
    if (raw == nullptr) {
        forget_all_blocks(state);
        raw = static_cast<uint8_t*>(g.shadow_arena.allocate(need));
        if (raw == nullptr) {
            return nullptr;
        }
    }
    BlockHeader* header = reinterpret_cast<BlockHeader*>(raw);
    header->magic = kBlockLive;
    header->capacity = static_cast<uint32_t>(bytes);
    DalvikArray* block = reinterpret_cast<DalvikArray*>(raw + kBlockHead);
    block->clazz = kDalvikArrayClass;
    block->lock = 0;
    block->length = static_cast<uint32_t>(bytes);
    block->pad = 0;
    if (bytes > 0) {
        original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
            env, local, 0, length, reinterpret_cast<jbyte*>(block->data));
    }
    __builtin_memset(block->data + bytes, 0, kBlockTail);
    state->blocks[state->block_count++] = block;
    state->block_bytes += bytes;
    original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, local);
    __atomic_fetch_add(&g.counters.shadows_live, 1u, __ATOMIC_RELAXED);
    ++g.counters.shadows_created;
    return block;
}

// A real Java array holding the block's bytes: what the runtime is handed in place of a shadow.
jobject unwrap_to_array(Env* env, jobject shadow) {
    const DalvikArray* block = static_cast<const DalvikArray*>(shadow);
    const jsize length = static_cast<jsize>(block->length);
    jobject array = original<jobject (*)(Env*, jsize)>(slot::NewByteArray)(env, length);
    if (array != nullptr && length > 0) {
        original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(
            env, array, 0, length, reinterpret_cast<const jbyte*>(block->data));
    }
    ++g.counters.shadows_unwrapped;
    return array;
}

// The temporary Java arrays a call made for its shadow arguments, released when the call is done.
struct Temps {
    explicit Temps(Env* environment) : env(environment) {}
    Temps(const Temps&) = delete;
    Temps& operator=(const Temps&) = delete;
    ~Temps() {
        for (unsigned i = 0; i < count; ++i) {
            original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(env, refs[i]);
        }
    }
    jobject take(jobject object) {
        if (!is_shadow(object)) {
            return object;
        }
        jobject array = unwrap_to_array(env, object);
        if (array != nullptr && count < kMaxParams + 2) {
            refs[count++] = array;
        }
        return array;
    }
    Env* env;
    jobject refs[kMaxParams + 2];
    unsigned count = 0;
};

// ---- local reference relief

void relieve(Env* env, ThreadState* state) {
    ++g.counters.probes;
    if (original<jboolean (*)(Env*)>(slot::ExceptionCheck)(env) != 0) {
        return;  // never disturb a pending exception
    }
    if (original<jint (*)(Env*, jint)>(slot::EnsureLocalCapacity)(env, kProbeRoom) == JNI_OK) {
        return;
    }
    original<void (*)(Env*)>(slot::ExceptionClear)(env);  // the OutOfMemoryError the refusal raised
    ++g.counters.relief_rounds;
    auto release = original<void (*)(Env*, jobject)>(slot::DeleteLocalRef);
    while (state->ring_count > kKeepNewest) {
        jobject oldest = state->ring[state->ring_head];
        state->ring_head = (state->ring_head + 1) % kRingSize;
        --state->ring_count;
        if (oldest != nullptr) {
            release(env, oldest);
            ++g.counters.references_released;
        }
    }
}

void track(Env* env, jobject reference) {
    if (!g.relief_on || reference == nullptr) {
        return;
    }
    ThreadState* state = thread();
    if (state == nullptr) {
        return;
    }
    state->ring[(state->ring_head + state->ring_count) % kRingSize] = reference;
    if (state->ring_count == kRingSize) {
        state->ring_head = (state->ring_head + 1) % kRingSize;
    } else {
        ++state->ring_count;
    }
    if (++state->since_probe >= kProbeEvery) {
        state->since_probe = 0;
        relieve(env, state);
    }
}

void untrack(jobject reference) {
    ThreadState* state = thread();
    if (state == nullptr) {
        return;
    }
    for (unsigned k = state->ring_count; k > 0; --k) {
        const unsigned index = (state->ring_head + k - 1) % kRingSize;
        if (state->ring[index] == reference) {
            state->ring[index] = nullptr;
            return;
        }
    }
}

// What an application gets for a reference-returning call: a shadow when the declared type is byte[],
// otherwise the reference, noted for relief.
jobject post_result(Env* env, bool bytes, jobject reference) {
    if (reference == nullptr) {
        return reference;
    }
    if (bytes && g.shadows_on) {
        jobject shadow = make_shadow(env, reference);
        if (shadow != nullptr) {
            return shadow;
        }
    }
    track(env, reference);
    return reference;
}

// ---- the Call families

enum class Kind { kInstance, kNonvirtual, kStatic };

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

bool collect_va(const MethodInfo& info, va_list ap, jvalue* pack, Temps& temps) {
    va_list copy;
    va_copy(copy, ap);
    bool changed = false;
    for (unsigned i = 0; i < info.nparams; ++i) {
        pack[i].j = 0;
        switch (info.kinds[i]) {
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
                    pack[i].l = temps.take(object);
                    changed = true;
                } else {
                    pack[i].l = object;
                }
            }
        }
    }
    va_end(copy);
    return changed;
}

bool collect_array(const MethodInfo& info, const jvalue* in, jvalue* pack, Temps& temps) {
    bool changed = false;
    for (unsigned i = 0; i < info.nparams; ++i) {
        pack[i] = in[i];
        if (info.kinds[i] == 'L' && is_shadow(in[i].l)) {
            pack[i].l = temps.take(in[i].l);
            changed = true;
        }
    }
    return changed;
}

template <class R, Kind K, int SlotV, int SlotA>
R run_va(Env* e, jobject l0, jobject l1, jmethodID m, va_list ap, bool app) {
    const MethodInfo* info = app ? find_method(m) : nullptr;
    if (app && live_shadows() != 0) {
        Temps temps(e);
        jobject receiver = K == Kind::kStatic ? l0 : temps.take(l0);
        jvalue pack[kMaxParams];
        if (info != nullptr && info->usable && collect_va(*info, ap, pack, temps)) {
            return complete<R>(e, info, app, [&] { return call_original_a<R, K>(SlotA, e, receiver, l1, m, pack); });
        }
        return complete<R>(e, info, app, [&] { return call_original_v<R, K>(SlotV, e, receiver, l1, m, ap); });
    }
    return complete<R>(e, info, app, [&] { return call_original_v<R, K>(SlotV, e, l0, l1, m, ap); });
}

template <class R, Kind K, int SlotV, int SlotA>
R run_array(Env* e, jobject l0, jobject l1, jmethodID m, const jvalue* args, bool app) {
    const MethodInfo* info = app ? find_method(m) : nullptr;
    if (app && live_shadows() != 0) {
        Temps temps(e);
        jobject receiver = K == Kind::kStatic ? l0 : temps.take(l0);
        jvalue pack[kMaxParams];
        if (info != nullptr && info->usable && collect_array(*info, args, pack, temps)) {
            args = pack;
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
        return run_va<R, Kind::kInstance, kSlotV, kSlotA>(e, o, nullptr, m, ap, from_app(__builtin_return_address(0)));
    }
    static R array(Env* e, jobject o, jmethodID m, const jvalue* a) {
        return run_array<R, Kind::kInstance, kSlotV, kSlotA>(e, o, nullptr, m, a, from_app(__builtin_return_address(0)));
    }
    static R variadic(Env* e, jobject o, jmethodID m, ...) {
        const bool app = from_app(__builtin_return_address(0));
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
        return run_va<R, Kind::kNonvirtual, kSlotV, kSlotA>(e, o, c, m, ap, from_app(__builtin_return_address(0)));
    }
    static R array(Env* e, jobject o, jclass c, jmethodID m, const jvalue* a) {
        return run_array<R, Kind::kNonvirtual, kSlotV, kSlotA>(e, o, c, m, a, from_app(__builtin_return_address(0)));
    }
    static R variadic(Env* e, jobject o, jclass c, jmethodID m, ...) {
        const bool app = from_app(__builtin_return_address(0));
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

// Static calls and constructors share the shape (jclass, jmethodID, arguments); `Base` is the first slot
// of the family (Call...Method), `Index` the member of the family.
template <class R, int SlotVarargs>
struct Static {
    static constexpr int kSlotV = SlotVarargs + 1;
    static constexpr int kSlotA = SlotVarargs + 2;
    static R va(Env* e, jclass c, jmethodID m, va_list ap) {
        return run_va<R, Kind::kStatic, kSlotV, kSlotA>(e, c, nullptr, m, ap, from_app(__builtin_return_address(0)));
    }
    static R array(Env* e, jclass c, jmethodID m, const jvalue* a) {
        return run_array<R, Kind::kStatic, kSlotV, kSlotA>(e, c, nullptr, m, a, from_app(__builtin_return_address(0)));
    }
    static R variadic(Env* e, jclass c, jmethodID m, ...) {
        const bool app = from_app(__builtin_return_address(0));
        va_list ap;
        va_start(ap, m);
        if constexpr (IsVoid<R>::value) {
            run_va<R, Kind::kStatic, kSlotV, kSlotA>(e, c, nullptr, m, ap, app);
            va_end(ap);
        } else {
            R result = run_va<R, Kind::kStatic, kSlotV, kSlotA>(e, c, nullptr, m, ap, app);
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

// ---- registration, fields, strings, classes

jmethodID w_GetMethodID(Env* e, jclass c, const char* name, const char* sig) {
    jmethodID id = original<jmethodID (*)(Env*, jclass, const char*, const char*)>(slot::GetMethodID)(e, c, name, sig);
    register_method(id, sig);
    return id;
}
jmethodID w_GetStaticMethodID(Env* e, jclass c, const char* name, const char* sig) {
    jmethodID id =
        original<jmethodID (*)(Env*, jclass, const char*, const char*)>(slot::GetStaticMethodID)(e, c, name, sig);
    register_method(id, sig);
    return id;
}
jfieldID w_GetFieldID(Env* e, jclass c, const char* name, const char* sig) {
    jfieldID id = original<jfieldID (*)(Env*, jclass, const char*, const char*)>(slot::GetFieldID)(e, c, name, sig);
    register_byte_field(id, sig);
    return id;
}
jfieldID w_GetStaticFieldID(Env* e, jclass c, const char* name, const char* sig) {
    jfieldID id =
        original<jfieldID (*)(Env*, jclass, const char*, const char*)>(slot::GetStaticFieldID)(e, c, name, sig);
    register_byte_field(id, sig);
    return id;
}

jobject w_GetObjectField(Env* e, jobject o, jfieldID f) {
    const bool app = from_app(__builtin_return_address(0));
    Temps temps(e);
    jobject receiver = app ? temps.take(o) : o;
    jobject result = original<jobject (*)(Env*, jobject, jfieldID)>(get_field_slot(kObject))(e, receiver, f);
    return app ? post_result(e, is_byte_field(f), result) : result;
}
jobject w_GetStaticObjectField(Env* e, jclass c, jfieldID f) {
    const bool app = from_app(__builtin_return_address(0));
    jobject result = original<jobject (*)(Env*, jclass, jfieldID)>(get_static_field_slot(kObject))(e, c, f);
    return app ? post_result(e, is_byte_field(f), result) : result;
}
void w_SetObjectField(Env* e, jobject o, jfieldID f, jobject value) {
    Temps temps(e);
    original<void (*)(Env*, jobject, jfieldID, jobject)>(set_field_slot(kObject))(
        e, from_app(__builtin_return_address(0)) ? temps.take(o) : o, f, temps.take(value));
}
void w_SetStaticObjectField(Env* e, jclass c, jfieldID f, jobject value) {
    Temps temps(e);
    original<void (*)(Env*, jclass, jfieldID, jobject)>(set_static_field_slot(kObject))(e, c, f, temps.take(value));
}
jobject w_GetObjectArrayElement(Env* e, jobject array, jsize index) {
    const bool app = from_app(__builtin_return_address(0));
    jobject result = original<jobject (*)(Env*, jobject, jsize)>(slot::GetObjectArrayElement)(e, array, index);
    if (app) {
        track(e, result);
    }
    return result;
}
void w_SetObjectArrayElement(Env* e, jobject array, jsize index, jobject value) {
    Temps temps(e);
    original<void (*)(Env*, jobject, jsize, jobject)>(slot::SetObjectArrayElement)(e, array, index, temps.take(value));
}

jobject track_result(Env* e, bool app, jobject result) {
    if (app) {
        track(e, result);
    }
    return result;
}
jobject w_NewStringUTF(Env* e, const char* bytes) {
    const bool app = from_app(__builtin_return_address(0));
    return track_result(e, app, original<jobject (*)(Env*, const char*)>(slot::NewStringUTF)(e, bytes));
}
jobject w_NewString(Env* e, const jchar* chars, jsize length) {
    const bool app = from_app(__builtin_return_address(0));
    return track_result(e, app, original<jobject (*)(Env*, const jchar*, jsize)>(slot::NewString)(e, chars, length));
}
jobject w_FindClass(Env* e, const char* name) {
    const bool app = from_app(__builtin_return_address(0));
    return track_result(e, app, original<jobject (*)(Env*, const char*)>(slot::FindClass)(e, name));
}
jobject w_GetSuperclass(Env* e, jclass c) {
    const bool app = from_app(__builtin_return_address(0));
    return track_result(e, app, original<jobject (*)(Env*, jclass)>(slot::GetSuperclass)(e, c));
}
jobject w_NewObjectArray(Env* e, jsize length, jclass c, jobject initial) {
    const bool app = from_app(__builtin_return_address(0));
    Temps temps(e);
    return track_result(
        e, app, original<jobject (*)(Env*, jsize, jclass, jobject)>(slot::NewObjectArray)(e, length, c, temps.take(initial)));
}
jobject w_NewByteArray(Env* e, jsize length) {
    const bool app = from_app(__builtin_return_address(0));
    return track_result(e, app, original<jobject (*)(Env*, jsize)>(slot::NewByteArray)(e, length));
}

// ---- references and arrays that may be handed a shadow

jobject w_GetObjectClass(Env* e, jobject o) {
    const bool app = from_app(__builtin_return_address(0));
    Temps temps(e);
    return track_result(e, app, original<jobject (*)(Env*, jobject)>(slot::GetObjectClass)(e, temps.take(o)));
}
jboolean w_IsInstanceOf(Env* e, jobject o, jclass c) {
    Temps temps(e);
    return original<jboolean (*)(Env*, jobject, jclass)>(slot::IsInstanceOf)(e, temps.take(o), c);
}
jobject w_NewGlobalRef(Env* e, jobject o) {
    Temps temps(e);
    return original<jobject (*)(Env*, jobject)>(slot::NewGlobalRef)(e, temps.take(o));
}
jobject w_NewWeakGlobalRef(Env* e, jobject o) {
    Temps temps(e);
    return original<jobject (*)(Env*, jobject)>(slot::NewWeakGlobalRef)(e, temps.take(o));
}
jobject w_NewLocalRef(Env* e, jobject o) {
    const bool app = from_app(__builtin_return_address(0));
    if (is_shadow(o)) {
        jobject array = unwrap_to_array(e, o);
        return track_result(e, app, array);
    }
    return track_result(e, app, original<jobject (*)(Env*, jobject)>(slot::NewLocalRef)(e, o));
}
jboolean w_IsSameObject(Env* e, jobject a, jobject b) {
    if (is_shadow(a) || is_shadow(b)) {
        return a == b;
    }
    return original<jboolean (*)(Env*, jobject, jobject)>(slot::IsSameObject)(e, a, b);
}
jint w_GetObjectRefType(Env* e, jobject o) {
    if (is_shadow(o)) {
        return kRefTypeLocal;
    }
    return original<jint (*)(Env*, jobject)>(slot::GetObjectRefType)(e, o);
}
void w_DeleteLocalRef(Env* e, jobject o) {
    const bool app = from_app(__builtin_return_address(0));
    if (is_shadow(o)) {
        ThreadState* state = thread();
        if (state == nullptr) {
            return;
        }
        for (unsigned i = state->block_count; i > 0; --i) {
            if (state->blocks[i - 1] == o) {
                DalvikArray* block = state->blocks[i - 1];
                state->block_bytes -= block->length;
                for (unsigned j = i; j < state->block_count; ++j) {
                    state->blocks[j - 1] = state->blocks[j];
                }
                --state->block_count;
                free_block(block);
                return;
            }
        }
        return;  // another thread's block: its owner evicts it
    }
    if (app && g.relief_on) {
        untrack(o);
    }
    original<void (*)(Env*, jobject)>(slot::DeleteLocalRef)(e, o);
}
jobject w_PopLocalFrame(Env* e, jobject result) {
    Temps temps(e);
    jobject out = original<jobject (*)(Env*, jobject)>(slot::PopLocalFrame)(e, temps.take(result));
    ThreadState* state = thread();
    if (state != nullptr) {
        state->ring_count = 0;  // the frame's references are gone
    }
    return out;
}

jsize w_GetArrayLength(Env* e, jarray a) {
    if (is_shadow(a)) {
        return static_cast<jsize>(static_cast<const DalvikArray*>(a)->length);
    }
    return original<jsize (*)(Env*, jarray)>(slot::GetArrayLength)(e, a);
}
jbyte* w_GetByteArrayElements(Env* e, jarray a, jboolean* is_copy) {
    if (is_shadow(a)) {
        if (is_copy != nullptr) {
            *is_copy = 0;
        }
        return reinterpret_cast<jbyte*>(static_cast<DalvikArray*>(a)->data);
    }
    return original<jbyte* (*)(Env*, jarray, jboolean*)>(slot::GetByteArrayElements)(e, a, is_copy);
}
void w_ReleaseByteArrayElements(Env* e, jarray a, jbyte* elements, jint mode) {
    if (is_shadow(a)) {
        return;  // the application read the block itself
    }
    original<void (*)(Env*, jarray, jbyte*, jint)>(slot::ReleaseByteArrayElements)(e, a, elements, mode);
}
void* w_GetPrimitiveArrayCritical(Env* e, jarray a, jboolean* is_copy) {
    if (is_shadow(a)) {
        if (is_copy != nullptr) {
            *is_copy = 0;
        }
        return static_cast<DalvikArray*>(a)->data;
    }
    return original<void* (*)(Env*, jarray, jboolean*)>(slot::GetPrimitiveArrayCritical)(e, a, is_copy);
}
void w_ReleasePrimitiveArrayCritical(Env* e, jarray a, void* elements, jint mode) {
    if (is_shadow(a)) {
        return;
    }
    original<void (*)(Env*, jarray, void*, jint)>(slot::ReleasePrimitiveArrayCritical)(e, a, elements, mode);
}
void w_GetByteArrayRegion(Env* e, jarray a, jsize start, jsize length, jbyte* buffer) {
    if (is_shadow(a)) {
        const DalvikArray* block = static_cast<const DalvikArray*>(a);
        if (start >= 0 && length >= 0 && static_cast<uint32_t>(start) + static_cast<uint32_t>(length) <= block->length) {
            __builtin_memcpy(buffer, block->data + start, static_cast<size_t>(length));
            return;
        }
        Temps temps(e);  // a range the block does not have: the runtime throws for a real array
        original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(
            e, temps.take(a), start, length, buffer);
        return;
    }
    original<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(slot::GetByteArrayRegion)(e, a, start, length, buffer);
}
void w_SetByteArrayRegion(Env* e, jarray a, jsize start, jsize length, const jbyte* buffer) {
    if (is_shadow(a)) {
        DalvikArray* block = static_cast<DalvikArray*>(a);
        if (start >= 0 && length >= 0 && static_cast<uint32_t>(start) + static_cast<uint32_t>(length) <= block->length) {
            __builtin_memcpy(block->data + start, buffer, static_cast<size_t>(length));
            return;
        }
        Temps temps(e);
        original<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(slot::SetByteArrayRegion)(
            e, temps.take(a), start, length, buffer);
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

bool is_shadow_block(const void* object) { return is_shadow(object); }

void release_thread_state(void* state) {
    if (state != nullptr && g.shadows_on) {
        forget_all_blocks(static_cast<ThreadState*>(state));
    }
}

Counters counters() { return g.counters; }

void reset_for_test() { __builtin_memset(&g, 0, sizeof(g)); }

bool install_jni_wrappers(void** table, const Platform& platform, void* shadow_region, size_t shadow_size,
                          bool relief, Env* env) {
    if (g.installed) {
        return false;
    }
    g.platform = platform;
    for (int i = 0; i < kSlotCount; ++i) {
        g.orig[i] = table[i];
    }
    g.relief_on = relief;
    if (relief && env != nullptr) {
        // A runtime that grants 4096 more references has no limit worth defending.
        if (original<jint (*)(Env*, jint)>(slot::EnsureLocalCapacity)(env, 4096) == JNI_OK) {
            g.relief_on = false;
        } else {
            original<void (*)(Env*)>(slot::ExceptionClear)(env);
        }
    }
    g.shadows_on = shadow_region != nullptr && shadow_size >= 4096 && g.shadow_arena.init(shadow_region, shadow_size);
    if (g.shadows_on) {
        g.shadow_lo = static_cast<const uint8_t*>(shadow_region);
        g.shadow_hi = g.shadow_lo + shadow_size;
    }
    if (!g.shadows_on && !g.relief_on) {
        return true;  // nothing to intercept
    }
    if (!platform.make_writable(table, kSlotCount * sizeof(void*))) {
        g.shadows_on = false;
        g.relief_on = false;
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
    using ObjectConstructor = Static<jobject, slot::NewObject>;
    table[slot::NewObject] = reinterpret_cast<void*>(&ObjectConstructor::variadic);
    table[slot::NewObjectV] = reinterpret_cast<void*>(&ObjectConstructor::va);
    table[slot::NewObjectA] = reinterpret_cast<void*>(&ObjectConstructor::array);
    table[get_field_slot(kObject)] = reinterpret_cast<void*>(&w_GetObjectField);
    table[get_static_field_slot(kObject)] = reinterpret_cast<void*>(&w_GetStaticObjectField);
    table[set_field_slot(kObject)] = reinterpret_cast<void*>(&w_SetObjectField);
    table[set_static_field_slot(kObject)] = reinterpret_cast<void*>(&w_SetStaticObjectField);
    table[slot::GetObjectArrayElement] = reinterpret_cast<void*>(&w_GetObjectArrayElement);
    table[slot::SetObjectArrayElement] = reinterpret_cast<void*>(&w_SetObjectArrayElement);
    table[slot::NewStringUTF] = reinterpret_cast<void*>(&w_NewStringUTF);
    table[slot::NewString] = reinterpret_cast<void*>(&w_NewString);
    table[slot::FindClass] = reinterpret_cast<void*>(&w_FindClass);
    table[slot::GetSuperclass] = reinterpret_cast<void*>(&w_GetSuperclass);
    table[slot::NewObjectArray] = reinterpret_cast<void*>(&w_NewObjectArray);
    table[slot::NewByteArray] = reinterpret_cast<void*>(&w_NewByteArray);
    table[slot::GetObjectClass] = reinterpret_cast<void*>(&w_GetObjectClass);
    table[slot::IsInstanceOf] = reinterpret_cast<void*>(&w_IsInstanceOf);
    table[slot::NewGlobalRef] = reinterpret_cast<void*>(&w_NewGlobalRef);
    table[slot::NewWeakGlobalRef] = reinterpret_cast<void*>(&w_NewWeakGlobalRef);
    table[slot::NewLocalRef] = reinterpret_cast<void*>(&w_NewLocalRef);
    table[slot::IsSameObject] = reinterpret_cast<void*>(&w_IsSameObject);
    table[slot::GetObjectRefType] = reinterpret_cast<void*>(&w_GetObjectRefType);
    table[slot::DeleteLocalRef] = reinterpret_cast<void*>(&w_DeleteLocalRef);
    table[slot::PopLocalFrame] = reinterpret_cast<void*>(&w_PopLocalFrame);
    table[slot::GetArrayLength] = reinterpret_cast<void*>(&w_GetArrayLength);
    table[slot::GetByteArrayElements] = reinterpret_cast<void*>(&w_GetByteArrayElements);
    table[slot::ReleaseByteArrayElements] = reinterpret_cast<void*>(&w_ReleaseByteArrayElements);
    table[slot::GetPrimitiveArrayCritical] = reinterpret_cast<void*>(&w_GetPrimitiveArrayCritical);
    table[slot::ReleasePrimitiveArrayCritical] = reinterpret_cast<void*>(&w_ReleasePrimitiveArrayCritical);
    table[slot::GetByteArrayRegion] = reinterpret_cast<void*>(&w_GetByteArrayRegion);
    table[slot::SetByteArrayRegion] = reinterpret_cast<void*>(&w_SetByteArrayRegion);
    platform.make_read_only(table, kSlotCount * sizeof(void*));
    return true;
}

bool install_vm_wrappers(void** vm_table, const Platform& platform) {
    if (g.vm_installed) {
        return false;
    }
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
    platform.make_read_only(vm_table, kVmSlotCount * sizeof(void*));
    return true;
}

}  // namespace oldapp

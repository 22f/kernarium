// SPDX-License-Identifier: GPL-2.0-or-later
//
// The old-application layer's JNI interposer (android/lineage-24/overlay/oldapp/jni_shim.cpp)
// against a fake JNI function table that accepts only live references: the slot constants against
// the specification's order (argv[1], tests/unit/andarium/fixtures/jni-slots.tsv), byte[] shadows
// with Dalvik's layout and everything an application can then do with one, the identity and writes
// of the array a shadow stands for, what the runtime is handed in place of a shadow (for any caller,
// whatever the method), the limits (nothing in use is recycled), a thread that ends, PopLocalFrame,
// and the calls of the platform's own libraries, which must see no shadow.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "jni_abi.hpp"
#include "jni_shim.hpp"
#include "require.hpp"

using namespace oldapp;
using namespace oldapp::jni;
using uml::testing::require;

namespace {

// ---- the fake runtime

struct Obj {
    std::vector<uint8_t> bytes;  // byte[] contents; strings keep their text here too
    bool is_array = false;
    bool held = false;           // Java holds it: a collection does not take it
    bool is_class = false;       // what FindClass gave
    int hash = 0;                // its identity hash (given at the first ask)
};
struct Ref {
    Obj* object;                 // null: a weak reference whose object was collected
    bool global;
    bool weak = false;
};

struct Method {
    std::string sig;
    std::string name;
};

struct World {
    std::set<Ref*> locals;
    std::set<Ref*> globals;
    std::set<Ref*> weaks;                   // weak global references
    std::set<Ref*> class_globals;           // the layer's global references to the classes it calls (kept for good)
    int gc_calls = 0;                       // Runtime.gc() calls
    int hash_calls = 0;                     // System.identityHashCode calls
    int next_hash = 100;
    bool same_hash = false;                 // every object has the identity hash 4242
    int weak_new_calls = 0;
    int same_calls = 0;                     // IsSameObject calls that reached the runtime
    bool find_fails = false;                // FindClass fails (an exception)
    bool weak_fails = false;                // NewWeakGlobalRef gives nothing
    std::vector<std::vector<Ref*>> frames;  // PushLocalFrame / PopLocalFrame
    std::map<void*, Method> methods;
    std::map<void*, std::string> fields;
    std::vector<jvalue> last_args;
    std::vector<Obj*> last_array_args;  // the object behind every reference argument, in order
    std::string last_call;
    bool exception_pending = false;
    int app_checks = 0;
    int traps = 0;
    int describe_calls = 0;
    bool describe_fails = false;
    int monitors = 0;
    jobject next_result = nullptr;
    int java_fill = -1;              // >= 0: every array argument of a call is filled with this byte during the call
    bool throw_after_call = false;   // a call leaves an exception pending
    int critical = 0;                // open critical regions of the fake runtime
    int set_calls = 0;               // SetByteArrayRegion calls that reached the runtime
    std::vector<std::vector<uint8_t>> seen_arrays;  // the bytes of every array argument when the call ran
    std::vector<uint8_t> seen_receiver;             // the bytes of an array receiver when the call ran
    int get_calls = 0;                              // GetByteArrayRegion calls that reached the runtime
    int release_calls = 0;                          // Release...Elements / ReleasePrimitiveArrayCritical calls that reached the runtime
    std::vector<const void*> released;              // the pointers they were handed
    struct LogLine {
        std::string message;
        long a, b;
    };
    std::vector<LogLine> logs;  // what the layer logged
    uint32_t clock_ms = 1000;   // the layer's clock
};
World world;

// CheckJNI's rules: nothing but the critical pair runs inside a critical region, and almost nothing with an
// exception pending.
void jni_allowed(const char* what) {
    require(world.critical == 0, what);
    require(!world.exception_pending, what);
}

bool live(jobject ref) {
    Ref* r = static_cast<Ref*>(ref);
    return world.locals.count(r) != 0 || world.globals.count(r) != 0 || world.weaks.count(r) != 0 ||
           world.class_globals.count(r) != 0;
}
Ref* new_local(Obj* object) {
    Ref* ref = new Ref{object, false};
    world.locals.insert(ref);
    if (!world.frames.empty()) world.frames.back().push_back(ref);
    return ref;
}
Obj* object_of(jobject ref) {
    require(live(ref), "the runtime is handed a live reference, never a shadow");
    return static_cast<Ref*>(ref)->object;
}

jobject byte_array(const std::vector<uint8_t>& bytes) {
    Obj* object = new Obj{bytes, true};
    return new_local(object);
}

bool g_app_caller = true;
uint64_t g_epoch = 1;
bool g_no_thread_state = false;                  // the platform has no thread state to give
// Whether a native function is in the application's own libraries (the thunks' domain); a predicate decides by the
// function's address. Independent of the caller (g_app_caller).
thread_local int t_ended_depth = 0;  // how deep this thread is in the ended lock (a wrapper is never entered under it)
bool g_own_default = true;
bool (*g_own_predicate)(const void*) = nullptr;
bool g_without_own_check = false;  // the next install gives the platform no is_own_library (not reset by reset_world)

bool fake_is_app(const void*) {
    require(t_ended_depth == 0, "a wrapper was entered while the ended lock is held");
    ++world.app_checks;
    return g_app_caller;
}
bool fake_is_own(const void* function) { return g_own_predicate != nullptr ? g_own_predicate(function) : g_own_default; }
thread_local void* t_block = nullptr;
thread_local uint64_t t_epoch = 0;
void* fake_thread_state(size_t size) {
    require(t_ended_depth == 0, "the thread state was asked for under the ended lock");
    if (g_no_thread_state) {
        return nullptr;
    }
    if (t_epoch != g_epoch) {
        std::free(t_block);
        t_block = std::calloc(1, size);
        t_epoch = g_epoch;
    }
    return t_block;
}
void fake_log(const char* message, long a, long b) { world.logs.push_back({message, a, b}); }
uint32_t fake_now() { return world.clock_ms; }
bool g_baseline_fails = false;
void* fake_alloc_baseline(size_t bytes) { return g_baseline_fails ? nullptr : std::malloc(bytes); }
void fake_free_baseline(void* baseline) { std::free(baseline); }
bool g_writable_ok = true;
bool g_read_only_ok = true;
bool fake_make_writable(void*, size_t) { return g_writable_ok; }
bool fake_make_read_only(void*, size_t) { return g_read_only_ok; }

// The kinds of a method's parameters from its signature, for an ID the layer never saw being looked up.
bool fake_describe(Env*, void* const*, void* id, bool, jobject, char* kinds, unsigned capacity, unsigned* count) {
    ++world.describe_calls;
    const auto found = world.methods.find(id);
    if (world.describe_fails || found == world.methods.end()) return false;
    const std::string& sig = found->second.sig;
    unsigned n = 0;
    for (size_t i = 1; sig[i] != ')'; ++i) {
        require(n < capacity, "the fake has room");
        switch (sig[i]) {
            case 'Z': case 'B': case 'C': case 'S': case 'I': case 'J': case 'F': case 'D': kinds[n++] = sig[i]; break;
            case 'L': while (sig[i] != ';') ++i; kinds[n++] = 'L'; break;
            case '[':
                while (sig[i] == '[') ++i;
                if (sig[i] == 'L') while (sig[i] != ';') ++i;
                kinds[n++] = 'L';
                break;
            default: require(false, "a signature the fake knows");
        }
    }
    *count = n;
    return true;
}
// The thunks of the fake: thunk i is the address kThunkBase + 8 * i, as the assembly lays them out.
constexpr uintptr_t kThunkBase = 0x70000000u;
void* fake_thunk_address(unsigned index) { return reinterpret_cast<void*>(kThunkBase + 8u * index); }
unsigned thunk_index_of(void* thunk) { return unsigned((reinterpret_cast<uintptr_t>(thunk) - kThunkBase) / 8u); }
bool is_thunk(void* address) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    return a >= kThunkBase && a < kThunkBase + 8u * 4096u;
}
unsigned g_thunk_capacity = 1024;
// The ended blocks' lock: a mutex that no thread takes twice, and that a wrapper is never entered under (the layer
// makes JNI calls under it, and they go to the runtime's own functions).
std::mutex g_ended_mutex;
int g_ended_locks = 0;
void fake_mutex_lock() {
    require(t_ended_depth == 0, "the ended lock is taken twice by one thread");
    g_ended_mutex.lock();
    ++t_ended_depth;
    ++g_ended_locks;
}
void fake_mutex_unlock() {
    require(t_ended_depth == 1, "the ended lock is released by its holder");
    --t_ended_depth;
    g_ended_mutex.unlock();
}
const Platform kPlatform{fake_is_app, fake_thread_state, fake_log, fake_make_writable, fake_make_read_only,
                         fake_describe, fake_now, fake_alloc_baseline, fake_free_baseline, fake_thunk_address, 1024,
                         fake_is_own, fake_mutex_lock, fake_mutex_unlock};

jsize f_GetArrayLength(Env*, jarray a) { return static_cast<jsize>(object_of(a)->bytes.size()); }
jobject f_NewByteArray(Env*, jsize n) { return byte_array(std::vector<uint8_t>(static_cast<size_t>(n), 0)); }
void f_SetByteArrayRegion(Env*, jarray a, jsize start, jsize len, const jbyte* buf) {
    jni_allowed("SetByteArrayRegion where JNI is not allowed (a critical region, or an exception pending)");
    ++world.set_calls;
    auto& bytes = object_of(a)->bytes;
    if (start < 0 || len < 0 || size_t(start) + size_t(len) > bytes.size()) {
        world.exception_pending = true;
        return;
    }
    std::memcpy(bytes.data() + start, buf, size_t(len));
}
void f_GetByteArrayRegion(Env*, jarray a, jsize start, jsize len, jbyte* buf) {
    jni_allowed("GetByteArrayRegion where JNI is not allowed (a critical region, or an exception pending)");
    ++world.get_calls;
    auto& bytes = object_of(a)->bytes;
    if (start < 0 || len < 0 || size_t(start) + size_t(len) > bytes.size()) {
        world.exception_pending = true;
        return;
    }
    std::memcpy(buf, bytes.data() + start, size_t(len));
}
jbyte* f_GetByteArrayElements(Env*, jarray a, jboolean* is_copy) {
    jni_allowed("GetByteArrayElements where JNI is not allowed");
    if (is_copy) *is_copy = 0;
    return reinterpret_cast<jbyte*>(object_of(a)->bytes.data());
}
void f_ReleaseByteArrayElements(Env*, jarray a, jbyte* p, jint) {
    (void)object_of(a);
    ++world.release_calls;
    world.released.push_back(p);
}
void* f_GetPrimitiveArrayCritical(Env*, jarray a, jboolean* is_copy) {
    if (is_copy) *is_copy = 0;
    ++world.critical;
    return object_of(a)->bytes.data();
}
void f_ReleasePrimitiveArrayCritical(Env*, jarray a, void* p, jint) {
    (void)object_of(a);
    ++world.release_calls;
    world.released.push_back(p);
    require(world.critical > 0, "a critical region was open");
    --world.critical;
}
void f_DeleteLocalRef(Env*, jobject o) {
    Ref* ref = static_cast<Ref*>(o);
    require(world.locals.erase(ref) == 1, "a local reference is deleted once, and only while it is live");
    for (auto& frame : world.frames) {
        for (auto at = frame.begin(); at != frame.end();) at = (*at == ref) ? frame.erase(at) : at + 1;
    }
    delete ref;
}
jobject f_NewGlobalRef(Env*, jobject o) {
    jni_allowed("NewGlobalRef where JNI is not allowed");
    require(live(o), "NewGlobalRef is handed a live reference, never a shadow");
    if (static_cast<Ref*>(o)->object == nullptr) return nullptr;  // a weak reference whose object was collected
    Ref* ref = new Ref{object_of(o), true};
    (ref->object->is_class ? world.class_globals : world.globals).insert(ref);
    return ref;
}
jobject f_NewWeakGlobalRef(Env*, jobject o) {
    jni_allowed("NewWeakGlobalRef where JNI is not allowed");
    ++world.weak_new_calls;
    if (world.weak_fails) return nullptr;
    Ref* ref = new Ref{object_of(o), true, true};
    world.weaks.insert(ref);
    return ref;
}
void f_DeleteWeakGlobalRef(Env*, jobject o) {
    require(world.critical == 0, "DeleteWeakGlobalRef inside a critical region");
    Ref* ref = static_cast<Ref*>(o);
    require(world.weaks.erase(ref) == 1, "a weak reference is deleted once, and only while it is live");
    delete ref;
}
void f_ExceptionClear(Env*) { world.exception_pending = false; }
// A collection: a weak reference to an object nothing holds is cleared. Held: a strong reference, or Java.
// The weak references the ended blocks hold (the layer's sentinel, once a block has ended, is one more).
size_t block_weaks() { return world.weaks.size() - (counters().blocks_ended != 0 ? 1 : 0); }
void collect() {
    std::set<Obj*> reachable;
    for (Ref* r : world.locals) reachable.insert(r->object);
    for (Ref* r : world.globals) reachable.insert(r->object);
    for (Ref* r : world.weaks) {
        if (r->object != nullptr && !r->object->held && reachable.count(r->object) == 0) r->object = nullptr;
    }
}
std::function<void(Obj*)> g_on_global_deleted;  // runs inside DeleteGlobalRef, before the reference goes
std::function<void()> g_on_find_class;           // runs inside FindClass: Java code that enters the layer again
void f_DeleteGlobalRef(Env*, jobject o) {
    require(world.critical == 0, "DeleteGlobalRef inside a critical region");  // allowed with an exception pending
    Ref* ref = static_cast<Ref*>(o);
    if (g_on_global_deleted) {
        auto hook = g_on_global_deleted;
        hook(ref->object);
    }
    require(world.globals.erase(ref) == 1, "a global reference is deleted once, and only while it is live");
    delete ref;
}
jobject f_NewLocalRef(Env*, jobject o) { return new_local(object_of(o)); }
jboolean f_IsSameObject(Env*, jobject a, jobject b) {
    require(world.critical == 0, "IsSameObject inside a critical region");
    ++world.same_calls;
    Obj* x = a == nullptr ? nullptr : object_of(a);
    Obj* y = b == nullptr ? nullptr : object_of(b);
    return x == y;
}
jobject f_GetObjectClass(Env*, jobject o) {
    (void)object_of(o);
    return new_local(new Obj{});
}
jboolean f_IsInstanceOf(Env*, jobject o, jclass) {
    (void)object_of(o);
    return 1;
}
jint f_MonitorEnter(Env*, jobject o) {
    (void)object_of(o);
    ++world.monitors;
    return 0;
}
jint f_MonitorExit(Env*, jobject o) {
    (void)object_of(o);
    --world.monitors;
    return 0;
}
jint f_PushLocalFrame(Env*, jint) {
    world.frames.emplace_back();
    return 0;
}
jobject f_PopLocalFrame(Env*, jobject result) {
    require(!world.frames.empty(), "a frame was pushed");
    Obj* keep = result == nullptr ? nullptr : object_of(result);
    for (Ref* r : world.frames.back()) {
        if (world.locals.erase(r) == 1) delete r;
    }
    world.frames.pop_back();
    return keep == nullptr ? nullptr : new_local(keep);
}
int g_register_fail_at = -1;                            // the call (counted from 0) that fails
std::vector<std::vector<NativeMethod>> g_registered;    // what each call of the runtime's RegisterNatives received
jint f_RegisterNatives(Env*, jclass, const NativeMethod* methods, jint count) {
    require(count > 0 && methods != nullptr, "the runtime is handed methods");
    g_registered.emplace_back(methods, methods + count);
    return int(g_registered.size()) - 1 == g_register_fail_at ? JNI_ERR : JNI_OK;
}
jboolean f_ExceptionCheck(Env*) { return world.exception_pending; }
jobject f_FindClass(Env*, const char*) {
    jni_allowed("FindClass where JNI is not allowed");
    require(t_ended_depth == 0, "FindClass runs Java: not under the ended lock");
    if (g_on_find_class) {
        auto hook = g_on_find_class;
        g_on_find_class = nullptr;
        hook();
    }
    if (world.find_fails) {
        world.exception_pending = true;
        return nullptr;
    }
    Obj* object = new Obj{};
    object->is_class = true;
    return new_local(object);
}
jmethodID f_GetMethodID(Env*, jclass, const char* name, const char* sig) {
    jni_allowed("GetMethodID where JNI is not allowed");
    require(t_ended_depth == 0, "a method lookup can run Java: not under the ended lock");
    void* id = new int(0);
    world.methods[id] = Method{sig, name};
    return id;
}
jfieldID f_GetFieldID(Env*, jclass, const char*, const char* sig) {
    void* id = new int(0);
    world.fields[id] = sig;
    return id;
}
jobject f_GetObjectField(Env*, jobject o, jfieldID f) {
    (void)object_of(o);
    require(world.fields.count(f) == 1, "a known field");
    return world.next_result;
}
jobject f_GetStaticObjectField(Env*, jclass, jfieldID f) {
    require(world.fields.count(f) == 1, "a known field");
    return world.next_result;
}

// A call arrives as V (decoded here by the method's signature), A (a jvalue array) or variadic.
std::vector<jvalue> decode_va(const std::string& sig, va_list ap) {
    std::vector<jvalue> args;
    for (size_t i = 1; sig[i] != ')'; ++i) {
        jvalue v;
        v.j = 0;
        switch (sig[i]) {
            case 'Z': v.z = jboolean(va_arg(ap, int)); break;
            case 'B': v.b = jbyte(va_arg(ap, int)); break;
            case 'C': v.c = jchar(va_arg(ap, int)); break;
            case 'S': v.s = jshort(va_arg(ap, int)); break;
            case 'I': v.i = va_arg(ap, jint); break;
            case 'J': v.j = va_arg(ap, jlong); break;
            case 'F': v.f = jfloat(va_arg(ap, double)); break;
            case 'D': v.d = va_arg(ap, double); break;
            case 'L':
                v.l = va_arg(ap, jobject);
                while (sig[i] != ';') ++i;
                break;
            case '[':
                v.l = va_arg(ap, jobject);
                while (sig[i] == '[') ++i;
                if (sig[i] == 'L') while (sig[i] != ';') ++i;
                break;
            default: require(false, "a signature the fake knows");
        }
        args.push_back(v);
    }
    return args;
}
std::vector<jvalue> decode_array(const std::string& sig, const jvalue* in) {
    std::vector<jvalue> args;
    size_t n = 0;
    for (size_t i = 1; sig[i] != ')'; ++i) {
        switch (sig[i]) {
            case 'Z': case 'B': case 'C': case 'S': case 'I': case 'J': case 'F': case 'D': break;
            case 'L': while (sig[i] != ';') ++i; break;
            case '[':
                while (sig[i] == '[') ++i;
                if (sig[i] == 'L') while (sig[i] != ';') ++i;
                break;
            default: require(false, "a signature the fake knows");
        }
        args.push_back(in[n++]);
    }
    return args;
}

// What the runtime saw: references must be real (a live reference of the fake), never a shadow.
void record(const std::string& call, void* method, const std::vector<jvalue>& args) {
    const Method& m = world.methods.at(method);
    world.last_call = call;
    world.last_args = args;
    world.last_array_args.clear();
    world.seen_arrays.clear();
    size_t n = 0;
    for (size_t i = 1; m.sig[i] != ')'; ++i, ++n) {
        if (m.sig[i] == 'L' || m.sig[i] == '[') {
            if (m.sig[i] == '[') {
                while (m.sig[i] == '[') ++i;
                if (m.sig[i] == 'L') while (m.sig[i] != ';') ++i;
            } else {
                while (m.sig[i] != ';') ++i;
            }
            if (args[n].l == nullptr) {
                world.last_array_args.push_back(nullptr);
                continue;
            }
            Obj* object = object_of(args[n].l);
            world.seen_arrays.push_back(object->bytes);  // what Java reads when the call begins
            if (world.java_fill >= 0) std::fill(object->bytes.begin(), object->bytes.end(), uint8_t(world.java_fill));
            world.last_array_args.push_back(object);
        }
    }
    if (world.throw_after_call) world.exception_pending = true;
}
jobject result_of(void* method) {
    const Method& m = world.methods.at(method);
    char kind = m.sig[m.sig.find(')') + 1];
    if (kind == 'V' || kind == 'I') return nullptr;
    return world.next_result;
}

jobject f_CallStaticObjectMethodV(Env*, jclass, jmethodID m, va_list ap) {
    record("static-object-V", m, decode_va(world.methods.at(m).sig, ap));
    return result_of(m);
}
jobject f_CallStaticObjectMethodA(Env*, jclass, jmethodID m, const jvalue* a) {
    if (world.methods.at(m).name == "getRuntime") {  // the layer's own call
        jni_allowed("Runtime.getRuntime where JNI is not allowed");
        return new_local(new Obj{});
    }
    record("static-object-A", m, decode_array(world.methods.at(m).sig, a));
    return result_of(m);
}
jobject f_CallStaticObjectMethod(Env*, jclass, jmethodID, ...) {
    require(false, "the variadic entry of the original is not used by the wrappers");
    return nullptr;
}
jint f_CallStaticIntMethodV(Env*, jclass, jmethodID m, va_list ap) {
    record("static-int-V", m, decode_va(world.methods.at(m).sig, ap));
    return 7;
}
jint f_CallStaticIntMethodA(Env*, jclass, jmethodID m, const jvalue* a) {
    if (world.methods.at(m).name == "identityHashCode") {  // the layer's own call
        jni_allowed("System.identityHashCode where JNI is not allowed");
        ++world.hash_calls;
        Obj* object = object_of(a[0].l);
        if (world.same_hash) return 4242;
        if (object->hash == 0) object->hash = world.next_hash++;
        return object->hash;
    }
    record("static-int-A", m, decode_array(world.methods.at(m).sig, a));
    return 7;
}
void f_CallVoidMethodA(Env*, jobject o, jmethodID m, const jvalue*) {
    require(world.methods.at(m).name == "gc", "the layer calls Runtime.gc and no other instance method of its own");
    jni_allowed("Runtime.gc where JNI is not allowed");
    (void)object_of(o);
    ++world.gc_calls;
    collect();
}
void f_CallStaticVoidMethodV(Env*, jclass, jmethodID m, va_list ap) {
    record("static-void-V", m, decode_va(world.methods.at(m).sig, ap));
}
void f_CallStaticVoidMethodA(Env*, jclass, jmethodID m, const jvalue* a) {
    record("static-void-A", m, decode_array(world.methods.at(m).sig, a));
}
jobject f_NewObjectA(Env*, jclass, jmethodID m, const jvalue* a) {
    record("new-A", m, decode_array(world.methods.at(m).sig, a));
    return new_local(new Obj{});
}
// What Java reads and writes through the receiver of an instance call.
void touch_receiver(jobject o) {
    Obj* object = object_of(o);
    world.seen_receiver.clear();
    if (object->is_array) {
        world.seen_receiver = object->bytes;
        if (world.java_fill >= 0) std::fill(object->bytes.begin(), object->bytes.end(), uint8_t(world.java_fill));
    }
}
jobject f_CallObjectMethodA(Env*, jobject o, jmethodID m, const jvalue* a) {
    touch_receiver(o);
    record("object-A", m, decode_array(world.methods.at(m).sig, a));
    return result_of(m);
}
jobject f_CallObjectMethodV(Env*, jobject o, jmethodID m, va_list ap) {
    touch_receiver(o);
    record("object-V", m, decode_va(world.methods.at(m).sig, ap));
    return result_of(m);
}
jobject f_CallNonvirtualObjectMethodA(Env*, jobject o, jclass, jmethodID m, const jvalue* a) {
    touch_receiver(o);
    record("nonvirtual-A", m, decode_array(world.methods.at(m).sig, a));
    return result_of(m);
}
jobject f_CallNonvirtualObjectMethodV(Env*, jobject o, jclass, jmethodID m, va_list ap) {
    touch_receiver(o);
    record("nonvirtual-V", m, decode_va(world.methods.at(m).sig, ap));
    return result_of(m);
}
jobject f_NewObjectV(Env*, jclass, jmethodID m, va_list ap) {
    record("new-V", m, decode_va(world.methods.at(m).sig, ap));
    return new_local(new Obj{});
}
void f_SetObjectField(Env*, jobject o, jfieldID, jobject value) {
    (void)object_of(o);
    if (value != nullptr) (void)object_of(value);
}
void f_SetObjectArrayElement(Env*, jobject, jsize, jobject value) {
    if (value != nullptr) (void)object_of(value);
}
jobject f_NewObjectArray(Env*, jsize, jclass, jobject initial) {
    if (initial != nullptr) (void)object_of(initial);
    return new_local(new Obj{});
}

void trap(...) {
    ++world.traps;
    require(false, "an original function the test did not provide was called");
}

struct Table {
    void* slots[kSlotCount];
    Env env;
    Table() {
        for (void*& s : slots) s = reinterpret_cast<void*>(&trap);
        slots[slot::GetArrayLength] = reinterpret_cast<void*>(&f_GetArrayLength);
        slots[slot::NewByteArray] = reinterpret_cast<void*>(&f_NewByteArray);
        slots[slot::SetByteArrayRegion] = reinterpret_cast<void*>(&f_SetByteArrayRegion);
        slots[slot::GetByteArrayRegion] = reinterpret_cast<void*>(&f_GetByteArrayRegion);
        slots[slot::GetByteArrayElements] = reinterpret_cast<void*>(&f_GetByteArrayElements);
        slots[slot::ReleaseByteArrayElements] = reinterpret_cast<void*>(&f_ReleaseByteArrayElements);
        slots[slot::GetPrimitiveArrayCritical] = reinterpret_cast<void*>(&f_GetPrimitiveArrayCritical);
        slots[slot::ReleasePrimitiveArrayCritical] = reinterpret_cast<void*>(&f_ReleasePrimitiveArrayCritical);
        slots[slot::DeleteLocalRef] = reinterpret_cast<void*>(&f_DeleteLocalRef);
        slots[slot::NewGlobalRef] = reinterpret_cast<void*>(&f_NewGlobalRef);
        slots[slot::NewWeakGlobalRef] = reinterpret_cast<void*>(&f_NewWeakGlobalRef);
        slots[slot::DeleteWeakGlobalRef] = reinterpret_cast<void*>(&f_DeleteWeakGlobalRef);
        slots[slot::ExceptionClear] = reinterpret_cast<void*>(&f_ExceptionClear);
        slots[call_slot(kVoid, kArray)] = reinterpret_cast<void*>(&f_CallVoidMethodA);
        slots[slot::DeleteGlobalRef] = reinterpret_cast<void*>(&f_DeleteGlobalRef);
        slots[slot::NewLocalRef] = reinterpret_cast<void*>(&f_NewLocalRef);
        slots[slot::IsSameObject] = reinterpret_cast<void*>(&f_IsSameObject);
        slots[slot::GetObjectClass] = reinterpret_cast<void*>(&f_GetObjectClass);
        slots[slot::IsInstanceOf] = reinterpret_cast<void*>(&f_IsInstanceOf);
        slots[slot::MonitorEnter] = reinterpret_cast<void*>(&f_MonitorEnter);
        slots[slot::MonitorExit] = reinterpret_cast<void*>(&f_MonitorExit);
        slots[slot::PushLocalFrame] = reinterpret_cast<void*>(&f_PushLocalFrame);
        slots[slot::PopLocalFrame] = reinterpret_cast<void*>(&f_PopLocalFrame);
        slots[slot::RegisterNatives] = reinterpret_cast<void*>(&f_RegisterNatives);
        slots[slot::ExceptionCheck] = reinterpret_cast<void*>(&f_ExceptionCheck);
        slots[slot::FindClass] = reinterpret_cast<void*>(&f_FindClass);
        slots[slot::GetMethodID] = reinterpret_cast<void*>(&f_GetMethodID);
        slots[slot::GetStaticMethodID] = reinterpret_cast<void*>(&f_GetMethodID);
        slots[slot::GetFieldID] = reinterpret_cast<void*>(&f_GetFieldID);
        slots[slot::GetStaticFieldID] = reinterpret_cast<void*>(&f_GetFieldID);
        slots[get_field_slot(kObject)] = reinterpret_cast<void*>(&f_GetObjectField);
        slots[get_static_field_slot(kObject)] = reinterpret_cast<void*>(&f_GetStaticObjectField);
        slots[set_field_slot(kObject)] = reinterpret_cast<void*>(&f_SetObjectField);
        slots[slot::SetObjectArrayElement] = reinterpret_cast<void*>(&f_SetObjectArrayElement);
        slots[slot::NewObjectArray] = reinterpret_cast<void*>(&f_NewObjectArray);
        slots[static_call_slot(kObject, kVa)] = reinterpret_cast<void*>(&f_CallStaticObjectMethodV);
        slots[static_call_slot(kObject, kArray)] = reinterpret_cast<void*>(&f_CallStaticObjectMethodA);
        slots[static_call_slot(kObject, kVarargs)] = reinterpret_cast<void*>(&f_CallStaticObjectMethod);
        slots[static_call_slot(kInt, kVa)] = reinterpret_cast<void*>(&f_CallStaticIntMethodV);
        slots[static_call_slot(kInt, kArray)] = reinterpret_cast<void*>(&f_CallStaticIntMethodA);
        slots[static_call_slot(kVoid, kVa)] = reinterpret_cast<void*>(&f_CallStaticVoidMethodV);
        slots[static_call_slot(kVoid, kArray)] = reinterpret_cast<void*>(&f_CallStaticVoidMethodA);
        slots[call_slot(kObject, kArray)] = reinterpret_cast<void*>(&f_CallObjectMethodA);
        slots[slot::NewObjectA] = reinterpret_cast<void*>(&f_NewObjectA);
        slots[call_slot(kObject, kVa)] = reinterpret_cast<void*>(&f_CallObjectMethodV);
        slots[nonvirtual_slot(kObject, kArray)] = reinterpret_cast<void*>(&f_CallNonvirtualObjectMethodA);
        slots[nonvirtual_slot(kObject, kVa)] = reinterpret_cast<void*>(&f_CallNonvirtualObjectMethodV);
        slots[slot::NewObjectV] = reinterpret_cast<void*>(&f_NewObjectV);
        env.functions = slots;
    }
};

alignas(16) uint8_t g_shadow_region[2u << 20];
alignas(16) uint8_t g_big_region[8u << 20];
uint8_t* g_region = g_shadow_region;  // the memory of the blocks the next install uses
size_t g_region_size = sizeof(g_shadow_region);

template <class F>
F fn(const Table& table, int slot) {
    return reinterpret_cast<F>(table.slots[slot]);
}

void reset_world() {
    reset_for_test();
    for (Ref* r : world.locals) delete r;
    for (Ref* r : world.globals) delete r;
    for (Ref* r : world.weaks) delete r;
    for (Ref* r : world.class_globals) delete r;
    world = World{};
    ++g_epoch;
    g_app_caller = true;
    g_writable_ok = true;
    g_read_only_ok = true;
    g_baseline_fails = false;
    g_own_predicate = nullptr;
    g_own_default = true;
    g_no_thread_state = false;
    g_ended_locks = 0;
    g_on_global_deleted = nullptr;
    g_on_find_class = nullptr;
    g_thunk_capacity = 1024;
    g_register_fail_at = -1;
    g_registered.clear();
}

// Installs over a fresh fake table.
Table* install(bool shadows = true, unsigned thunks = 1024) {
    reset_world();
    Table* table = new Table;
    Platform platform = kPlatform;
    platform.thunk_capacity = thunks;
    if (g_without_own_check) {
        platform.is_own_library = nullptr;
    }
    require(install_jni_wrappers(table->slots, platform, shadows ? g_region : nullptr, shadows ? g_region_size : 0),
            "the wrappers install");
    return table;
}

void check_slot_constants(const char* fixture_path) {
    std::ifstream file(fixture_path);
    require(file.is_open(), "the slot fixture opens");
    std::map<std::string, int> env_slots;
    std::map<std::string, int> vm_slots;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        std::string table, name;
        int index;
        words >> table >> index >> name;
        (table == "env" ? env_slots : vm_slots)[name] = index;
    }
    require(env_slots.size() == size_t(kSlotCount), "the fixture lists every slot of the table");
    auto expect = [&](const std::string& name, int index) {
        const auto found = env_slots.find(name);
        require(found != env_slots.end() && found->second == index, name.c_str());
    };
    const char* names[] = {"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"};
    const char* variants[] = {"", "V", "A"};
    for (int t = 0; t < 10; ++t) {
        for (int v = 0; v < 3; ++v) {
            expect(std::string("Call") + names[t] + "Method" + variants[v], call_slot(Type(t), Variant(v)));
            expect(std::string("CallNonvirtual") + names[t] + "Method" + variants[v], nonvirtual_slot(Type(t), Variant(v)));
            expect(std::string("CallStatic") + names[t] + "Method" + variants[v], static_call_slot(Type(t), Variant(v)));
        }
    }
    for (int t = 0; t < 9; ++t) {
        expect(std::string("Get") + names[t] + "Field", get_field_slot(Type(t)));
        expect(std::string("Set") + names[t] + "Field", set_field_slot(Type(t)));
        expect(std::string("GetStatic") + names[t] + "Field", get_static_field_slot(Type(t)));
        expect(std::string("SetStatic") + names[t] + "Field", set_static_field_slot(Type(t)));
    }
    for (int t = 1; t < 9; ++t) {
        expect(std::string("New") + names[t] + "Array", new_array_slot(Type(t)));
        expect(std::string("Get") + names[t] + "ArrayElements", get_array_elements_slot(Type(t)));
        expect(std::string("Release") + names[t] + "ArrayElements", release_array_elements_slot(Type(t)));
        expect(std::string("Get") + names[t] + "ArrayRegion", get_array_region_slot(Type(t)));
        expect(std::string("Set") + names[t] + "ArrayRegion", set_array_region_slot(Type(t)));
    }
    expect("GetVersion", slot::GetVersion);
    expect("FindClass", slot::FindClass);
    expect("GetSuperclass", slot::GetSuperclass);
    expect("Throw", slot::Throw);
    expect("ThrowNew", slot::ThrowNew);
    expect("DeleteWeakGlobalRef", slot::DeleteWeakGlobalRef);
    expect("ExceptionClear", slot::ExceptionClear);
    expect("PushLocalFrame", slot::PushLocalFrame);
    expect("PopLocalFrame", slot::PopLocalFrame);
    expect("RegisterNatives", slot::RegisterNatives);
    expect("NewGlobalRef", slot::NewGlobalRef);
    expect("DeleteGlobalRef", slot::DeleteGlobalRef);
    expect("DeleteLocalRef", slot::DeleteLocalRef);
    expect("IsSameObject", slot::IsSameObject);
    expect("NewLocalRef", slot::NewLocalRef);
    expect("EnsureLocalCapacity", slot::EnsureLocalCapacity);
    expect("NewObject", slot::NewObject);
    expect("NewObjectV", slot::NewObjectV);
    expect("NewObjectA", slot::NewObjectA);
    expect("GetObjectClass", slot::GetObjectClass);
    expect("IsInstanceOf", slot::IsInstanceOf);
    expect("GetMethodID", slot::GetMethodID);
    expect("GetFieldID", slot::GetFieldID);
    expect("GetStaticMethodID", slot::GetStaticMethodID);
    expect("GetStaticFieldID", slot::GetStaticFieldID);
    expect("NewString", slot::NewString);
    expect("NewStringUTF", slot::NewStringUTF);
    expect("GetArrayLength", slot::GetArrayLength);
    expect("NewObjectArray", slot::NewObjectArray);
    expect("GetObjectArrayElement", slot::GetObjectArrayElement);
    expect("SetObjectArrayElement", slot::SetObjectArrayElement);
    expect("GetPrimitiveArrayCritical", slot::GetPrimitiveArrayCritical);
    expect("ReleasePrimitiveArrayCritical", slot::ReleasePrimitiveArrayCritical);
    expect("NewWeakGlobalRef", slot::NewWeakGlobalRef);
    expect("ExceptionCheck", slot::ExceptionCheck);
    expect("GetObjectRefType", slot::GetObjectRefType);
    expect("MonitorEnter", slot::MonitorEnter);
    expect("MonitorExit", slot::MonitorExit);
    expect("ToReflectedMethod", slot::ToReflectedMethod);
    expect("GetStringUTFChars", slot::GetStringUTFChars);
    expect("ReleaseStringUTFChars", slot::ReleaseStringUTFChars);
    require(vm_slots.at("GetEnv") == vmslot::GetEnv && vm_slots.at("AttachCurrentThread") == vmslot::AttachCurrentThread &&
                vm_slots.at("AttachCurrentThreadAsDaemon") == vmslot::AttachCurrentThreadAsDaemon &&
                vm_slots.at("DestroyJavaVM") == vmslot::DestroyJavaVM &&
                vm_slots.at("DetachCurrentThread") == vmslot::DetachCurrentThread,
            "the invoke interface slots");
}


// ---- helpers for the scenarios

struct Fixture {
    Table* table;
    Env* env;
    jclass cls;
    jmethodID get_bytes;        // ()[B  (static)
    jmethodID consume;          // ([B)I (static)
    jmethodID mixed;            // (IJLjava/lang/String;[BDF)I (static)
    jmethodID consume_void;     // (Z[B)V
    jmethodID instance_bytes;   // ()[B (instance)
    jmethodID construct;        // ([B)V
    jmethodID not_bytes;        // ()Ljava/lang/Object;
    jmethodID many;             // twenty ints and a byte[]: more parameters than a fixed array of sixteen
    jfieldID bytes_field;       // [B
    jfieldID other_field;       // Ljava/lang/String;
};

Fixture make(bool shadows = true, unsigned thunks = 1024) {
    Fixture f;
    f.table = install(shadows, thunks);
    f.env = &f.table->env;
    f.cls = new_local(new Obj{});
    auto get_static = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetStaticMethodID);
    auto get_method = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetMethodID);
    auto get_field = fn<jfieldID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetFieldID);
    f.get_bytes = get_static(f.env, f.cls, "bytes", "()[B");
    f.consume = get_static(f.env, f.cls, "consume", "([B)I");
    f.mixed = get_static(f.env, f.cls, "mixed", "(IJLjava/lang/String;[BDF)I");
    f.consume_void = get_static(f.env, f.cls, "consumeVoid", "(Z[B)V");
    f.instance_bytes = get_method(f.env, f.cls, "ibytes", "()[B");
    f.construct = get_method(f.env, f.cls, "<init>", "([B)V");
    f.not_bytes = get_static(f.env, f.cls, "other", "()Ljava/lang/Object;");
    f.many = get_static(f.env, f.cls, "many", "(IIIIIIIIIIIIIIIIIIII[B)V");
    f.bytes_field = get_field(f.env, f.cls, "data", "[B");
    f.other_field = get_field(f.env, f.cls, "name", "Ljava/lang/String;");
    return f;
}

std::vector<uint8_t> sample(size_t n) {
    std::vector<uint8_t> bytes(n);
    for (size_t i = 0; i < n; ++i) bytes[i] = uint8_t(0x30 + i % 40);
    return bytes;
}

// The wrappers' variadic entry: the original CallStaticObjectMethod must not be needed by them.
jobject call_static_object_v(Fixture& f, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    jobject r = fn<jobject (*)(Env*, jclass, jmethodID, va_list)>(*f.table, static_call_slot(kObject, kVa))(f.env, f.cls, m, ap);
    va_end(ap);
    return r;
}
jint call_static_int_v(Fixture& f, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    jint r = fn<jint (*)(Env*, jclass, jmethodID, va_list)>(*f.table, static_call_slot(kInt, kVa))(f.env, f.cls, m, ap);
    va_end(ap);
    return r;
}
jobject call_object_v(Fixture& f, jobject receiver, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    jobject r = fn<jobject (*)(Env*, jobject, jmethodID, va_list)>(*f.table, call_slot(kObject, kVa))(f.env, receiver, m, ap);
    va_end(ap);
    return r;
}
void call_static_void_v(Fixture& f, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    fn<void (*)(Env*, jclass, jmethodID, va_list)>(*f.table, static_call_slot(kVoid, kVa))(f.env, f.cls, m, ap);
    va_end(ap);
}

// A byte[] result for application code: the shadow, and the object behind it.
struct Made {
    jobject shadow;
    Obj* object;
};
Made make_shadow_of(Fixture& f, const std::vector<uint8_t>& payload) {
    jobject array = byte_array(payload);
    Obj* object = object_of(array);
    world.next_result = array;
    return Made{call_static_object_v(f, f.get_bytes), object};
}

uint8_t* raw_of(const Made& made) { return const_cast<uint8_t*>(static_cast<const uint8_t*>(made.shadow)) + 16; }
jobject string_object() { return new_local(new Obj{}); }

// ---- scenarios

void check_shadow_layout_and_use() {
    Fixture f = make();
    const auto payload = sample(100);
    jobject array = byte_array(payload);
    Obj* object = object_of(array);
    world.next_result = array;
    const size_t locals_before = world.locals.size();
    jobject shadow = call_static_object_v(f, f.get_bytes);
    require(shadow != nullptr && is_shadow_block(shadow), "a byte[] result for application code is a shadow");
    require(world.locals.size() == locals_before - 1, "the real local reference was released");
    require(world.globals.size() == 1, "the block keeps a global reference to the array");
    require(counters().shadows_created == 1 && counters().shadows_live == 1, "one shadow");
    const uint8_t* raw = static_cast<const uint8_t*>(shadow);
    uint32_t length;
    std::memcpy(&length, raw + 8, 4);
    require(length == 100, "length at +8, as in Dalvik's ArrayObject");
    require(std::memcmp(raw + 0x10, payload.data(), 100) == 0, "the bytes start at +0x10");
    for (int i = 0; i < 16; ++i) require(raw[0x10 + 100 + i] == 0, "zeroes behind the data");
    require((reinterpret_cast<uintptr_t>(shadow) & 15u) == 0, "16-byte aligned");

    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, shadow) == 100, "GetArrayLength");
    jboolean copy = 1;
    jbyte* elements = fn<jbyte* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetByteArrayElements)(f.env, shadow, &copy);
    require(elements == reinterpret_cast<const jbyte*>(raw + 0x10) && copy == 1,
            "the elements are the block, which is separate storage, and the flag says so");
    auto release = fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements);
    elements[0] = 99;
    release(f.env, shadow, elements, kReleaseAbort);
    require(object->bytes[0] == payload[0] && raw[0x10] == payload[0],
            "JNI_ABORT leaves the array as it was, and the block shows it again");
    elements[0] = 99;
    release(f.env, shadow, elements, kReleaseCommit);
    require(object->bytes[0] == 99, "a commit copies the block back");
    elements[1] = 98;
    release(f.env, shadow, elements, 0);
    require(object->bytes[1] == 98 && object->bytes[2] == payload[2], "a release copies the block back");

    jbyte buffer[10];
    fn<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(*f.table, slot::GetByteArrayRegion)(f.env, shadow, 5, 10, buffer);
    require(std::memcmp(buffer, payload.data() + 5, 10) == 0, "GetByteArrayRegion reads the block");
    jbyte patch[3] = {1, 2, 3};
    fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, shadow, 0, 3, patch);
    require(raw[0x10] == 1 && raw[0x12] == 3, "SetByteArrayRegion writes the block");
    require(object->bytes[0] == 1 && object->bytes[2] == 3, "and the array");
    require(!world.exception_pending, "in range, no exception");
    const size_t locals = world.locals.size();
    fn<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(*f.table, slot::GetByteArrayRegion)(f.env, shadow, 95, 10, buffer);
    require(world.exception_pending, "out of range, the runtime raises it for the real array");
    world.exception_pending = false;
    fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, shadow, 95, 10, patch);
    require(world.exception_pending && raw[0x10 + 95] == payload[95], "and for a write; the block is not changed");
    world.exception_pending = false;
    require(world.locals.size() == locals, "no temporary reference exists after either");

    require(fn<jint (*)(Env*, jobject)>(*f.table, slot::GetObjectRefType)(f.env, shadow) == kRefTypeLocal, "a local reference");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, shadow, shadow) == 1, "same");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, shadow, f.cls) == 0, "different");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, shadow, nullptr) == 0, "not null");

    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, shadow);
    require(counters().shadows_live == 0 && world.globals.empty(), "DeleteLocalRef ended the block and its global reference");
    require(!is_shadow_block(shadow), "an ended block is no live shadow");
    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, shadow) == 100,
            "an ended block still knows its length (its reference may be kept)");
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, shadow);  // a second delete is ignored
    require(counters().shadows_live == 0, "and a second delete changes nothing");
}

// The array a shadow stands for keeps its identity, and the ordinary JNI writes reach it.
void check_identity_and_writes() {
    Fixture f = make();
    const auto payload = sample(8);
    Made made = make_shadow_of(f, payload);
    require(is_shadow_block(made.shadow), "a shadow");
    jobject java_side = new_local(made.object);  // the reference the Java side keeps to the same array
    auto same = fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject);
    require(same(f.env, made.shadow, java_side) == 1, "IsSameObject(shadow, the array) is true");
    require(same(f.env, java_side, made.shadow) == 1, "in either order");
    jobject global = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, made.shadow);
    require(world.globals.count(static_cast<Ref*>(global)) && object_of(global) == made.object,
            "NewGlobalRef(shadow) refers to the same object, not a copy");
    require(same(f.env, global, java_side) == 1 && same(f.env, made.shadow, global) == 1, "and compares equal to both");
    jobject weak = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewWeakGlobalRef)(f.env, made.shadow);
    require(object_of(weak) == made.object, "NewWeakGlobalRef(shadow) refers to the same object");
    jobject local = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewLocalRef)(f.env, made.shadow);
    require(!is_shadow_block(local) && object_of(local) == made.object, "NewLocalRef(shadow) is a real reference to the same object");
    jobject cls = fn<jobject (*)(Env*, jobject)>(*f.table, slot::GetObjectClass)(f.env, made.shadow);
    require(world.locals.count(static_cast<Ref*>(cls)), "GetObjectClass(shadow)");
    require(fn<jboolean (*)(Env*, jobject, jclass)>(*f.table, slot::IsInstanceOf)(f.env, made.shadow, f.cls) == 1, "IsInstanceOf(shadow)");
    require(counters().conversions >= 7, "each of those was a conversion");

    jbyte one = 77;
    fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, made.shadow, 3, 1, &one);
    require(made.object->bytes[3] == 77, "SetByteArrayRegion reaches the array");
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release_critical = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    auto* bytes = static_cast<jbyte*>(critical(f.env, made.shadow, nullptr));
    require(bytes == reinterpret_cast<const jbyte*>(static_cast<const uint8_t*>(made.shadow) + 16), "the critical region is the block");
    bytes[0] = 88;
    release_critical(f.env, made.shadow, bytes, kReleaseAbort);
    require(made.object->bytes[0] == payload[0], "an aborted critical region leaves the array");
    bytes[0] = 88;
    release_critical(f.env, made.shadow, bytes, 0);
    require(made.object->bytes[0] == 88, "a released critical region reaches the array");
    // A buffer that is not the block (the application copied the elements) is what Release writes back.
    jbyte mine[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements)(f.env, made.shadow, mine, 0);
    require(made.object->bytes[7] == 9 && static_cast<const uint8_t*>(made.shadow)[16 + 7] == 9,
            "the application's own buffer is written back");

    // The block's global reference goes with the block, and only then.
    const size_t globals = world.globals.size();
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, made.shadow);
    require(world.globals.size() == globals - 1, "deleting the shadow deletes its own global reference");
    require(world.globals.count(static_cast<Ref*>(global)), "and not the one made from it");
    require(object_of(global) == made.object, "which still refers to the array");
}

void check_platform_caller_sees_nothing() {
    Fixture f = make();
    g_app_caller = false;
    const auto payload = sample(40);
    world.next_result = byte_array(payload);
    jobject result = call_static_object_v(f, f.get_bytes);
    require(!is_shadow_block(result) && world.locals.count(static_cast<Ref*>(result)), "the platform gets the real reference");
    require(counters().shadows_created == 0 && world.globals.empty(), "no shadow made");
}

void check_shadow_passed_back() {
    Fixture f = make();
    const auto payload = sample(64);
    Made made = make_shadow_of(f, payload);
    require(is_shadow_block(made.shadow), "a shadow");
    const size_t locals = world.locals.size();
    const size_t globals = world.globals.size();

    require(call_static_int_v(f, f.consume, made.shadow) == 7, "consume(V)");
    require(world.last_call == "static-int-A" && world.last_array_args.size() == 1 && world.last_array_args[0] == made.object,
            "the runtime was handed the array the shadow stands for, through the array form");
    require(world.locals.size() == locals && world.globals.size() == globals, "no temporary reference was made");

    jvalue args[1];
    args[0].l = made.shadow;
    require(fn<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kInt, kArray))(f.env, f.cls, f.consume, args) == 7,
            "consume(A)");
    require(world.last_call == "static-int-A" && world.last_array_args[0] == made.object, "the same through the array form");
    require(args[0].l == made.shadow, "the caller's own array is not modified");

    jobject text = string_object();
    require(call_static_int_v(f, f.mixed, jint(-5), jlong(0x123456789abcdefLL), text, made.shadow, 2.5, 1.5) == 7, "mixed");
    require(world.last_args.size() == 6, "six arguments");
    require(world.last_args[0].i == -5 && world.last_args[1].j == 0x123456789abcdefLL && world.last_args[2].l == text &&
                world.last_args[4].d == 2.5 && world.last_args[5].f == 1.5f,
            "the other arguments arrive unchanged, in order and width");
    require(world.last_array_args.size() == 2 && world.last_array_args[1] == made.object, "the shadow became its array");

    jobject plain = byte_array(sample(8));
    require(call_static_int_v(f, f.consume, plain) == 7, "a real array passes through");
    require(world.last_args[0].l == plain, "unchanged");

    auto void_v = fn<void (*)(Env*, jclass, jmethodID, va_list)>(*f.table, static_call_slot(kVoid, kVa));
    auto call_void = [&](jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        void_v(f.env, f.cls, m, ap);
        va_end(ap);
    };
    call_void(f.consume_void, 1, made.shadow);
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object, "void call");
    auto new_object = fn<jobject (*)(Env*, jclass, jmethodID, va_list)>(*f.table, slot::NewObjectV);
    auto construct = [&](jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        jobject r = new_object(f.env, f.cls, m, ap);
        va_end(ap);
        return r;
    };
    jobject built = construct(f.construct, made.shadow);
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object && world.locals.count(static_cast<Ref*>(built)),
            "a constructor given a shadow");

    fn<void (*)(Env*, jobject, jfieldID, jobject)>(*f.table, set_field_slot(kObject))(f.env, f.cls, f.other_field, made.shadow);
    fn<void (*)(Env*, jobject, jsize, jobject)>(*f.table, slot::SetObjectArrayElement)(f.env, f.cls, 0, made.shadow);
    fn<jobject (*)(Env*, jsize, jclass, jobject)>(*f.table, slot::NewObjectArray)(f.env, 4, f.cls, made.shadow);
    fn<jobject (*)(Env*, jobject, jfieldID)>(*f.table, get_field_slot(kObject))(f.env, made.shadow, f.other_field);
}

// Whoever calls: a platform helper that is handed a shadow turns it back too.
void check_conversion_for_any_caller() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));
    g_app_caller = false;
    require(call_static_int_v(f, f.consume, made.shadow) == 7, "consume as a platform caller");
    require(world.last_array_args[0] == made.object, "the platform's call got the real array");
    require(object_of(fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, made.shadow)) == made.object,
            "NewGlobalRef as a platform caller");
    Made again = make_shadow_of(f, sample(4));
    require(!is_shadow_block(again.shadow), "but the platform is not given a shadow");
    require(counters().shadows_created == 1, "none was made for it");
}

void check_monitor() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));
    require(fn<jint (*)(Env*, jobject)>(*f.table, slot::MonitorEnter)(f.env, made.shadow) == 0 && world.monitors == 1,
            "MonitorEnter reaches the runtime with the real array");
    require(fn<jint (*)(Env*, jobject)>(*f.table, slot::MonitorExit)(f.env, made.shadow) == 0 && world.monitors == 0, "MonitorExit");
}

// A method ID the layer never saw looked up, and a method with more parameters than a fixed array holds.
void check_unknown_and_long_signatures() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(16));
    void* unknown = new int(0);
    world.methods[unknown] = Method{"(I[B)V", "unknown"};
    call_static_void_v(f, unknown, 5, made.shadow);
    require(world.describe_calls == 1, "the signature of an unknown ID is asked for once");
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object, "and the shadow still became its array");
    jvalue args[2];
    args[0].i = 1;
    args[1].l = made.shadow;
    fn<void (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kVoid, kArray))(f.env, f.cls, unknown, args);
    require(world.describe_calls == 1 && world.last_array_args[0] == made.object, "the answer is kept (array form)");

    void* hopeless = new int(0);
    world.methods[hopeless] = Method{"(I)V", "hopeless"};
    world.describe_fails = true;
    call_static_void_v(f, hopeless, 3);
    call_static_void_v(f, hopeless, 4);
    require(world.describe_calls == 2, "an ID that cannot be described is not asked about again");
    world.describe_fails = false;

    // Twenty ints and a byte[] (more than sixteen parameters), the shadow last.
    call_static_void_v(f, f.many, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, made.shadow);
    require(world.last_args.size() == 21 && world.last_args[19].i == 20 && world.last_array_args.size() == 1 &&
                world.last_array_args[0] == made.object,
            "twenty-one parameters: the shadow is converted (V)");
    jvalue many[21];
    for (int i = 0; i < 20; ++i) many[i].i = i + 1;
    many[20].l = made.shadow;
    fn<void (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kVoid, kArray))(f.env, f.cls, f.many, many);
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object && many[20].l == made.shadow,
            "twenty-one parameters: the shadow is converted (A), the caller's array is untouched");
}

void check_fields_and_other_results() {
    Fixture f = make();
    const auto payload = sample(30);
    world.next_result = byte_array(payload);
    jobject receiver = byte_array(sample(3));
    auto instance_v = fn<jobject (*)(Env*, jobject, jmethodID, va_list)>(*f.table, call_slot(kObject, kVa));
    auto call_instance = [&](jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        jobject r = instance_v(f.env, receiver, m, ap);
        va_end(ap);
        return r;
    };
    jobject shadow = call_instance(f.instance_bytes);
    require(is_shadow_block(shadow), "an instance method that returns byte[]");
    world.next_result = byte_array(payload);
    jobject field = fn<jobject (*)(Env*, jobject, jfieldID)>(*f.table, get_field_slot(kObject))(f.env, receiver, f.bytes_field);
    require(is_shadow_block(field), "a byte[] field read");
    world.next_result = byte_array(payload);
    jobject static_field =
        fn<jobject (*)(Env*, jclass, jfieldID)>(*f.table, get_static_field_slot(kObject))(f.env, f.cls, f.bytes_field);
    require(is_shadow_block(static_field), "a static byte[] field read");
    world.next_result = byte_array(payload);
    jobject other = fn<jobject (*)(Env*, jobject, jfieldID)>(*f.table, get_field_slot(kObject))(f.env, receiver, f.other_field);
    require(!is_shadow_block(other), "a String field is the real reference");
    world.next_result = byte_array(payload);
    jobject obj = call_static_object_v(f, f.not_bytes);
    require(!is_shadow_block(obj), "the declared type decides");
    // NewByteArray is not a source of shadows: the application made that array itself.
    jobject fresh = fn<jobject (*)(Env*, jsize)>(*f.table, slot::NewByteArray)(f.env, 4);
    require(!is_shadow_block(fresh) && world.locals.count(static_cast<Ref*>(fresh)), "NewByteArray gives the real reference");
}

// Java writes into the array a block stands for: the block follows it where the application asks for the bytes, and
// after a call that was passed the block.
using GetRegion = void (*)(Env*, jarray, jsize, jsize, jbyte*);
using GetElems = jbyte* (*)(Env*, jarray, jboolean*);
using RelElems = void (*)(Env*, jarray, jbyte*, jint);

// The call that was passed the block fills the array: the raw pointer shows it at once.
void check_block_follows_a_call() {
    Fixture f = make();
    Made passed = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    const uint8_t* raw = static_cast<const uint8_t*>(passed.shadow) + 16;
    world.java_fill = 0x41;
    require(call_static_int_v(f, f.consume, passed.shadow) == 7, "consume");
    require(raw[0] == 0x41 && raw[7] == 0x41, "(variadic form) the block shows what Java wrote during the call");
    std::fill(passed.object->bytes.begin(), passed.object->bytes.end(), uint8_t(0));
    jvalue args[1];
    args[0].l = passed.shadow;
    world.java_fill = 0x42;
    require(fn<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kInt, kArray))(f.env, f.cls, f.consume, args) == 7,
            "consume(A)");
    require(raw[0] == 0x42 && raw[7] == 0x42, "(array form) the same");
    world.java_fill = 0x43;
    jobject built = fn<jobject (*)(Env*, jclass, jmethodID, ...)>(*f.table, slot::NewObject)(f.env, f.cls, f.construct, passed.shadow);
    require(built != nullptr && raw[0] == 0x43, "(NewObject) the same");
    world.java_fill = -1;
}

// Java writes later (another thread, a callback): every way to ask for the bytes shows it, and a release of a
// one-byte change keeps what Java wrote.
void check_block_follows_later_writes() {
    Fixture f = make();
    Made later = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    const uint8_t* later_raw = static_cast<const uint8_t*>(later.shadow) + 16;
    std::fill(later.object->bytes.begin(), later.object->bytes.end(), uint8_t(0x41));
    jbyte buffer[8] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, later.shadow, 0, 8, buffer);
    require(static_cast<uint8_t>(buffer[0]) == 0x41 && static_cast<uint8_t>(buffer[7]) == 0x41 && later_raw[0] == 0x41,
            "GetByteArrayRegion gives Java's bytes, and the block takes them");
    std::fill(later.object->bytes.begin(), later.object->bytes.end(), uint8_t(0x44));
    jbyte* elements = fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, later.shadow, nullptr);
    require(static_cast<uint8_t>(elements[0]) == 0x44 && static_cast<uint8_t>(elements[7]) == 0x44,
            "GetByteArrayElements gives Java's bytes");
    elements[0] = 0x01;
    fn<RelElems>(*f.table, slot::ReleaseByteArrayElements)(f.env, later.shadow, elements, 0);
    require(later.object->bytes[0] == 0x01 && later.object->bytes[5] == 0x44, "Release(0) keeps the bytes Java wrote");
}

// JNI_ABORT: the change is gone from the array and from the block.
void check_abort_gives_the_arrays_bytes_back() {
    Fixture f = make();
    Made aborted = make_shadow_of(f, std::vector<uint8_t>(8, 0x10));
    jbyte* elements = fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, aborted.shadow, nullptr);
    elements[0] = 0x77;
    fn<RelElems>(*f.table, slot::ReleaseByteArrayElements)(f.env, aborted.shadow, elements, kReleaseAbort);
    jbyte one = 0;
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, aborted.shadow, 0, 1, &one);
    require(static_cast<uint8_t>(one) == 0x10 && aborted.object->bytes[0] == 0x10, "after JNI_ABORT the bytes are Java's");
}

// A call that raised an exception leaves the block as it was (no JNI call runs with an exception pending); the next
// request for the bytes shows what Java wrote.
void check_exception_leaves_the_block() {
    Fixture f = make();
    Made thrown = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    const uint8_t* thrown_raw = static_cast<const uint8_t*>(thrown.shadow) + 16;
    world.java_fill = 0x55;
    world.throw_after_call = true;
    call_static_int_v(f, f.consume, thrown.shadow);
    world.java_fill = -1;
    world.throw_after_call = false;
    require(world.exception_pending && thrown_raw[0] == 0, "an exception pending: nothing was called to refresh");
    world.exception_pending = false;
    jbyte one = 0;
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, thrown.shadow, 0, 1, &one);
    require(static_cast<uint8_t>(one) == 0x55 && thrown_raw[0] == 0x55, "and the next request shows Java's bytes");
}

// The critical pair runs inside another array's critical region: only the real array's critical pair is called.
void check_critical_pair_inside_a_region() {
    Fixture f = make();
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release_critical = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0x10));
    std::fill(made.object->bytes.begin(), made.object->bytes.end(), uint8_t(0x42));  // what Java wrote
    jobject plain = byte_array(sample(4));
    void* pinned = critical(f.env, plain, nullptr);
    require(world.critical == 1, "a critical region of a real array is open");
    auto* bytes = static_cast<jbyte*>(critical(f.env, made.shadow, nullptr));
    require(static_cast<uint8_t>(bytes[0]) == 0x42, "the critical pointer shows what Java wrote");
    release_critical(f.env, made.shadow, bytes, kReleaseAbort);
    raw_of(made)[5] = 0x99;  // written through the pointer, never released
    bytes = static_cast<jbyte*>(critical(f.env, made.shadow, nullptr));
    require(static_cast<uint8_t>(bytes[5]) == 0x99 && made.object->bytes[5] == 0x99,
            "a raw write reaches the array through the critical pair");
    bytes[1] = 0x66;
    release_critical(f.env, made.shadow, bytes, 0);
    require(made.object->bytes[1] == 0x66 && made.object->bytes[2] == 0x42, "and its release reaches the array");
    bytes = static_cast<jbyte*>(critical(f.env, made.shadow, nullptr));
    bytes[3] = 0x11;
    release_critical(f.env, made.shadow, bytes, kReleaseAbort);
    require(made.object->bytes[3] == 0x42 && static_cast<const uint8_t*>(made.shadow)[16 + 3] == 0x42,
            "an aborted release gives the block the array's bytes back");
    release_critical(f.env, plain, pinned, 0);
    require(world.critical == 0, "the real region closed");
}

// The blocks of ended threads wait for a wrapped call outside any critical region.
void check_critical_entries_do_not_drain() {
    Fixture f = make();
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release_critical = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0x10));
    std::thread other([&] {
        for (int i = 0; i < 2; ++i) require(is_shadow_block(make_shadow_of(f, sample(8)).shadow), "a shadow");
        release_thread_state(t_block);
    });
    other.join();
    jobject plain = byte_array(sample(4));
    void* pinned = critical(f.env, plain, nullptr);
    const size_t globals = world.globals.size();
    auto* bytes = static_cast<jbyte*>(critical(f.env, made.shadow, nullptr));
    release_critical(f.env, made.shadow, bytes, 0);
    require(world.critical == 1 && world.globals.size() == globals && counters().shadows_released == 0,
            "inside the region no global reference was deleted, and nothing was drained");
    release_critical(f.env, plain, pinned, 0);
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(counters().shadows_released == 2, "the next wrapped call outside a region drained them");
}

// On Dalvik the application's pointer and Java saw one memory. A block and its array are two copies kept alike by
// synchronizing them: bytes the application wrote through the raw pointer and never released reach the array before a
// call that is passed it, and are still there afterwards.

void check_raw_write_reaches_a_call() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    uint8_t* raw = raw_of(made);
    raw[0] = 0xAA;
    raw[1] = 0xAB;
    world.set_calls = 0;
    require(call_static_int_v(f, f.consume, made.shadow) == 7, "consume");
    require(world.seen_arrays.size() == 1 && world.seen_arrays[0][0] == 0xAA && world.seen_arrays[0][1] == 0xAB,
            "Java read what the application wrote through the pointer");
    require(raw[0] == 0xAA && raw[1] == 0xAB && made.object->bytes[0] == 0xAA, "and the bytes are still in both copies");
    require(world.set_calls == 1, "one chunk went to the array");
    call_static_int_v(f, f.consume, made.shadow);
    require(world.set_calls == 1, "a second call with nothing written changes nothing in the array");
}

// Java writes during the call and the application reads the pointer afterwards (the case MR1 fixed), while an earlier
// raw write stays.
void check_raw_write_and_java_write_in_one_call() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    uint8_t* raw = raw_of(made);
    raw[0] = 0xAA;
    world.java_fill = 0x41;
    call_static_int_v(f, f.consume, made.shadow);
    world.java_fill = -1;
    require(world.seen_arrays[0][0] == 0xAA, "Java saw the raw write when the call began");
    require(raw[0] == 0x41 && raw[7] == 0x41 && made.object->bytes[0] == 0x41, "and what Java wrote is in the block after it");
}

// Both sides changed the same chunk since the last synchronization: each side's bytes survive in it, and where both
// wrote the same byte the application's wins (the merge is byte by byte, in every synchronization).
void check_both_sides_changed_one_chunk_both_survive() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0x10));
    uint8_t* raw = raw_of(made);
    raw[0] = 0xAA;                // the application, through the pointer
    raw[2] = 0xAD;                // the application, the byte Java also wrote
    made.object->bytes[1] = 0xBB;  // Java, in the same chunk
    made.object->bytes[2] = 0xBD;
    jbyte buffer[8] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 8, buffer);
    require(static_cast<uint8_t>(buffer[0]) == 0xAA && made.object->bytes[0] == 0xAA, "the application's byte reached the array");
    require(static_cast<uint8_t>(buffer[1]) == 0xBB && made.object->bytes[1] == 0xBB && raw[1] == 0xBB,
            "Java's byte in that chunk survived, in the block too");
    require(static_cast<uint8_t>(buffer[2]) == 0xAD && made.object->bytes[2] == 0xAD, "where both wrote one byte, the application's wins");
    require(raw[3] == 0x10 && made.object->bytes[3] == 0x10, "the others are unchanged");
    // The same through a call: the application's chunk is pushed before it, merged with what Java holds.
    raw[4] = 0xC4;
    made.object->bytes[5] = 0xC5;
    call_static_int_v(f, f.consume, made.shadow);
    require(world.seen_arrays.back()[4] == 0xC4 && world.seen_arrays.back()[5] == 0xC5, "Java saw both writes when the call began");
    require(raw[4] == 0xC4 && raw[5] == 0xC5, "and the block holds both");
}

// The merge decides four bytes at once where it can; every word shape gives the byte-by-byte result: the application wrote
// the whole word, Java did, both did (different bytes, and the same byte), and the last bytes that fill no word.
void check_the_merge_by_words_is_byte_exact() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(4099, 0x10));
    uint8_t* raw = raw_of(made);
    for (unsigned i = 0; i < 4; ++i) {
        raw[0 + i] = static_cast<uint8_t>(0xA0 + i);  // word 0: only the application wrote
        made.object->bytes[4 + i] = static_cast<uint8_t>(0xB0 + i);  // word 1: only Java wrote
    }
    raw[8] = 0xC0;  // word 2: the application wrote the first and third bytes, Java the second and the third
    raw[10] = 0xC2;
    made.object->bytes[9] = 0xD1;
    made.object->bytes[10] = 0xD2;
    raw[12] = 0xE0;  // word 3: both wrote the first byte
    made.object->bytes[12] = 0xE1;
    raw[4096] = 0xF0;  // the three bytes after the last whole word of the block
    raw[4098] = 0xF2;
    made.object->bytes[4097] = 0xF1;
    made.object->bytes[4098] = 0xF3;
    jbyte buffer[16] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 16, buffer);
    require(static_cast<uint8_t>(buffer[0]) == 0xA0 && static_cast<uint8_t>(buffer[3]) == 0xA3 && made.object->bytes[3] == 0xA3,
            "the application's whole word reached the array");
    require(static_cast<uint8_t>(buffer[4]) == 0xB0 && static_cast<uint8_t>(buffer[7]) == 0xB3 && raw[7] == 0xB3,
            "Java's whole word reached the block");
    require(static_cast<uint8_t>(buffer[8]) == 0xC0 && static_cast<uint8_t>(buffer[9]) == 0xD1 && static_cast<uint8_t>(buffer[10]) == 0xC2 &&
                buffer[11] == 0x10,
            "a word both wrote keeps each side's own bytes and the application's where it is the same byte");
    require(static_cast<uint8_t>(buffer[12]) == 0xE0 && made.object->bytes[12] == 0xE0 && raw[12] == 0xE0, "the application wins the same byte");
    jbyte tail[3] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 4096, 3, tail);
    require(static_cast<uint8_t>(tail[0]) == 0xF0 && static_cast<uint8_t>(tail[1]) == 0xF1 && static_cast<uint8_t>(tail[2]) == 0xF2 &&
                made.object->bytes[4098] == 0xF2 && raw[4097] == 0xF1,
            "the bytes after the last whole word merge the same way");
}

// The unit is a chunk of 4096 bytes: a write in one chunk does not hold back what Java wrote in another.
void check_synchronization_is_per_chunk() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(10000, 0));
    uint8_t* raw = raw_of(made);
    raw[9000] = 0x77;                // chunk 2, the application
    made.object->bytes[10] = 0x55;   // chunk 0, Java
    world.set_calls = 0;
    jbyte buffer[20] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 20, buffer);
    require(static_cast<uint8_t>(buffer[10]) == 0x55 && raw[10] == 0x55, "a read of chunk 0 shows Java's write");
    require(world.set_calls == 0 && made.object->bytes[9000] == 0, "and does not touch chunk 2");
    fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, made.shadow, nullptr);
    require(made.object->bytes[9000] == 0x77 && world.set_calls == 1, "the whole array's turn pushes chunk 2, and only it");
}

// A block that neither side touched makes no write to the array at any synchronization point.
void check_a_clean_block_makes_no_array_write() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(9000));
    world.set_calls = 0;
    call_static_int_v(f, f.consume, made.shadow);
    jbyte buffer[100];
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 4000, 100, buffer);
    jbyte* elements = fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, made.shadow, nullptr);
    fn<RelElems>(*f.table, slot::ReleaseByteArrayElements)(f.env, made.shadow, elements, kReleaseAbort);
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release_critical = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    void* pinned = critical(f.env, made.shadow, nullptr);
    release_critical(f.env, made.shadow, pinned, kReleaseAbort);
    require(world.set_calls == 0, "no write to the array");
    require(std::memcmp(raw_of(made), sample(9000).data(), 9000) == 0, "and the block is unchanged");
}

// A SetByteArrayRegion keeps what the application wrote elsewhere in the same chunks.
void check_set_region_keeps_other_raw_writes() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(200, 0));
    raw_of(made)[100] = 0x31;
    jbyte patch[4] = {1, 2, 3, 4};
    fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, made.shadow, 0, 4, patch);
    require(made.object->bytes[100] == 0x31 && made.object->bytes[3] == 4 && raw_of(made)[100] == 0x31 && raw_of(made)[3] == 4,
            "both the region and the earlier raw write are in both copies");
    jbyte buffer[1] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 100, 1, buffer);
    require(static_cast<uint8_t>(buffer[0]) == 0x31, "and a later read still has it");
}

// An exception pending: no JNI call runs, so nothing is pushed or pulled around the call.
void check_no_synchronization_with_an_exception_pending() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    raw_of(made)[0] = 0xAA;
    world.exception_pending = true;
    call_static_int_v(f, f.consume, made.shadow);
    require(world.seen_arrays[0][0] == 0, "the raw write was not pushed");
    world.exception_pending = false;
    jbyte one = 0;
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 1, &one);
    require(static_cast<uint8_t>(one) == 0xAA && made.object->bytes[0] == 0xAA, "it is pushed at the next synchronization point");
}

// A thread holds at most 512 live shadows; past that, and when the region is full, the result is the real
// reference. Nothing a caller may still use is ever recycled.
void check_limits_without_recycling() {
    Fixture f = make();
    std::vector<Made> held;
    for (int i = 0; i < 512; ++i) {
        std::vector<uint8_t> payload = sample(4);
        payload[0] = uint8_t(i);
        held.push_back(make_shadow_of(f, payload));
        require(is_shadow_block(held.back().shadow), "a shadow");
    }
    require(counters().shadows_live == 512 && counters().shadows_declined == 0, "512 live shadows");
    Made refused = make_shadow_of(f, sample(4));
    require(!is_shadow_block(refused.shadow) && world.locals.count(static_cast<Ref*>(refused.shadow)),
            "the 513th result is the real reference");
    require(counters().shadows_declined == 1 && counters().shadows_live == 512, "and it is counted as declined");
    for (int i = 0; i < 512; ++i) {
        const uint8_t* raw = static_cast<const uint8_t*>(held[size_t(i)].shadow);
        require(is_shadow_block(held[size_t(i)].shadow) && raw[16] == uint8_t(i), "every earlier shadow is intact");
        require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, held[size_t(i)].shadow) == 4, "with its length");
    }
    // A shadow the application deletes makes room.
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, held[100].shadow);
    Made again = make_shadow_of(f, sample(4));
    require(is_shadow_block(again.shadow), "after a delete the next result is a shadow again");
    // Another thread has its own 512.
    std::thread other([&] {
        Made mine = make_shadow_of(f, sample(10));
        require(is_shadow_block(mine.shadow), "another thread makes its own");
    });
    other.join();
    // The region is the other limit.
    Fixture g = make();
    Made big = make_shadow_of(g, std::vector<uint8_t>(1500000, 7));
    require(is_shadow_block(big.shadow), "a large array fits the region once");
    Made too_big = make_shadow_of(g, std::vector<uint8_t>(1500000, 8));
    require(!is_shadow_block(too_big.shadow) && counters().shadows_declined == 1, "the next one does not: the real reference");
    require(static_cast<const uint8_t*>(big.shadow)[16] == 7, "and the first is untouched");
}

// The diagnosis lines: the first declined shadow (with what the thread holds) and the most blocks a thread has held,
// at most once every 30 s while it grows.
size_t log_count(const char* part) {
    size_t n = 0;
    for (const auto& line : world.logs) n += line.message.find(part) != std::string::npos;
    return n;
}
void check_diagnosis_lines() {
    Fixture f = make();
    require(log_count("byte[] blocks") == 0 && log_count("stayed a real reference") == 0, "nothing logged before a shadow");
    std::vector<Made> held;
    held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("most live byte[] blocks") == 1 && world.logs.back().a == 1, "the first shadow logs the peak: 1");
    for (int i = 0; i < 20; ++i) held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("most live byte[] blocks") == 1, "growing within 30 s logs nothing more");
    world.clock_ms += 29999;
    held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("most live byte[] blocks") == 1, "29.999 s later still nothing");
    world.clock_ms += 1;
    held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("most live byte[] blocks") == 2 && world.logs.back().a == 23 && world.logs.back().b == 23,
            "after 30 s the peak is logged: 23 blocks, 23 live in the process");
    world.clock_ms += 60000;
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, held[3].shadow);
    held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("most live byte[] blocks") == 2, "a thread's count that does not exceed the peak is not a change");
    const size_t before_filling = log_count("most live byte[] blocks");
    while (counters().shadows_live < 512) held.push_back(make_shadow_of(f, sample(100)));
    require(log_count("stayed a real reference") == 0, "512 blocks are all shadows");
    require(log_count("most live byte[] blocks") == before_filling + 1, "the first one above the logged peak logged it, the rest are within 30 s");
    Made refused = make_shadow_of(f, sample(100));
    require(!is_shadow_block(refused.shadow) && log_count("stayed a real reference") == 1, "the first declined shadow logs once");
    for (const auto& line : world.logs) {
        if (line.message.find("stayed a real reference") != std::string::npos) {
            require(line.message.find("thread limit") != std::string::npos && line.a == 512 && line.b == 512 * 100,
                    "with the reason and the thread's block count and bytes");
        }
    }
    make_shadow_of(f, sample(100));
    require(log_count("stayed a real reference") == 1, "a second declined shadow logs nothing");
    world.clock_ms += 30000;
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, held[4].shadow);
    make_shadow_of(f, sample(100));
    require(log_count("most live byte[] blocks") == before_filling + 2 && world.logs.back().a == 512,
            "and the peak of 512 is logged after 30 s");
    // The region is the other reason, and a new process logs again.
    Fixture g = make();
    world.clock_ms = 5;
    Made big = make_shadow_of(g, std::vector<uint8_t>(1500000, 7));
    Made too_big = make_shadow_of(g, std::vector<uint8_t>(1500000, 8));
    require(is_shadow_block(big.shadow) && !is_shadow_block(too_big.shadow), "the region holds one");
    require(log_count("no room in the region") == 1, "the region's refusal is named");
    for (const auto& line : world.logs) {
        if (line.message.find("no room in the region") != std::string::npos) {
            require(line.a == 1 && line.b == 1500000, "with the thread's one block of 1500000 bytes");
        }
    }
}


// The comparison with the baseline is exact: two words that a 32-bit digest would have called unchanged are a write.
void check_an_exact_comparison_keeps_every_write() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    const uint32_t first = 0x00000001u, second = 0x4f69ba21u;  // the pair the reviewer found with equal digests
    std::memcpy(raw_of(made), &first, 4);
    std::memcpy(raw_of(made) + 4, &second, 4);
    std::vector<uint8_t> written(raw_of(made), raw_of(made) + 8);
    jbyte buffer[8] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 8, buffer);
    require(std::memcmp(buffer, written.data(), 8) == 0 && made.object->bytes == written, "the write reached the array and the reader");
}

// A run of chunks that are alike is one region call (two for a run the application wrote: read, merge, write), and a
// clean chunk is never written.
void check_a_run_of_chunks_is_one_region_call() {
    Fixture f = make();
    Made made = make_shadow_of(f, sample(6 * 4096));
    raw_of(made)[2 * 4096] ^= 0x55;
    raw_of(made)[3 * 4096] ^= 0x55;
    raw_of(made)[4 * 4096 + 5] ^= 0x55;
    world.set_calls = 0;
    world.get_calls = 0;
    call_static_int_v(f, f.consume, made.shadow);
    require(world.set_calls == 1, "chunks 2 to 4 went to the array in one call");
    require(world.get_calls == 2, "after one read of that run (it is merged with Java's bytes), and the six chunks came back in one");
    require(world.seen_arrays[0][2 * 4096] == raw_of(made)[2 * 4096] && world.seen_arrays[0][4 * 4096 + 5] == raw_of(made)[4 * 4096 + 5],
            "Java saw the writes");
    Made big = make_shadow_of(f, sample(256 * 4096));
    world.set_calls = 0;
    world.get_calls = 0;
    call_static_int_v(f, f.consume, big.shadow);
    require(world.set_calls == 0 && world.get_calls == 1, "a clean 1 MiB block: one region call, no write");
    // The range of a small read is one chunk's turn, and only the bytes asked for come over.
    world.get_calls = 0;
    jbyte sixteen[16];
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, big.shadow, 100000, 16, sixteen);
    require(world.get_calls == 1 && world.set_calls == 0, "16 bytes: one small call");
}

// A committing release applies the chunk rule: the chunk the application wrote goes to the array, Java's other chunks stay.
void check_committing_release_keeps_java_bytes_in_clean_chunks() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8192, 0));
    auto elements = fn<jbyte* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetByteArrayElements)(f.env, made.shadow, nullptr);
    auto release = fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements);
    elements[0] = 0x77;
    made.object->bytes[4096] = 0x41;  // Java, in the other chunk
    release(f.env, made.shadow, elements, 0);
    require(made.object->bytes[0] == 0x77 && made.object->bytes[4096] == 0x41, "mode 0: the application's chunk goes, Java's stays");
    require(raw_of(made)[4096] == 0x41, "and the block follows the array");
    elements[1] = 0x78;
    made.object->bytes[5000] = 0x42;
    release(f.env, made.shadow, elements, kReleaseCommit);
    require(made.object->bytes[1] == 0x78 && made.object->bytes[5000] == 0x42, "COMMIT the same");
    elements[2] = 0x79;
    release(f.env, made.shadow, elements, kReleaseAbort);
    require(made.object->bytes[2] == 0 && raw_of(made)[2] == 0, "ABORT discards the working copy");
    require(raw_of(made)[0] == 0x77 && raw_of(made)[1] == 0x78, "and keeps what had been committed");
}

// The reviewer's nested case: the array is open through its own critical pointer and through the block's.
void check_nested_critical_release_keeps_the_other_pointers_bytes() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8192, 0));
    Ref* real = new_local(made.object);
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    uint8_t* original_pointer = static_cast<uint8_t*>(critical(f.env, real, nullptr));
    uint8_t* shadow_pointer = static_cast<uint8_t*>(critical(f.env, made.shadow, nullptr));
    original_pointer[4096] = 0x41;
    shadow_pointer[0] = 0x77;
    release(f.env, made.shadow, shadow_pointer, 0);
    release(f.env, real, original_pointer, 0);
    require(made.object->bytes[0] == 0x77, "the block's write reached the array");
    require(made.object->bytes[4096] == 0x41, "the other pointer's write in the other chunk survived");
    require(raw_of(made)[4096] == 0x41, "and the block shows it");
    // ABORT through the critical pair discards the block's writes.
    shadow_pointer = static_cast<uint8_t*>(critical(f.env, made.shadow, nullptr));
    shadow_pointer[8] = 0x55;
    release(f.env, made.shadow, shadow_pointer, kReleaseAbort);
    require(made.object->bytes[8] == 0 && raw_of(made)[8] == 0, "critical ABORT: the working copy is discarded");
}

// A shadow used as the receiver is synchronized like a parameter, in every call form, also for a method with no parameters.
void check_a_receiver_is_synchronized() {
    Fixture f = make();
    auto get_method = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetMethodID);
    jmethodID clone = get_method(f.env, f.cls, "clone", "()Ljava/lang/Object;");
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    const jvalue none[1] = {};
    for (int form = 0; form < 5; ++form) {
        raw_of(made)[0] = uint8_t(0xA0 + form);
        world.seen_receiver.clear();
        switch (form) {
            case 0: fn<jobject (*)(Env*, jobject, jmethodID, ...)>(*f.table, call_slot(kObject, kVarargs))(f.env, made.shadow, clone); break;
            case 1: call_object_v(f, made.shadow, clone); break;
            case 2: fn<jobject (*)(Env*, jobject, jmethodID, const jvalue*)>(*f.table, call_slot(kObject, kArray))(f.env, made.shadow, clone, none); break;
            case 3: fn<jobject (*)(Env*, jobject, jclass, jmethodID, const jvalue*)>(*f.table, nonvirtual_slot(kObject, kArray))(f.env, made.shadow, f.cls, clone, none); break;
            default: fn<jobject (*)(Env*, jobject, jclass, jmethodID, ...)>(*f.table, nonvirtual_slot(kObject, kVarargs))(f.env, made.shadow, f.cls, clone); break;
        }
        require(world.seen_receiver.size() == 8 && world.seen_receiver[0] == uint8_t(0xA0 + form), "Java read the receiver's bytes");
    }
    // What Java writes through the receiver shows in the block afterwards.
    world.java_fill = 0x31;
    fn<jobject (*)(Env*, jobject, jmethodID, const jvalue*)>(*f.table, call_slot(kObject, kArray))(f.env, made.shadow, clone, none);
    world.java_fill = -1;
    require(raw_of(made)[0] == 0x31 && raw_of(made)[7] == 0x31, "and Java's writes through it come back");
}

// The same array returned twice is one block, counted twice.
void check_one_block_per_array() {
    Fixture f = make();
    jobject array = byte_array(std::vector<uint8_t>(8, 0));
    Obj* object = object_of(array);
    world.next_result = array;
    jobject first = call_static_object_v(f, f.get_bytes);
    world.next_result = new_local(object);
    jobject second = call_static_object_v(f, f.get_bytes);
    require(is_shadow_block(first) && second == first, "the same array is the same block");
    require(counters().shadows_live == 1 && world.globals.size() == 1, "one block, one global reference");
    static_cast<uint8_t*>(first)[16] = 0x66;
    call_static_int_v(f, f.consume, second);
    require(world.seen_arrays[0][0] == 0x66, "a write through the first is seen by Java when the second is passed");
    world.next_result = byte_array(std::vector<uint8_t>(8, 1));
    jobject other = call_static_object_v(f, f.get_bytes);
    require(other != first && counters().shadows_live == 2, "another array is another block");
    auto del = fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef);
    del(f.env, first);
    require(is_shadow_block(second) && counters().shadows_live == 2, "the block outlives one of its two references");
    del(f.env, second);
    require(counters().shadows_live == 1 && world.globals.size() == 1, "and goes with the last");
    // An array of another size, or one that is not among the recent blocks, is not mistaken for it.
    world.next_result = new_local(object);
    require(!is_shadow_block(first) && is_shadow_block(call_static_object_v(f, f.get_bytes)), "it is a new block once the old one is gone");
}

// A real reference to the same array (a global reference, or a pinned pointer) is a second way to reach the bytes, so the
// getters of a shadow must say that their pointer is a copy: conformant code commits when it is told so.
void check_the_flag_of_the_elements_is_truthful() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0));
    Ref* alias = new_local(made.object);  // a real reference to the same array
    auto get_method = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetMethodID);
    jmethodID clone = get_method(f.env, f.cls, "clone", "()Ljava/lang/Object;");
    jboolean copy = 0;
    jbyte* elements = fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, made.shadow, &copy);
    elements[0] = 0x77;
    if (copy != 0) {
        fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements)(f.env, made.shadow, elements, kReleaseCommit);
    }
    const jvalue none[1] = {};
    fn<jobject (*)(Env*, jobject, jmethodID, const jvalue*)>(*f.table, call_slot(kObject, kArray))(f.env, alias, clone, none);
    require(world.seen_receiver.size() == 8 && world.seen_receiver[0] == 0x77,
            "Java, through the real reference, sees the byte of a code that commits when told to");
    require(copy == 1, "the flag of a shadow's elements is true");
    // A real reference keeps the runtime's own flag.
    jboolean real_copy = 1;
    fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, alias, &real_copy);
    require(real_copy == 0, "a real reference gets the runtime's flag");
}
void check_the_flag_of_the_critical_pointer_is_truthful() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8192, 0));
    Ref* alias = new_local(made.object);
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    uint8_t* real = static_cast<uint8_t*>(critical(f.env, alias, nullptr));  // the real array, pinned
    jboolean copy = 0;
    uint8_t* block = static_cast<uint8_t*>(critical(f.env, made.shadow, &copy));
    block[0] = 0x77;
    if (copy != 0) release(f.env, made.shadow, block, 0);
    require(real[0] == 0x77, "the real pinned pointer sees the byte of a code that commits when told to");
    require(copy == 1, "the flag of a shadow's critical pointer is true");
    if (copy == 0) release(f.env, made.shadow, block, 0);
    release(f.env, alias, real, 0);
    jboolean real_copy = 1;
    void* again = critical(f.env, alias, &real_copy);
    require(real_copy == 0, "a real reference gets the runtime's flag");
    release(f.env, alias, again, 0);
}

// The length word of the Dalvik object is the application's to write: the layer trusts only its own capacity.
void check_a_stray_length_write_is_harmless() {
    Fixture f = make();
    std::vector<uint8_t> bytes;
    bytes.reserve(8 + 64);
    bytes.assign(8, 0x10);
    std::memset(bytes.data() + 8, 0xEE, 64);  // behind the array: what an overrun would hit
    jobject array = new_local(new Obj{std::move(bytes), true});
    Obj* object = object_of(array);
    world.next_result = array;
    jobject shadow = call_static_object_v(f, f.get_bytes);
    require(is_shadow_block(shadow), "a shadow");
    const uint32_t stray = 100000;
    std::memcpy(static_cast<uint8_t*>(shadow) + 8, &stray, 4);
    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, shadow) == 8, "the length is the capacity");
    auto elements = fn<jbyte* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetByteArrayElements)(f.env, shadow, nullptr);
    auto release = fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements);
    elements[0] = 0x21;
    release(f.env, shadow, elements, 0);
    auto critical = fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical);
    auto release_critical = fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical);
    uint8_t* pinned = static_cast<uint8_t*>(critical(f.env, shadow, nullptr));
    pinned[1] = 0x22;
    release_critical(f.env, shadow, pinned, 0);
    release_critical(f.env, shadow, critical(f.env, shadow, nullptr), kReleaseAbort);
    jbyte buffer[8] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, shadow, 0, 8, buffer);
    call_static_int_v(f, f.consume, shadow);
    require(object->bytes[0] == 0x21 && object->bytes[1] == 0x22, "the writes reached the array");
    for (int i = 0; i < 64; ++i) require(object->bytes.data()[8 + i] == 0xEE, "and nothing behind it was written");
    jbyte wide[16] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, shadow, 0, 16, wide);
    require(world.exception_pending, "a range beyond the capacity is the runtime's exception, whatever the length word says");
    world.exception_pending = false;
}


// ---- the end of a native method (the thunks, RegisterNatives, PushLocalFrame/PopLocalFrame, DetachCurrentThread)

// A function of an application library and its thunk, as the assembly would run it: the thunk is entered with the
// runtime's return address and a stack pointer; the stub that follows the method's return gives that stack pointer back.
void* app_function(unsigned n) { return reinterpret_cast<void*>(uintptr_t{0x20000000u} + 32u * n); }
struct Native {
    void* thunk;
    unsigned index;
};
Native thunk_of(void* function) {
    void* thunk = thunk_for(function);
    require(is_thunk(thunk), "an application function gets a thunk");
    return Native{thunk, thunk_index_of(thunk)};
}
bool enter_native(const Native& native, void* function, uintptr_t lr, uintptr_t sp) {
    const ThunkEntry entry = thunk_enter(native.index, reinterpret_cast<void*>(lr), reinterpret_cast<void*>(sp));
    require(entry.target == function, "the thunk jumps to its function");
    return entry.pushed == 1;
}
void* leave_native(uintptr_t sp) { return thunk_leave(reinterpret_cast<void*>(sp)); }

// The blocks a native method made end when it returns, whether it deleted any reference or not.
void check_a_native_invocation_ends_its_blocks() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    require(thunk_for(function) == native.thunk, "one thunk for a function, however often it is registered");
    require(counters().natives_wrapped == 1, "counted once");
    require(enter_native(native, function, 0x1234, 0x8000), "a record is pushed");
    Made a = make_shadow_of(f, sample(10));
    Made b = make_shadow_of(f, sample(20));
    Made c = make_shadow_of(f, sample(30));
    require(counters().shadows_live == 3 && world.globals.size() == 3, "three blocks during the invocation");
    require(is_shadow_block(a.shadow) && is_shadow_block(b.shadow) && is_shadow_block(c.shadow), "all usable");
    require(leave_native(0x8000) == reinterpret_cast<void*>(0x1234), "the return stub goes back to the runtime");
    require(counters().shadows_live == 0 && world.globals.empty() && counters().ended_by_return == 3,
            "the blocks ended: no block, no global reference");
    require(!is_shadow_block(a.shadow) && !is_shadow_block(c.shadow), "a block that ended is no block");
    // A reference the method deleted itself is not ended twice.
    require(enter_native(native, function, 0x1234, 0x8000), "again");
    Made d = make_shadow_of(f, sample(10));
    Made e = make_shadow_of(f, sample(10));
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, d.shadow);
    require(counters().shadows_live == 1, "the deleted one is gone");
    leave_native(0x8000);
    require(counters().shadows_live == 0 && world.globals.empty() && counters().ended_by_return == 4 &&
                !is_shadow_block(e.shadow),
            "the other ended at the return");
    // The result of a call that is not byte[] is no block, and nothing else is touched.
    const size_t locals = world.locals.size();
    require(enter_native(native, function, 0x1234, 0x8000), "once more");
    leave_native(0x8000);
    require(world.locals.size() == locals, "an invocation that made no block ends nothing");
}

// A native method that calls Java, which calls native again: each return ends its own blocks and no more.
void check_nested_invocations_end_their_own_blocks() {
    Fixture f = make();
    void* outer_function = app_function(1);
    void* inner_function = app_function(2);
    const Native outer = thunk_of(outer_function);
    const Native inner = thunk_of(inner_function);
    require(enter_native(outer, outer_function, 0x100, 0x9000), "outer");
    Made a = make_shadow_of(f, sample(4));
    require(enter_native(inner, inner_function, 0x200, 0x8000), "the outer method called Java, which called inner");
    Made b = make_shadow_of(f, sample(5));
    Made c = make_shadow_of(f, sample(6));
    require(leave_native(0x8000) == reinterpret_cast<void*>(0x200), "inner returns to the runtime");
    require(counters().shadows_live == 1 && is_shadow_block(a.shadow) && !is_shadow_block(b.shadow) &&
                !is_shadow_block(c.shadow),
            "inner's blocks ended, outer's did not");
    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, a.shadow) == 4, "outer's block still works");
    Made d = make_shadow_of(f, sample(7));
    require(leave_native(0x9000) == reinterpret_cast<void*>(0x100), "outer returns");
    require(counters().shadows_live == 0 && world.globals.empty() && !is_shadow_block(a.shadow) && !is_shadow_block(d.shadow),
            "and its blocks, the ones made before and after the inner call, ended");
    // A recursion twenty levels deep: every level keeps its own block until its own return.
    std::vector<Made> held;
    for (unsigned depth = 0; depth < 20; ++depth) {
        require(enter_native(outer, outer_function, 0x1000 + depth, 0x20000 - 0x100 * depth), "a level");
        std::vector<uint8_t> payload = sample(8);
        payload[0] = uint8_t(depth);
        held.push_back(make_shadow_of(f, payload));
    }
    for (unsigned depth = 20; depth > 0; --depth) {
        const unsigned level = depth - 1;
        require(is_shadow_block(held[level].shadow) && raw_of(held[level])[0] == uint8_t(level), "the level's block is intact");
        require(leave_native(0x20000 - 0x100 * level) == reinterpret_cast<void*>(uintptr_t{0x1000} + level), "the level returns");
        require(!is_shadow_block(held[level].shadow) && counters().shadows_live == level, "and only its block ended");
    }
}

// An exception pending at the return (the method threw, or a Java call it made did): the ends wait, with the blocks
// still strong, because the weak reference and the array write they need are not allowed then; the exception stays, and
// the next call without one finishes them.
void check_a_native_method_that_returns_with_an_exception_pending() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    require(enter_native(native, function, 0x1234, 0x8000), "enter");
    Made a = make_shadow_of(f, sample(10));
    Made b = make_shadow_of(f, sample(11));
    world.exception_pending = true;
    require(leave_native(0x8000) == reinterpret_cast<void*>(0x1234), "the stub goes back");
    require(world.exception_pending && world.weaks.empty() && counters().shadows_ended == 0,
            "no weak reference was made, the exception is still pending");
    require(world.globals.size() == 2 && counters().shadows_live == 2, "the blocks wait, strong");
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(world.exception_pending && world.globals.size() == 2, "a call made with the exception still pending ends nothing");
    world.exception_pending = false;
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(counters().shadows_live == 0 && counters().shadows_ended == 2 && world.globals.empty() && block_weaks() == 2,
            "the next call without one finished the ends: ended, weak");
    require(is_ended_block(a.shadow) && is_ended_block(b.shadow) && !is_shadow_block(a.shadow), "two ended blocks");
}

// The 513th result of a thread is no longer the end of the game's shadows: a method per frame, a result per method.
void check_six_hundred_invocations_decline_nothing() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    for (unsigned i = 0; i < 600; ++i) {
        require(enter_native(native, function, 0x1234, 0x8000), "enter");
        std::vector<uint8_t> payload = sample(100);
        payload[0] = uint8_t(i);
        Made made = make_shadow_of(f, payload);
        require(is_shadow_block(made.shadow) && raw_of(made)[0] == uint8_t(i), "a shadow, the 600th too");
        leave_native(0x8000);
    }
    require(counters().shadows_declined == 0 && counters().shadows_live == 0 && counters().peak_thread_blocks == 1,
            "none declined, none live, never more than one at a time");
    require(counters().ended_by_return == 600 && world.globals.empty(), "every one ended at its return");
}

// Frame records: at most 64. A method entered beyond them has no record and returns as it is; its blocks end with
// the nearest recorded invocation.
void check_more_than_sixty_four_nested_invocations() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    std::vector<Made> held;
    for (unsigned i = 0; i < 70; ++i) {
        const bool pushed = enter_native(native, function, 0x1000 + i, 0x40000 - 0x100 * i);
        require(pushed == (i < 64), "a record for each of the first 64 nested invocations, none after");
        held.push_back(make_shadow_of(f, sample(4)));
    }
    require(counters().shadows_live == 70, "70 blocks");
    // The methods past the 64th return without the stub; the 64th one's return ends all of theirs.
    require(leave_native(0x40000 - 0x100 * 63) == reinterpret_cast<void*>(uintptr_t{0x1000} + 63), "the 64th returns");
    require(counters().shadows_live == 63, "its own and the six after it ended");
    for (int i = 62; i >= 0; --i) {
        require(leave_native(0x40000 - 0x100 * unsigned(i)) == reinterpret_cast<void*>(uintptr_t{0x1000} + unsigned(i)), "return");
    }
    require(counters().shadows_live == 0 && world.globals.empty(), "all ended");
}

// A thread the platform has no state for: no record, the method returns as it is.
void check_no_thread_state_no_record() {
    make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    g_no_thread_state = true;
    require(!enter_native(native, function, 0x1234, 0x8000), "no record");
    g_no_thread_state = false;
}

// A method that never returned (a longjmp over it): the outer return ends its blocks, and the records are trimmed.
void check_an_abandoned_invocation_is_trimmed() {
    Fixture f = make();
    void* outer_function = app_function(1);
    void* inner_function = app_function(2);
    const Native outer = thunk_of(outer_function);
    const Native inner = thunk_of(inner_function);
    require(enter_native(outer, outer_function, 0x100, 0x9000), "outer");
    make_shadow_of(f, sample(4));
    require(enter_native(inner, inner_function, 0x200, 0x8000), "inner");
    make_shadow_of(f, sample(5));
    require(leave_native(0x9000) == reinterpret_cast<void*>(0x100), "outer returns over the inner invocation");
    require(counters().shadows_live == 0 && world.globals.empty(), "both blocks ended");
    // The thread goes on with clean records.
    require(enter_native(outer, outer_function, 0x300, 0x9000), "outer again");
    make_shadow_of(f, sample(4));
    require(leave_native(0x9000) == reinterpret_cast<void*>(0x300) && counters().shadows_live == 0, "and returns");
}

// A PushLocalFrame the method never popped is part of its invocation: the return ends its entries, and ART's own
// segment goes with the invocation.
void check_an_unmatched_push_local_frame_ends_with_the_invocation() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    require(enter_native(native, function, 0x1234, 0x8000), "enter");
    make_shadow_of(f, sample(4));
    require(push(f.env, 16) == 0, "push");
    make_shadow_of(f, sample(4));
    require(counters().shadows_live == 2, "two blocks");
    leave_native(0x8000);
    require(counters().shadows_live == 0 && world.globals.empty(), "the return ended the entries in both");
    // A later method with a pop of its own works on its own frame.
    require(enter_native(native, function, 0x1234, 0x8000), "enter again");
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    require(push(f.env, 16) == 0, "its own push");
    make_shadow_of(f, sample(4));
    pop(f.env, nullptr);
    require(counters().shadows_live == 0, "its pop ended its own entry");
    leave_native(0x8000);
}

// The distinct natives that can have a thunk: 1024; past them a function is registered as it is and the block it
// makes ends with its references.
void check_the_thunks_run_out() {
    make();
    for (unsigned i = 0; i < 1030; ++i) {
        void* function = app_function(i);
        void* thunk = thunk_for(function);
        if (i < 1024) {
            require(is_thunk(thunk) && thunk_index_of(thunk) == i, "the first 1024 functions get a thunk each");
        } else {
            require(thunk == function, "then the function itself");
        }
    }
    require(counters().natives_wrapped == 1024 && counters().natives_unwrapped == 6, "counted");
    require(log_count("got no thunk") == 1, "and the first one is logged, once");
    require(thunk_for(app_function(5)) == fake_thunk_address(5), "a function that has a thunk still gets it");
    require(counters().natives_unwrapped == 6, "which is not a new miss");
    make(true, 4);
    for (unsigned i = 0; i < 4; ++i) require(is_thunk(thunk_for(app_function(i))), "the platform's capacity is the limit");
    require(thunk_for(app_function(4)) == app_function(4), "past it, none");
}

// A native method that got no thunk (the thunks ran out) is bound as it is, with one log line: the runtime enters and
// leaves it with no record, so its blocks end only by the other exact ends (DeleteLocalRef, PopLocalFrame,
// DetachCurrentThread) or count toward the thread's 512, the result beyond it staying the real reference.
void check_a_method_without_a_thunk_ends_by_the_other_ends() {
    Fixture f = make(true, 2);
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    auto del = fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef);
    require(is_thunk(thunk_for(app_function(0))) && is_thunk(thunk_for(app_function(1))), "two thunks, both in use");
    void* without = app_function(2);
    require(thunk_for(without) == without && thunk_for(app_function(3)) == app_function(3),
            "the next functions are bound as they are");
    require(counters().natives_wrapped == 2 && counters().natives_unwrapped == 2 && log_count("got no thunk") == 1,
            "counted, and logged once");
    // Called by the runtime directly: no record, no return stub. Its blocks stay until something ends them.
    require(push(f.env, 16) == 0, "a frame");
    for (int i = 0; i < 3; ++i) make_shadow_of(f, sample(4 + size_t(i)));
    require(counters().shadows_live == 3, "the blocks of the method stay with their references");
    pop(f.env, nullptr);
    require(counters().shadows_live == 0 && counters().ended_by_pop == 3 && counters().ended_by_return == 0,
            "PopLocalFrame ends them");
    std::vector<Made> held;
    for (int i = 0; i < 512; ++i) {
        std::vector<uint8_t> payload = sample(4);
        payload[0] = uint8_t(i);
        held.push_back(make_shadow_of(f, payload));
    }
    require(counters().shadows_live == 512 && counters().shadows_declined == 0, "512 count toward the limit");
    Made refused = make_shadow_of(f, sample(4));
    require(!is_shadow_block(refused.shadow) && counters().shadows_declined == 1, "the 513th is the real reference");
    del(f.env, held[7].shadow);
    require(is_shadow_block(make_shadow_of(f, sample(4)).shadow), "DeleteLocalRef makes room");
    struct Fns {
        static jint detach(Vm*) { return 0; }
        static jint other(Vm*, void**, jint) { return 0; }
    };
    void* vm_table[kVmSlotCount];
    for (void*& entry : vm_table) entry = reinterpret_cast<void*>(&Fns::other);
    vm_table[vmslot::DetachCurrentThread] = reinterpret_cast<void*>(&Fns::detach);
    require(install_vm_wrappers(vm_table, kPlatform), "the invoke table installs");
    Vm vm{vm_table};
    reinterpret_cast<jint (*)(Vm*)>(vm_table[vmslot::DetachCurrentThread])(&vm);
    require(counters().shadows_live == 0 && counters().ended_by_detach == 512 && world.globals.empty(),
            "DetachCurrentThread ends the rest");
}

// Which functions get a thunk: those of the application's own libraries, with shadows on and thunks provided. The
// function's address decides; who is calling does not.
void check_which_functions_get_a_thunk() {
    make();
    g_own_default = false;
    require(thunk_for(app_function(1)) == app_function(1) && counters().natives_wrapped == 0,
            "a function outside the application's own libraries (another package's, a platform library): none");
    g_app_caller = false;
    g_own_default = true;
    require(is_thunk(thunk_for(app_function(1))), "an own function gets its thunk whoever is calling");
    g_app_caller = true;
    g_own_default = false;
    require(thunk_for(app_function(2)) == app_function(2), "and a foreign one gets none from an application caller");
    g_own_default = true;
    require(thunk_for(nullptr) == nullptr, "no function, none");
    make(/*shadows=*/false);
    require(thunk_for(app_function(1)) == app_function(1), "with no shadows the function stays as it is");
    make(true, 0);
    require(thunk_for(app_function(1)) == app_function(1), "and with no thunks");
    reset_world();
    require(thunk_for(app_function(1)) == app_function(1), "and before the wrappers are installed");
    // A platform that cannot tell the libraries apart: the application's code is the domain.
    g_without_own_check = true;
    make();
    g_without_own_check = false;
    require(is_thunk(thunk_for(app_function(1))), "with no own-library test, application code");
    g_app_caller = false;
    require(thunk_for(app_function(2)) == app_function(2), "and a platform function: none");
}

bool own_even(const void* address) { return ((reinterpret_cast<uintptr_t>(address) - 0x20000000u) / 32u) % 2 == 0; }

// RegisterNatives: the application's functions are registered through their thunks, in chunks, with the rest of
// the call unchanged and the runtime's first failure ending it.
void check_register_natives_registers_thunks() {
    Fixture f = make();
    g_own_predicate = own_even;
    std::vector<NativeMethod> methods;
    for (unsigned i = 0; i < 70; ++i) methods.push_back(NativeMethod{"name", "()V", app_function(i)});
    auto register_natives = fn<jint (*)(Env*, jclass, const NativeMethod*, jint)>(*f.table, slot::RegisterNatives);
    require(register_natives(f.env, f.cls, methods.data(), 70) == JNI_OK, "registered");
    std::vector<NativeMethod> got;
    for (const auto& call : g_registered) got.insert(got.end(), call.begin(), call.end());
    require(g_registered.size() == 3 && got.size() == 70, "three calls of the runtime, 32 + 32 + 6 methods");
    for (unsigned i = 0; i < 70; ++i) {
        require(got[i].name == methods[i].name && got[i].signature == methods[i].signature, "names and signatures unchanged");
        require(methods[i].function == app_function(i), "the caller's array is unchanged");
        if (i % 2 == 0) {
            require(is_thunk(got[i].function), "an application function is registered through its thunk");
            require(thunk_for(app_function(i)) == got[i].function, "which is its thunk");
        } else {
            require(got[i].function == app_function(i), "a function outside the application's libraries as it is");
        }
    }
    // The function's address decides, not the caller's: a platform library registering an own function gets its
    // thunk, an application library registering a foreign one gets none.
    g_own_predicate = nullptr;
    g_registered.clear();
    methods.assign({NativeMethod{"a", "()V", app_function(100)}});
    g_app_caller = false;
    require(register_natives(f.env, f.cls, methods.data(), 1) == JNI_OK && is_thunk(g_registered.back()[0].function),
            "a platform caller registering an own function: its thunk");
    g_app_caller = true;
    g_own_default = false;
    methods.assign({NativeMethod{"b", "()V", app_function(101)}});
    require(register_natives(f.env, f.cls, methods.data(), 1) == JNI_OK && g_registered.back()[0].function == app_function(101),
            "an application caller registering a foreign function: as it is");
    g_own_default = true;
    g_registered.clear();
    methods.assign(70, NativeMethod{"name", "()V", app_function(0)});
    g_register_fail_at = 1;
    require(register_natives(f.env, f.cls, methods.data(), 70) == JNI_ERR && g_registered.size() == 2,
            "the runtime's error ends the registration and is the result");
}

// The attachment of a thread ends its references.
void check_detach_ends_the_references_of_a_thread() {
    Fixture f = make();
    struct Fns {
        static jint detach(Vm*) { return 0; }
        static jint other(Vm*, void**, jint) { return 0; }
    };
    void* vm_table[kVmSlotCount];
    for (void*& s : vm_table) s = reinterpret_cast<void*>(&Fns::other);
    vm_table[vmslot::DetachCurrentThread] = reinterpret_cast<void*>(&Fns::detach);
    require(install_vm_wrappers(vm_table, kPlatform), "the invoke table installs");
    Vm vm{vm_table};
    std::thread attached([&] {
        for (int i = 0; i < 5; ++i) require(is_shadow_block(make_shadow_of(f, sample(8)).shadow), "a shadow");
        require(counters().shadows_live == 5, "five references");
        const jint rc = reinterpret_cast<jint (*)(Vm*)>(vm_table[vmslot::DetachCurrentThread])(&vm);
        require(rc == 0, "the runtime's detach is the result");
        require(counters().shadows_live == 0 && world.globals.empty() && counters().ended_by_detach == 5,
                "the five ended with the attachment");
        // Attached again, it makes more; its end (no detach) leaves them to the next call with an environment.
        for (int i = 0; i < 2; ++i) make_shadow_of(f, sample(8));
        require(counters().shadows_live == 2, "the new attachment has its own");
        release_thread_state(t_block);
    });
    attached.join();
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(counters().shadows_live == 0 && world.globals.empty(), "and the thread's end freed those");
}

// PushLocalFrame and PopLocalFrame: the entries made in a frame end with it; the shadow a pop is handed stays, with an
// entry in the outer frame.
void check_push_and_pop_local_frame_end_their_entries() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    auto del = fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef);
    Made base = make_shadow_of(f, sample(4));
    require(push(f.env, 16) == 0, "push");
    make_shadow_of(f, sample(5));
    make_shadow_of(f, sample(6));
    require(counters().shadows_live == 3, "three blocks, two of them in the frame");
    pop(f.env, nullptr);
    require(counters().shadows_live == 1 && is_shadow_block(base.shadow) && counters().ended_by_pop == 2,
            "the pop ended the frame's two, not the one below");
    // Nested frames.
    require(push(f.env, 16) == 0, "outer frame");
    Made x = make_shadow_of(f, sample(7));
    require(push(f.env, 16) == 0, "inner frame");
    Made y = make_shadow_of(f, sample(8));
    pop(f.env, nullptr);
    require(is_shadow_block(x.shadow) && !is_shadow_block(y.shadow), "the inner pop ended the inner frame's block");
    pop(f.env, nullptr);
    require(!is_shadow_block(x.shadow) && is_shadow_block(base.shadow), "and the outer one the outer frame's");
    // A shadow handed to the pop: it comes out into the outer frame and stays until it is deleted there.
    require(push(f.env, 16) == 0, "a frame");
    Made result = make_shadow_of(f, sample(9));
    make_shadow_of(f, sample(9));
    jobject out = pop(f.env, result.shadow);
    require(out == result.shadow && is_shadow_block(result.shadow) && counters().shadows_live == 2,
            "the result stayed, the other block of the frame ended");
    del(f.env, result.shadow);
    require(!is_shadow_block(result.shadow) && counters().shadows_live == 1, "the outer frame's reference is deleted once");
    del(f.env, base.shadow);
    require(counters().shadows_live == 0 && world.globals.empty(), "nothing is left");
}

// More PushLocalFrame calls than there are records: the pops of the unrecorded ones end nothing, the entries made
// in them end with the last recorded frame.
void check_frames_beyond_the_records() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    for (unsigned i = 0; i < 70; ++i) {
        require(push(f.env, 4) == 0, "push");
        make_shadow_of(f, sample(4));
    }
    require(counters().shadows_live == 70, "70 blocks");
    for (unsigned i = 0; i < 6; ++i) pop(f.env, nullptr);
    require(counters().shadows_live == 70, "the six pops of unrecorded frames ended nothing");
    pop(f.env, nullptr);
    require(counters().shadows_live == 63, "the 64th frame's pop ended its entries and those of the frames after it");
    for (unsigned i = 0; i < 63; ++i) pop(f.env, nullptr);
    require(counters().shadows_live == 0 && world.globals.empty(), "and the rest");
}

// DeleteLocalRef takes the newest entry of the block; the frames above it stay right.
void check_delete_local_ref_keeps_the_frames_right() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    auto del = fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef);
    Made a = make_shadow_of(f, sample(4));
    Made b = make_shadow_of(f, sample(5));
    Made c = make_shadow_of(f, sample(6));
    require(push(f.env, 16) == 0, "push");
    Made d = make_shadow_of(f, sample(7));
    del(f.env, b.shadow);  // an entry below the frame
    pop(f.env, nullptr);
    require(!is_shadow_block(d.shadow) && !is_shadow_block(b.shadow), "the frame's block ended, and the deleted one");
    require(is_shadow_block(a.shadow) && is_shadow_block(c.shadow) && counters().shadows_live == 2,
            "the pop did not end the entries below the frame, whatever was deleted there");
    // The same array twice is one block with two entries: the first delete leaves it, the second ends it.
    jobject array = byte_array(sample(12));
    Obj* object = object_of(array);
    world.next_result = array;
    jobject first = call_static_object_v(f, f.get_bytes);
    world.next_result = new_local(object);
    jobject second = call_static_object_v(f, f.get_bytes);
    require(first == second && counters().shadows_live == 3, "one block");
    del(f.env, first);
    require(is_shadow_block(first) && counters().shadows_live == 3, "one reference left");
    del(f.env, second);
    require(!is_shadow_block(first) && counters().shadows_live == 2, "and none");
    // An entry made in a native invocation for an array the invocation's caller already holds goes with the invocation only.
    void* function = app_function(1);
    const Native native = thunk_of(function);
    jobject again = byte_array(sample(12));
    Obj* again_object = object_of(again);
    world.next_result = again;
    jobject held = call_static_object_v(f, f.get_bytes);
    require(enter_native(native, function, 0x1234, 0x8000), "enter");
    world.next_result = new_local(again_object);
    jobject same = call_static_object_v(f, f.get_bytes);
    require(same == held, "the same array is the same block");
    leave_native(0x8000);
    require(is_shadow_block(held), "the invocation's reference ended, the block of the caller's stays");
    del(f.env, held);
    require(!is_shadow_block(held), "and goes with its own");
}

// The diagnosis lines of the thunks: the first block that ended at a return, and the first function that got none.
void check_the_thunk_diagnosis_lines() {
    Fixture f = make();
    void* function = app_function(1);
    const Native native = thunk_of(function);
    require(log_count("ended at the return of a native method") == 0, "nothing yet");
    for (int i = 0; i < 3; ++i) {
        enter_native(native, function, 0x1234, 0x8000);
        make_shadow_of(f, sample(4));
        make_shadow_of(f, sample(4));
        leave_native(0x8000);
    }
    require(log_count("ended at the return of a native method") == 1, "once");
    for (const auto& line : world.logs) {
        if (line.message.find("ended at the return") != std::string::npos) {
            require(line.a == 2 && line.b == 1, "with the references it ended and the natives with a thunk");
        }
    }
    // The most one return has ended: logged when it grows, at most every 30 s, and a plateau reached inside the interval
    // is logged by the next return after it.
    const auto invocation = [&](int blocks) {
        enter_native(native, function, 0x1234, 0x8000);
        for (int i = 0; i < blocks; ++i) make_shadow_of(f, sample(4));
        leave_native(0x8000);
    };
    require(log_count("the most references one native method call") == 0, "no line for the first return's count");
    world.clock_ms += 31000;
    invocation(5);
    require(log_count("the most references one native method call") == 1 && world.logs.back().a == 5 && world.logs.back().b == 1,
            "a larger count is logged after 30 s");
    invocation(7);
    require(log_count("the most references one native method call") == 1, "not again within 30 s");
    world.clock_ms += 31000;
    invocation(1);
    require(log_count("the most references one native method call") == 2 && world.logs.back().a == 7,
            "and the count reached in between, by the next return after 30 s");
    require(counters().max_ended_by_return == 7, "the counter keeps the most");
}

// A thread ends: its blocks wait for an environment, then go with their global references.
void check_thread_end() {
    Fixture f = make();
    void* ended_state = nullptr;
    std::thread other([&] {
        for (int i = 0; i < 3; ++i) require(is_shadow_block(make_shadow_of(f, sample(8)).shadow), "a shadow");
        ended_state = t_block;
        release_thread_state(ended_state);  // what the thread's exit does: no JNI call
    });
    other.join();
    require(counters().shadows_live == 3 && world.globals.size() == 3, "the blocks wait: no environment on the exiting thread");
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(counters().shadows_live == 0 && world.globals.empty() && counters().shadows_released == 3,
            "the next wrapped call with an environment freed them and deleted their global references");
}

// PopLocalFrame(shadow): the frame's references go, the shadow (in no table) stays and is what the caller gets.
void check_pop_local_frame() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    const size_t locals = world.locals.size();
    require(push(f.env, 32) == 0, "push");
    Made made = make_shadow_of(f, sample(4));
    jobject inside = fn<jobject (*)(Env*, const char*)>(*f.table, slot::FindClass)(f.env, "x");
    require(is_shadow_block(made.shadow) && world.locals.count(static_cast<Ref*>(inside)), "a shadow and another local in the frame");
    jobject result = pop(f.env, made.shadow);
    require(result == made.shadow && is_shadow_block(result), "the same block comes out of the frame");
    require(world.locals.size() == locals, "the frame's other references are gone");
    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, result) == 4, "and it is usable");
    require(!world.exception_pending, "no stale reference was deleted");
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, result);
    // A real reference and a null result take the original path.
    push(f.env, 32);
    jobject real = byte_array(sample(4));
    jobject out = pop(f.env, real);
    require(world.locals.count(static_cast<Ref*>(out)) && !is_shadow_block(out), "a real result is the original's");
    push(f.env, 32);
    require(pop(f.env, nullptr) == nullptr, "no result");
}

// ---- ended blocks: a block whose references ended stays at its address until its array is collected

void end_ref(Fixture& f, jobject shadow) { fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, shadow); }

// A block of an array Java holds (a collection does not take it).
Made make_held(Fixture& f, const std::vector<uint8_t>& payload) {
    Made made = make_shadow_of(f, payload);
    made.object->held = true;
    return made;
}

// The same array obtained again from a call: a new local reference to the same object.
Made obtain_again(Fixture& f, Obj* object) {
    world.next_result = new_local(object);
    return Made{call_static_object_v(f, f.get_bytes), object};
}

// A kept pointer reads its bytes after a thousand later blocks of the same size, and no address is reused.
void check_a_kept_pointer_keeps_its_bytes() {
    Fixture f = make();
    std::vector<uint8_t> payload = sample(100);
    Made kept = make_held(f, payload);
    const uint8_t* p = static_cast<const uint8_t*>(kept.shadow);
    end_ref(f, kept.shadow);
    require(is_ended_block(p) && !is_shadow_block(p), "the block ended, and is no live block");
    require(counters().shadows_live == 0 && counters().shadows_ended == 1 && counters().ended_bytes == 100 &&
                world.globals.empty() && block_weaks() == 1,
            "no strong reference is left, a weak one is, and the block is counted as ended");
    std::set<const void*> addresses;
    for (int i = 0; i < 1000; ++i) {
        std::vector<uint8_t> other = sample(100);
        other[0] = uint8_t(i);
        Made temporary = make_shadow_of(f, other);
        require(is_shadow_block(temporary.shadow) && temporary.shadow != kept.shadow, "a new block, not the kept one's address");
        require(addresses.insert(temporary.shadow).second, "and no address is reused while nothing was collected");
        end_ref(f, temporary.shadow);
    }
    require(std::memcmp(p + 16, payload.data(), 100) == 0, "the kept pointer reads the bytes it had");
    require(counters().shadows_ended == 1001 && counters().shadows_declined == 0, "1001 ended blocks, none declined");
    require(g_ended_locks > 0, "the index is under the ended lock");
}

// A collection: the next creations free the ended blocks whose arrays are gone, and the next block of the size takes
// the address.
void check_a_collection_frees_ended_blocks() {
    Fixture f = make();
    Made gone = make_shadow_of(f, sample(100));
    Made kept = make_held(f, sample(100));
    const void* gone_at = gone.shadow;
    end_ref(f, gone.shadow);
    end_ref(f, kept.shadow);
    require(counters().shadows_ended == 2, "two ended blocks");
    Made before = make_shadow_of(f, sample(50));  // no collection yet: nothing is tested
    require(counters().blocks_collected == 0 && world.same_calls <= 1 + 8, "no collection, no sweep");
    collect();
    Made after = make_shadow_of(f, sample(100));
    require(counters().blocks_collected == 1 && counters().shadows_ended == 1, "the block of the gone array was freed");
    require(after.shadow == gone_at, "and the next block of that size takes its address");
    require(is_ended_block(kept.shadow), "the block of the array Java holds stays");
    require(block_weaks() == 1, "one weak reference is left");
    (void)before;
}

// A sweep tests 32 ended blocks at a creation, one test of the sentinel when nothing was collected.
void check_a_sweep_takes_thirty_two_blocks_at_a_creation() {
    Fixture f = make();
    for (int i = 0; i < 101; ++i) {
        std::vector<uint8_t> payload = sample(20);
        payload[0] = uint8_t(i);
        end_ref(f, make_shadow_of(f, payload).shadow);
    }
    world.same_calls = 0;
    end_ref(f, make_shadow_of(f, sample(30)).shadow);
    require(world.same_calls == 1 && counters().blocks_collected == 0, "nothing was collected: one test, of the sentinel");
    collect();
    const uint32_t expected[4] = {32, 64, 96, 102};  // the blocks that ended after the collection are tested, not freed
    for (int step = 0; step < 4; ++step) {
        world.same_calls = 0;
        std::vector<uint8_t> payload = sample(40);
        payload[0] = uint8_t(step);
        end_ref(f, make_shadow_of(f, payload).shadow);
        require(world.same_calls <= 1 + 32 + 1, "at most the sentinel and 32 blocks are tested at a creation");
        require(counters().blocks_collected == expected[step], "32 blocks at a creation, until the end of the list");
    }
    world.same_calls = 0;
    end_ref(f, make_shadow_of(f, sample(50)).shadow);
    require(world.same_calls == 1, "and then one test again, of the new sentinel");
}

// The end is a synchronization point: a raw write followed by a frame end reaches the array.
void check_the_end_is_a_synchronization_point() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    require(push(f.env, 16) == 0, "a frame");
    Made made = make_held(f, sample(9000));
    raw_of(made)[7] = 0x71;
    raw_of(made)[5000] = 0x72;
    require(made.object->bytes[7] != 0x71, "Java has not seen the writes");
    pop(f.env, nullptr);
    require(is_ended_block(made.shadow), "the frame's end ended the block");
    require(made.object->bytes[7] == 0x71 && made.object->bytes[5000] == 0x72, "the writes reached the array at the end");
    // The same at a native method's return.
    void* function = app_function(1);
    const Native native = thunk_of(function);
    require(enter_native(native, function, 0x1234, 0x8000), "enter");
    Made second = make_held(f, sample(100));
    raw_of(second)[3] = 0x73;
    leave_native(0x8000);
    require(is_ended_block(second.shadow) && second.object->bytes[3] == 0x73, "and at a return");
}

// The same array again: the same address, and Java's write to one chunk and the raw write to another both survive.
void check_the_same_array_again_gets_its_block() {
    Fixture f = make();
    Made first = make_held(f, sample(9000));
    const void* p = first.shadow;
    uint8_t* raw = raw_of(first);
    end_ref(f, first.shadow);
    first.object->bytes[3] = 0xaa;  // Java writes to chunk 0 meanwhile
    raw[16 + 5000 - 16] = 0;        // (no write: the block is only read here)
    raw[5000] = 0xbb;               // the application writes through its kept pointer to chunk 1
    Made again = obtain_again(f, first.object);
    require(again.shadow == p && is_shadow_block(again.shadow), "the same array gets its block back: the pointer it kept");
    require(counters().blocks_revived == 1 && counters().shadows_created == 1 && counters().shadows_ended == 0 &&
                counters().shadows_live == 1 && world.globals.size() == 1 && block_weaks() == 0,
            "a revival: no new block, a global reference again, the weak one gone");
    require(raw_of(again)[3] == 0xaa, "Java's write to chunk 0 reached the block");
    require(first.object->bytes[5000] == 0xbb, "the raw write to chunk 1 reached the array");
    require(raw_of(again)[5000] == 0xbb && first.object->bytes[3] == 0xaa, "and both survive on both sides");
    end_ref(f, again.shadow);
    require(is_ended_block(p), "it ends again");
}

// An exception pending at a frame end: the end is deferred, no call the runtime forbids is made.
void check_an_exception_pending_defers_a_frame_end() {
    Fixture f = make();
    auto push = fn<jint (*)(Env*, jint)>(*f.table, slot::PushLocalFrame);
    auto pop = fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame);
    require(push(f.env, 16) == 0, "a frame");
    Made made = make_held(f, sample(100));
    raw_of(made)[1] = 0x44;
    world.exception_pending = true;
    pop(f.env, nullptr);
    require(world.exception_pending && is_shadow_block(made.shadow) && world.globals.size() == 1 && block_weaks() == 0 &&
                made.object->bytes[1] != 0x44,
            "the block waits, strong, with the raw write not yet pushed; the fake enforced the rules of CheckJNI");
    world.exception_pending = false;
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(is_ended_block(made.shadow) && made.object->bytes[1] == 0x44 && world.globals.empty() && block_weaks() == 1,
            "the next call without an exception finished the end");
}

// A thread that ends, and DetachCurrentThread: the blocks end, they are not freed.
void check_thread_end_and_detach_end_blocks() {
    Fixture f = make();
    struct Fns {
        static jint detach(Vm*) { return 0; }
        static jint other(Vm*, void**, jint) { return 0; }
    };
    void* vm_table[kVmSlotCount];
    for (void*& entry : vm_table) entry = reinterpret_cast<void*>(&Fns::other);
    vm_table[vmslot::DetachCurrentThread] = reinterpret_cast<void*>(&Fns::detach);
    require(install_vm_wrappers(vm_table, kPlatform), "the invoke table installs");
    Vm vm{vm_table};
    const void* attached_block = nullptr;
    const void* ended_thread_block = nullptr;
    std::thread attached([&] {
        Made made = make_held(f, sample(64));
        attached_block = made.shadow;
        raw_of(made)[0] = 0x66;
        reinterpret_cast<jint (*)(Vm*)>(vm_table[vmslot::DetachCurrentThread])(&vm);
        require(is_ended_block(attached_block) && made.object->bytes[0] == 0x66, "detach ended the block and pushed the write");
        Made other = make_held(f, sample(65));
        ended_thread_block = other.shadow;
        release_thread_state(t_block);  // the thread ends with no detach
    });
    attached.join();
    require(is_shadow_block(ended_thread_block), "the block of a thread that ended waits for an environment, strong");
    jobject plain = byte_array(sample(4));
    fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, plain);
    require(is_ended_block(ended_thread_block) && is_ended_block(attached_block) && counters().shadows_ended == 2 &&
                counters().shadows_live == 0,
            "and the next call with an environment ended it: both are ended, not freed");
}

// The arena is full of ended blocks whose arrays Java holds: one Runtime.gc(), a decline with the line, no second gc
// within 10 s, and success once the arrays are gone.
void check_an_arena_full_of_ended_blocks() {
    Fixture f = make();
    std::vector<Made> kept;
    for (int i = 0; i < 100; ++i) {
        Made made = make_held(f, std::vector<uint8_t>(100000, uint8_t(i)));
        if (!is_shadow_block(made.shadow)) break;  // the region is full
        kept.push_back(made);
        end_ref(f, made.shadow);
    }
    require(kept.size() >= 10 && kept.size() < 100, "the region held some blocks and then no more");
    const uint32_t ended = counters().shadows_ended;
    require(counters().shadows_declined == 1 && ended == kept.size(), "the first that did not fit was declined");
    require(world.gc_calls == 1 && counters().collections_asked == 1, "after one Runtime.gc()");
    require(log_count("did not fit: the runtime was asked to collect") == 1 && log_count("stayed a real reference (no room") == 1 &&
                log_count("waiting for their arrays' collection") == 1,
            "the lines: the collection, the decline, the ended blocks waiting");
    for (const auto& line : world.logs) {
        if (line.message.find("waiting for their arrays' collection") != std::string::npos) {
            require(line.a == long(ended) && line.b == long(ended) * 100000, "with the ended blocks and bytes");
        }
    }
    world.clock_ms += 9000;
    Made again = make_shadow_of(f, std::vector<uint8_t>(100000, 0x55));
    require(!is_shadow_block(again.shadow) && world.gc_calls == 1, "no second collection within 10 s");
    world.clock_ms += 1100;
    Made third = make_shadow_of(f, std::vector<uint8_t>(100000, 0x56));
    require(!is_shadow_block(third.shadow) && world.gc_calls == 2, "a second one after 10 s: the arrays are still held");
    for (const Made& made : kept) made.object->held = false;
    world.clock_ms += 11000;
    Made fourth = make_shadow_of(f, std::vector<uint8_t>(100000, 0x57));
    require(is_shadow_block(fourth.shadow) && world.gc_calls == 3, "and when the arrays are not held, a collection makes room");
    require(counters().shadows_ended < ended, "the blocks of the gone arrays were freed");
}

// The sweep that makes room needs no sentinel: arrays the runtime cleared without the sentinel having been cleared
// are found by the full sweep, with no call to the runtime.
void check_the_full_sweep_does_not_need_the_sentinel() {
    Fixture f = make();
    std::vector<Made> kept;
    for (int i = 0; i < 100; ++i) {
        Made made = make_held(f, std::vector<uint8_t>(100000, uint8_t(i)));
        if (!is_shadow_block(made.shadow)) break;
        kept.push_back(made);
        end_ref(f, made.shadow);
    }
    require(kept.size() >= 10 && kept.size() < 32 && world.gc_calls == 1, "the region is full of ended blocks");
    for (const Made& made : kept) {
        for (Ref* weak : world.weaks) {
            if (weak->object == made.object) weak->object = nullptr;  // the runtime cleared these and not the sentinel
        }
    }
    world.clock_ms += 1000;
    Made fresh = make_shadow_of(f, std::vector<uint8_t>(100000, 0x77));
    require(is_shadow_block(fresh.shadow), "room was made");
    require(world.gc_calls == 1 && counters().blocks_collected == kept.size(), "by the full sweep, with no call to the runtime");
}

// The cap on ended blocks: 16384; a full sweep, a collection, then the result stays the real reference.
void check_the_cap_of_ended_blocks() {
    g_region = g_big_region;  // 16384 blocks do not fit in the small region
    g_region_size = sizeof(g_big_region);
    Fixture f = make();
    g_region = g_shadow_region;
    g_region_size = sizeof(g_shadow_region);
    std::vector<Obj*> kept;
    for (unsigned i = 0; i < 16384; ++i) {
        Made made = make_held(f, std::vector<uint8_t>());
        require(is_shadow_block(made.shadow), "a block up to the cap");
        kept.push_back(made.object);
        end_ref(f, made.shadow);
    }
    require(counters().shadows_ended == 16384 && counters().shadows_declined == 0, "16384 ended blocks");
    Made over = make_shadow_of(f, sample(10));
    require(!is_shadow_block(over.shadow) && counters().shadows_declined == 1 && world.gc_calls == 1,
            "the next result stayed the real reference, after one collection");
    require(log_count("too many ended blocks") == 1, "and the line names the cap");
    for (Obj* object : kept) object->held = false;
    world.clock_ms += 11000;
    Made after = make_shadow_of(f, sample(10));
    require(is_shadow_block(after.shadow) && counters().shadows_ended < 16384, "with the arrays gone a block fits again");
}

// 64 arrays of one length that Java keeps, each obtained 1000 times in turn: 64 blocks, 64 addresses, never a second
// block, no decline.
void check_arrays_that_java_keeps_are_obtained_again_and_again() {
    Fixture f = make();
    std::vector<Obj*> objects;
    std::vector<const void*> addresses;
    for (int i = 0; i < 64; ++i) {
        std::vector<uint8_t> payload = sample(200);
        payload[0] = uint8_t(i);
        Made made = make_held(f, payload);
        objects.push_back(made.object);
        addresses.push_back(made.shadow);
        end_ref(f, made.shadow);
    }
    require(std::set<const void*>(addresses.begin(), addresses.end()).size() == 64, "64 addresses");
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 64; ++i) {
            Made again = obtain_again(f, objects[size_t(i)]);
            require(again.shadow == addresses[size_t(i)], "each array always gets its own block");
            end_ref(f, again.shadow);
        }
    }
    require(counters().shadows_created == 64 && counters().shadows_declined == 0 && counters().blocks_revived == 64000 &&
                counters().shadows_ended == 64 && counters().shadows_live == 0,
            "64 blocks, 64000 revivals, no second block, no decline");
}

// Arrays with the same identity hash are told apart by identity.
void check_arrays_with_equal_hashes_get_their_own_blocks() {
    Fixture f = make();
    world.same_hash = true;
    Made a = make_held(f, sample(100));
    Made b = make_held(f, sample(100));
    const void* pa = a.shadow;
    const void* pb = b.shadow;
    require(pa != pb, "two blocks");
    end_ref(f, a.shadow);
    end_ref(f, b.shadow);
    Made b_again = obtain_again(f, b.object);
    Made a_again = obtain_again(f, a.object);
    require(b_again.shadow == pb && a_again.shadow == pa, "each gets its own block back, however the hashes compare");
    Made c = make_shadow_of(f, sample(100));
    require(c.shadow != pa && c.shadow != pb && counters().shadows_created == 3, "a third array gets a third block");
}

// Two ended blocks for one array (it was live on two threads): the newest is found.
void check_the_newest_ended_block_of_an_array_is_found() {
    Fixture f = make();
    Made first = make_held(f, sample(100));
    const void* first_at = first.shadow;
    const void* second_at = nullptr;
    std::thread other([&] {
        Made second = obtain_again(f, first.object);  // live on the first thread: this thread gets a block of its own
        second_at = second.shadow;
        end_ref(f, second.shadow);
    });
    other.join();
    require(second_at != nullptr && second_at != first_at, "a block of its own on the other thread");
    end_ref(f, first.shadow);  // the first block ended last: it is the newest
    Made again = obtain_again(f, first.object);
    require(again.shadow == first_at, "the newest ended block is the one found");
    end_ref(f, again.shadow);
}

// Where the identity hash cannot be had, a block is made as before and nothing is revived.
void check_no_identity_hash_means_no_revival() {
    reset_world();
    world.find_fails = true;
    Table* table = new Table;
    require(install_jni_wrappers(table->slots, kPlatform, g_shadow_region, sizeof(g_shadow_region)), "installs");
    Fixture f;
    f.table = table;
    f.env = &table->env;
    f.cls = new_local(new Obj{});
    f.get_bytes = fn<jmethodID (*)(Env*, jclass, const char*, const char*)>(*f.table, slot::GetStaticMethodID)(
        f.env, f.cls, "bytes", "()[B");
    Made made = make_held(f, sample(100));
    require(is_shadow_block(made.shadow) && !world.exception_pending, "a block, and the failed lookup left no exception");
    end_ref(f, made.shadow);
    Made again = obtain_again(f, made.object);
    require(is_shadow_block(again.shadow) && counters().blocks_revived == 0 && counters().shadows_created == 2,
            "the same array gets a second block");
}

// No weak reference to give: the block goes at its end, as before.
void check_no_weak_reference_frees_the_block() {
    Fixture f = make();
    Made made = make_held(f, sample(100));
    world.weak_fails = true;
    end_ref(f, made.shadow);
    require(!is_shadow_block(made.shadow) && !is_ended_block(made.shadow) && world.globals.empty() &&
                counters().shadows_live == 0 && counters().shadows_ended == 0,
            "the block was freed, with its global reference");
}

// Raw access to an ended block reads the bytes as of its end, and Java's later writes do not reach it.
void check_an_ended_block_does_not_see_javas_later_writes() {
    Fixture f = make();
    Made made = make_held(f, sample(100));
    const uint8_t first = raw_of(made)[10];
    end_ref(f, made.shadow);
    made.object->bytes[10] = uint8_t(first + 1);
    require(raw_of(made)[10] == first, "the pointer still reads the bytes as of the end");
    raw_of(made)[11] = 0x5e;  // a raw write with nothing to carry it: the stated limit
    require(made.object->bytes[11] != 0x5e, "and a raw write does not reach Java until the array is obtained again");
}


// ---- pins, the references of ended blocks, and the merge (the review round of the block ends)

using SetRegion = void (*)(Env*, jarray, jsize, jsize, const jbyte*);
jbyte* get_elements(Fixture& f, jobject a) { return fn<GetElems>(*f.table, slot::GetByteArrayElements)(f.env, a, nullptr); }
void release_elements(Fixture& f, jobject a, const void* p, jint mode) {
    fn<void (*)(Env*, jarray, const void*, jint)>(*f.table, slot::ReleaseByteArrayElements)(f.env, a, p, mode);
}
void* get_critical(Fixture& f, jobject a) {
    return fn<void* (*)(Env*, jarray, jboolean*)>(*f.table, slot::GetPrimitiveArrayCritical)(f.env, a, nullptr);
}
void release_critical(Fixture& f, jobject a, void* p, jint mode) {
    fn<void (*)(Env*, jarray, void*, jint)>(*f.table, slot::ReleasePrimitiveArrayCritical)(f.env, a, p, mode);
}
// The object address of the block whose data starts at `p`.
const uint8_t* object_at(const void* p) { return static_cast<const uint8_t*>(p) - 16; }
jsize length_of(Fixture& f, jobject a) { return fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, a); }

// A pointer from Get...Elements that is never released keeps the array reachable and the block valid, as Dalvik's pin
// did: no collection takes it and no later block takes its address.
void check_a_pointer_never_released_keeps_its_array() {
    Fixture f = make();
    const std::vector<uint8_t> payload = sample(4096);
    Made kept = make_shadow_of(f, payload);  // Java holds nothing of this array
    jbyte* p = get_elements(f, kept.shadow);
    require(reinterpret_cast<uint8_t*>(p) == raw_of(kept), "the pointer is the block's bytes");
    end_ref(f, kept.shadow);  // the native method returns; the pointer is never released
    require(is_ended_block(object_at(p)) && counters().shadows_ended == 1 && world.globals.size() == 1 && block_weaks() == 0,
            "the block ended, still held by a strong reference");
    collect();
    for (int i = 0; i < 100; ++i) {
        std::vector<uint8_t> other = sample(4096);
        other[0] = uint8_t(i + 1);
        Made temporary = make_shadow_of(f, other);
        require(temporary.shadow != kept.shadow, "no new block takes the kept pointer's address");
        end_ref(f, temporary.shadow);
        if (i % 10 == 9) collect();
    }
    require(is_ended_block(object_at(p)) && std::memcmp(p, payload.data(), payload.size()) == 0, "the kept pointer still reads its bytes");
    // The array obtained again is the block, with its pin; the release through it ends the pin, and then the array can go.
    Made again = obtain_again(f, kept.object);
    require(again.shadow == kept.shadow && is_shadow_block(again.shadow), "the same array gets its block back");
    release_elements(f, again.shadow, p, 0);
    end_ref(f, again.shadow);
    require(block_weaks() >= 1 && is_ended_block(object_at(p)), "unpinned, the block ended weakly like any other");
    const uint32_t freed = counters().blocks_collected;
    collect();
    for (int i = 0; i < 40 && counters().blocks_collected == freed + 0; ++i) {
        end_ref(f, make_shadow_of(f, sample(10)).shadow);  // a creation tests ended blocks
    }
    require(counters().blocks_collected > freed, "and a collection freed it");
}

// A Release goes to the block its pointer lies in, through whichever reference it arrives, and never to the runtime;
// COMMIT keeps the pin, mode 0 and ABORT end it; the last pin of an ended block through a critical release waits for a sweep.
void check_a_release_goes_to_the_block_its_pointer_lies_in() {
    Fixture f = make();
    Made m = make_held(f, sample(100));
    jbyte* p = get_elements(f, m.shadow);
    Ref* real = new_local(m.object);  // another reference to the same array
    p[0] = 0x44;
    world.release_calls = 0;
    release_elements(f, real, p, kReleaseCommit);
    require(world.release_calls == 0, "a release by a real reference with the block's pointer is not the runtime's");
    require(m.object->bytes[0] == 0x44, "it synchronized the block");
    end_ref(f, m.shadow);
    require(is_ended_block(object_at(p)) && world.globals.size() == 1, "COMMIT did not end the pin: the ended block is held strongly");
    release_elements(f, real, p, 0);
    require(world.release_calls == 0 && world.globals.empty() && block_weaks() == 1,
            "mode 0 through the real reference ended the pin of the ended block: it waits weakly now");
    // ABORT, a stale reference of the ended block, and a pointer that is a freed block's
    Made n = make_held(f, sample(100));
    jbyte* q = get_elements(f, n.shadow);
    end_ref(f, n.shadow);
    release_elements(f, n.shadow, q, kReleaseAbort);
    require(world.release_calls == 0 && block_weaks() == 2, "ABORT through the ended block's own reference ended the pin too");
    n.object->held = false;
    m.object->held = false;
    end_ref(f, real);  // the plain reference to the array goes too
    const uint32_t freed = counters().blocks_collected;
    collect();
    for (int i = 0; i < 40 && counters().blocks_collected < freed + 2; ++i) end_ref(f, make_shadow_of(f, sample(10)).shadow);
    require(counters().blocks_collected >= freed + 2, "the blocks were freed");
    Ref* plain = new_local(m.object);
    release_elements(f, plain, q, 0);
    require(world.release_calls == 0, "a freed block's pointer is not the runtime's either");
    // the critical pair: the last pin of an ended block waits for a sweep
    Made c = make_shadow_of(f, sample(100));
    void* r = get_critical(f, c.shadow);
    end_ref(f, c.shadow);
    world.released.clear();
    release_critical(f, new_local(c.object), r, 0);
    require(world.critical == 0 && is_ended_block(object_at(r)), "a critical release by a real reference: the block's");
    for (const void* released : world.released) {
        require(released != r, "the runtime was never handed the block's pointer");
    }
    const size_t strong = world.globals.size();
    collect();
    end_ref(f, make_shadow_of(f, sample(10)).shadow);  // a creation after a collection sweeps, and the unpinned block weakens
    require(world.globals.size() < strong, "the sweep gave the unpinned block's strong reference up");
}

// The reference of an ended block works in every wrapper (an application keeps a byte[] in a C global across native
// calls): the block lives again, with an entry on the calling thread. A release, a delete and the critical pair do not
// revive it. The reference of a block whose array was collected converts to nothing.
void check_the_reference_of_an_ended_block_keeps_working() {
    for (int with_live = 0; with_live < 2; ++with_live) {
        Fixture f = make();
        const std::vector<uint8_t> payload = sample(100);
        Made other = with_live ? make_held(f, sample(50)) : Made{nullptr, nullptr};
        Made kept = make_held(f, payload);
        end_ref(f, kept.shadow);
        require(is_ended_block(kept.shadow) && !is_shadow_block(kept.shadow), "the block ended");
        const uint32_t revived = counters().blocks_revived;

        require(length_of(f, kept.shadow) == 100 && counters().blocks_revived == revived, "the length, without a revival");
        jbyte* p = get_elements(f, kept.shadow);
        require(p == reinterpret_cast<jbyte*>(raw_of(kept)) && std::memcmp(p, payload.data(), 100) == 0, "the elements are the block's");
        require(is_shadow_block(kept.shadow) && counters().blocks_revived == revived + 1, "the reference revived the block");
        release_elements(f, kept.shadow, p, 0);
        end_ref(f, kept.shadow);
        require(is_ended_block(kept.shadow), "it ended again");

        jbyte buffer[10] = {};
        fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, kept.shadow, 5, 10, buffer);
        require(std::memcmp(buffer, payload.data() + 5, 10) == 0, "a region read has the bytes");
        const jbyte patch[3] = {1, 2, 3};
        fn<SetRegion>(*f.table, slot::SetByteArrayRegion)(f.env, kept.shadow, 0, 3, patch);
        require(kept.object->bytes[0] == 1 && kept.object->bytes[2] == 3 && raw_of(kept)[1] == 2, "a region write reaches the array");
        end_ref(f, kept.shadow);

        require(call_static_int_v(f, f.consume, kept.shadow) == 7 && world.last_array_args.size() == 1 &&
                    world.last_array_args[0] == kept.object,
                "a call is handed the real array");
        end_ref(f, kept.shadow);
        world.next_result = nullptr;  // the call returns nothing
        call_object_v(f, kept.shadow, f.instance_bytes);
        require(world.seen_receiver.size() == 100, "and so is the receiver of an instance call");
        end_ref(f, kept.shadow);

        require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, kept.shadow, kept.shadow) == 1,
                "IsSameObject");
        end_ref(f, kept.shadow);
        jobject global = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, kept.shadow);
        require(global != nullptr && object_of(global) == kept.object, "NewGlobalRef is a real reference to the array");
        fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteGlobalRef)(f.env, global);
        end_ref(f, kept.shadow);
        world.next_result = nullptr;
        fn<void (*)(Env*, jobject, jfieldID, jobject)>(*f.table, set_field_slot(kObject))(f.env, f.cls, f.bytes_field, kept.shadow);
        end_ref(f, kept.shadow);

        // Not revived: a delete, a release, the critical pair.
        const uint32_t revivals = counters().blocks_revived;
        world.release_calls = 0;
        end_ref(f, kept.shadow);
        release_elements(f, kept.shadow, payload.data(), 0);
        require(get_critical(f, kept.shadow) == nullptr, "the critical entry asks nothing of an ended reference");
        require(counters().blocks_revived == revivals && is_ended_block(kept.shadow) && world.release_calls == 0,
                "none of them revived it, and none reached the runtime");
        (void)other;
    }

    // A block whose array was collected: the reference converts to nothing, and nothing in the layer crashes.
    Fixture f = make();
    Made gone = make_shadow_of(f, sample(100));
    end_ref(f, gone.shadow);
    collect();
    require(length_of(f, gone.shadow) == 100, "the length is the block's");
    require(get_elements(f, gone.shadow) == nullptr, "no elements: the array is gone");
    jbyte buffer[4] = {1, 1, 1, 1};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, gone.shadow, 0, 4, buffer);
    require(buffer[0] == 1, "a region read copies nothing");
    require(call_static_int_v(f, f.consume, gone.shadow) == 7 && world.last_array_args.size() == 1 &&
                world.last_array_args[0] == nullptr,
            "a call is handed null");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, gone.shadow, nullptr) == 1,
            "and the reference is the null reference");
}

// The merge is byte by byte in every synchronization, the end and the revival included: a byte Java wrote in a chunk the
// application also wrote survives.
void check_a_merge_is_byte_exact_across_an_end() {
    Fixture f = make();
    Made made = make_held(f, std::vector<uint8_t>(8192, 0x10));
    raw_of(made)[20] = 0xB2;        // the application, through the pointer
    made.object->bytes[10] = 0xA1;  // Java, in the same chunk
    end_ref(f, made.shadow);
    require(made.object->bytes[10] == 0xA1 && made.object->bytes[20] == 0xB2, "the end merged both into the array");
    Made again = obtain_again(f, made.object);
    require(again.shadow == made.shadow && raw_of(again)[10] == 0xA1 && raw_of(again)[20] == 0xB2, "and the revived block has both");
    raw_of(again)[30] = 0xC3;
    made.object->bytes[31] = 0xC4;
    void* pinned = get_critical(f, again.shadow);
    require(made.object->bytes[30] == 0xC3 && made.object->bytes[31] == 0xC4 && raw_of(again)[31] == 0xC4,
            "the critical pair merges the same way");
    release_critical(f, again.shadow, pinned, kReleaseAbort);
    // A copy of the elements the application wrote into and released: only the bytes it changed.
    jbyte* elements = get_elements(f, again.shadow);
    made.object->bytes[40] = 0xD0;  // Java, after the elements were taken
    std::vector<uint8_t> copy(elements, elements + 8192);
    copy[50] = 0xD1;
    release_elements(f, again.shadow, copy.data(), 0);
    require(made.object->bytes[50] == 0xD1 && made.object->bytes[40] == 0xD0, "a release of a copy: its changed byte, and Java's");
}

// Declined results do not sweep again and again: a full sweep needs a collection since the last one, or a second.
void check_declined_results_do_not_repeat_a_full_sweep() {
    Fixture f = make();
    std::vector<Made> kept;
    for (int i = 0; i < 100; ++i) {
        Made made = make_held(f, std::vector<uint8_t>(100000, uint8_t(i)));
        if (!is_shadow_block(made.shadow)) break;
        kept.push_back(made);
        end_ref(f, made.shadow);
    }
    require(kept.size() >= 10 && world.gc_calls == 1 && counters().shadows_declined == 1, "the region is full of ended blocks");
    int before = world.same_calls;
    for (int i = 0; i < 20; ++i) {
        require(!is_shadow_block(make_shadow_of(f, std::vector<uint8_t>(100000, uint8_t(i))).shadow), "declined");
    }
    require(world.same_calls - before <= 3 * 20, "no full sweep: no more than the tests of the sentinel for each");
    world.clock_ms += 1000;  // the fallback that does not depend on the sentinel
    before = world.same_calls;
    make_shadow_of(f, std::vector<uint8_t>(100000, 1));
    require(world.same_calls - before >= int(kept.size()), "after a second, one full sweep");
    before = world.same_calls;
    make_shadow_of(f, std::vector<uint8_t>(100000, 2));
    require(world.same_calls - before <= 4, "and only one");
    collect();
    before = world.same_calls;
    make_shadow_of(f, std::vector<uint8_t>(100000, 3));
    require(world.same_calls - before >= int(kept.size()), "a collection since the last one makes another at once");
}

// A second thread that obtains the array while an end runs gets the block the end makes, not a block of its own: the weak
// reference and the index entry come in one step under the lock.
void check_an_end_and_a_second_thread_make_one_block() {
    Fixture f = make();
    Made made = make_held(f, sample(100));
    Obj* object = made.object;
    std::atomic<bool> done{false};
    const void* other_at = nullptr;
    std::thread other;
    g_on_global_deleted = [&](Obj* gone) {  // inside the end: the strong reference goes, the block is not in the index yet
        if (gone != object || other.joinable()) return;
        world.next_result = new_local(object);
        other = std::thread([&] {
            other_at = call_static_object_v(f, f.get_bytes);
            done = true;
        });
        for (int i = 0; i < 300 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    end_ref(f, made.shadow);
    g_on_global_deleted = nullptr;
    other.join();
    require(other_at == made.shadow && counters().shadows_created == 1, "the other thread got the block back");
    end_ref(f, const_cast<void*>(other_at));
}

// The same for the ends that waited for an environment (a thread that ended, an exception): the exchange and the ends are
// one step.
void check_draining_the_ends_and_a_second_thread_make_one_block() {
    Fixture f = make();
    Obj* object = nullptr;
    const void* first_at = nullptr;
    std::thread owner([&] {
        Made made = make_held(f, sample(100));
        object = made.object;
        first_at = made.shadow;
        release_thread_state(t_block);  // the thread ends with no detach: its block waits for an environment
    });
    owner.join();
    std::atomic<bool> done{false};
    const void* other_at = nullptr;
    std::thread other;
    g_on_global_deleted = [&](Obj* gone) {
        if (gone != object || other.joinable()) return;
        world.next_result = new_local(object);
        other = std::thread([&] {
            other_at = call_static_object_v(f, f.get_bytes);
            done = true;
        });
        for (int i = 0; i < 300 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    jobject plain = byte_array(sample(4));
    length_of(f, plain);  // the next call with an environment drains
    g_on_global_deleted = nullptr;
    if (other.joinable()) other.join();
    require(other_at == first_at && counters().shadows_created == 1, "the other thread got the block back");
}

// The lookup of the classes runs Java and may enter the layer again, on the same thread: it holds no lock of the layer.
void check_the_class_lookup_may_enter_the_layer() {
    Fixture f = make();
    bool inner_block = false;
    g_on_find_class = [&] {
        Made inner = make_shadow_of(f, sample(10));
        inner_block = is_shadow_block(inner.shadow);
        end_ref(f, inner.shadow);
    };
    Made outer = make_held(f, sample(20));
    require(inner_block && is_shadow_block(outer.shadow), "both results are blocks");
    end_ref(f, outer.shadow);
    Made again = obtain_again(f, outer.object);
    require(again.shadow == outer.shadow, "and the lookup finished: the array is found again");
}


void check_install_rules() {
    reset_world();
    Table table;
    g_writable_ok = false;
    require(!install_jni_wrappers(table.slots, kPlatform, g_shadow_region, sizeof(g_shadow_region)),
            "a table that cannot be written is left alone");
    require(table.slots[slot::GetArrayLength] == reinterpret_cast<void*>(&f_GetArrayLength), "unchanged");
    g_writable_ok = true;
    g_read_only_ok = false;
    require(install_jni_wrappers(table.slots, kPlatform, g_shadow_region, sizeof(g_shadow_region)),
            "a table that cannot be made read-only again is still installed (and reported)");
    g_read_only_ok = true;
    require(!install_jni_wrappers(table.slots, kPlatform, g_shadow_region, sizeof(g_shadow_region)), "installing twice is refused");
    require(table.slots[slot::GetArrayLength] != reinterpret_cast<void*>(&f_GetArrayLength), "wrapped");
    require(table.slots[slot::MonitorEnter] != reinterpret_cast<void*>(&f_MonitorEnter) &&
                table.slots[slot::MonitorExit] != reinterpret_cast<void*>(&f_MonitorExit),
            "MonitorEnter and MonitorExit are wrapped");
    require(table.slots[slot::NewStringUTF] == reinterpret_cast<void*>(&trap) && table.slots[slot::FindClass] == reinterpret_cast<void*>(&f_FindClass) &&
                table.slots[slot::NewByteArray] == reinterpret_cast<void*>(&f_NewByteArray),
            "the functions that only made references the layer tracked are no longer wrapped");
    // VM wrappers ask the platform who is calling: that is the "a library may have loaded" notification.
    void* vm_table[kVmSlotCount] = {};
    struct VmFns {
        static jint get_env(Vm*, void**, jint) { return 0; }
    };
    for (void*& s : vm_table) s = reinterpret_cast<void*>(&VmFns::get_env);
    require(install_vm_wrappers(vm_table, kPlatform), "the invoke table installs");
    const int before = world.app_checks;
    Vm vm{vm_table};
    void* out;
    require(reinterpret_cast<jint (*)(Vm*, void**, jint)>(vm_table[vmslot::GetEnv])(&vm, &out, 0x10006) == 0, "GetEnv passes through");
    require(world.app_checks == before + 1, "and asks who the caller is");
}

void check_no_shadows_when_disabled() {
    Fixture f = make(/*shadows=*/false);
    const bool nothing = f.table->slots[slot::GetArrayLength] == reinterpret_cast<void*>(&f_GetArrayLength);
    require(nothing, "with no region the table is left as it was");
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: oldapp_jni_tests <jni-slots.tsv>");
    check_slot_constants(argv[1]);
    check_shadow_layout_and_use();
    check_identity_and_writes();
    check_platform_caller_sees_nothing();
    check_shadow_passed_back();
    check_conversion_for_any_caller();
    check_monitor();
    check_unknown_and_long_signatures();
    check_fields_and_other_results();
    check_block_follows_a_call();
    check_block_follows_later_writes();
    check_abort_gives_the_arrays_bytes_back();
    check_exception_leaves_the_block();
    check_raw_write_reaches_a_call();
    check_raw_write_and_java_write_in_one_call();
    check_both_sides_changed_one_chunk_both_survive();
    check_the_merge_by_words_is_byte_exact();
    check_synchronization_is_per_chunk();
    check_a_clean_block_makes_no_array_write();
    check_set_region_keeps_other_raw_writes();
    check_an_exact_comparison_keeps_every_write();
    check_a_run_of_chunks_is_one_region_call();
    check_committing_release_keeps_java_bytes_in_clean_chunks();
    check_nested_critical_release_keeps_the_other_pointers_bytes();
    check_a_receiver_is_synchronized();
    check_one_block_per_array();
    check_a_stray_length_write_is_harmless();
    check_the_flag_of_the_elements_is_truthful();
    check_the_flag_of_the_critical_pointer_is_truthful();
    check_no_synchronization_with_an_exception_pending();
    check_critical_pair_inside_a_region();
    check_critical_entries_do_not_drain();
    check_limits_without_recycling();
    check_diagnosis_lines();
    check_thread_end();
    check_pop_local_frame();
    check_a_native_invocation_ends_its_blocks();
    check_nested_invocations_end_their_own_blocks();
    check_a_native_method_that_returns_with_an_exception_pending();
    check_six_hundred_invocations_decline_nothing();
    check_more_than_sixty_four_nested_invocations();
    check_no_thread_state_no_record();
    check_an_abandoned_invocation_is_trimmed();
    check_an_unmatched_push_local_frame_ends_with_the_invocation();
    check_the_thunks_run_out();
    check_a_method_without_a_thunk_ends_by_the_other_ends();
    check_which_functions_get_a_thunk();
    check_register_natives_registers_thunks();
    check_detach_ends_the_references_of_a_thread();
    check_push_and_pop_local_frame_end_their_entries();
    check_frames_beyond_the_records();
    check_delete_local_ref_keeps_the_frames_right();
    check_the_thunk_diagnosis_lines();
    check_a_kept_pointer_keeps_its_bytes();
    check_a_collection_frees_ended_blocks();
    check_a_sweep_takes_thirty_two_blocks_at_a_creation();
    check_the_end_is_a_synchronization_point();
    check_the_same_array_again_gets_its_block();
    check_an_exception_pending_defers_a_frame_end();
    check_thread_end_and_detach_end_blocks();
    check_an_arena_full_of_ended_blocks();
    check_the_full_sweep_does_not_need_the_sentinel();
    check_the_cap_of_ended_blocks();
    check_arrays_that_java_keeps_are_obtained_again_and_again();
    check_arrays_with_equal_hashes_get_their_own_blocks();
    check_the_newest_ended_block_of_an_array_is_found();
    check_no_identity_hash_means_no_revival();
    check_no_weak_reference_frees_the_block();
    check_an_ended_block_does_not_see_javas_later_writes();
    check_a_pointer_never_released_keeps_its_array();
    check_a_release_goes_to_the_block_its_pointer_lies_in();
    check_the_reference_of_an_ended_block_keeps_working();
    check_a_merge_is_byte_exact_across_an_end();
    check_declined_results_do_not_repeat_a_full_sweep();
    check_an_end_and_a_second_thread_make_one_block();
    check_draining_the_ends_and_a_second_thread_make_one_block();
    check_the_class_lookup_may_enter_the_layer();
    check_install_rules();
    check_no_shadows_when_disabled();
    std::puts("old-application JNI: ok");
    return 0;
}

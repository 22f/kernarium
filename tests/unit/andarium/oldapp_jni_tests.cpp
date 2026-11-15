// SPDX-License-Identifier: GPL-2.0-or-later
//
// The old-application layer's JNI interposer (android/lineage-24/overlay/oldapp/jni_shim.cpp)
// against a fake JNI function table that accepts only live references: the slot constants against
// the specification's order (argv[1], tests/unit/andarium/fixtures/jni-slots.tsv), byte[] shadows
// with Dalvik's layout and everything an application can then do with one, the identity and writes
// of the array a shadow stands for, what the runtime is handed in place of a shadow (for any caller,
// whatever the method), the limits (nothing in use is recycled), a thread that ends, PopLocalFrame,
// and the calls of the platform's own libraries, which must see no shadow.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
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
};
struct Ref {
    Obj* object;
    bool global;
};

struct Method {
    std::string sig;
};

struct World {
    std::set<Ref*> locals;
    std::set<Ref*> globals;
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
    return world.locals.count(r) != 0 || world.globals.count(r) != 0;
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

bool fake_is_app(const void*) {
    ++world.app_checks;
    return g_app_caller;
}
thread_local void* t_block = nullptr;
thread_local uint64_t t_epoch = 0;
void* fake_thread_state(size_t size) {
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
const Platform kPlatform{fake_is_app, fake_thread_state, fake_log, fake_make_writable, fake_make_read_only,
                         fake_describe, fake_now, fake_alloc_baseline, fake_free_baseline};

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
void f_ReleaseByteArrayElements(Env*, jarray a, jbyte*, jint) { (void)object_of(a); }
void* f_GetPrimitiveArrayCritical(Env*, jarray a, jboolean* is_copy) {
    if (is_copy) *is_copy = 0;
    ++world.critical;
    return object_of(a)->bytes.data();
}
void f_ReleasePrimitiveArrayCritical(Env*, jarray a, void*, jint) {
    (void)object_of(a);
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
    Ref* ref = new Ref{object_of(o), true};
    world.globals.insert(ref);
    return ref;
}
jobject f_NewWeakGlobalRef(Env*, jobject o) { return f_NewGlobalRef(nullptr, o); }
void f_DeleteGlobalRef(Env*, jobject o) {
    jni_allowed("DeleteGlobalRef where JNI is not allowed");
    Ref* ref = static_cast<Ref*>(o);
    require(world.globals.erase(ref) == 1, "a global reference is deleted once, and only while it is live");
    delete ref;
}
jobject f_NewLocalRef(Env*, jobject o) { return new_local(object_of(o)); }
jboolean f_IsSameObject(Env*, jobject a, jobject b) {
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
jboolean f_ExceptionCheck(Env*) { return world.exception_pending; }
jobject f_FindClass(Env*, const char*) { return new_local(new Obj{}); }
jmethodID f_GetMethodID(Env*, jclass, const char*, const char* sig) {
    void* id = new int(0);
    world.methods[id] = Method{sig};
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
    record("static-int-A", m, decode_array(world.methods.at(m).sig, a));
    return 7;
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
        slots[slot::DeleteGlobalRef] = reinterpret_cast<void*>(&f_DeleteGlobalRef);
        slots[slot::NewLocalRef] = reinterpret_cast<void*>(&f_NewLocalRef);
        slots[slot::IsSameObject] = reinterpret_cast<void*>(&f_IsSameObject);
        slots[slot::GetObjectClass] = reinterpret_cast<void*>(&f_GetObjectClass);
        slots[slot::IsInstanceOf] = reinterpret_cast<void*>(&f_IsInstanceOf);
        slots[slot::MonitorEnter] = reinterpret_cast<void*>(&f_MonitorEnter);
        slots[slot::MonitorExit] = reinterpret_cast<void*>(&f_MonitorExit);
        slots[slot::PushLocalFrame] = reinterpret_cast<void*>(&f_PushLocalFrame);
        slots[slot::PopLocalFrame] = reinterpret_cast<void*>(&f_PopLocalFrame);
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

template <class F>
F fn(const Table& table, int slot) {
    return reinterpret_cast<F>(table.slots[slot]);
}

void reset_world() {
    reset_for_test();
    for (Ref* r : world.locals) delete r;
    for (Ref* r : world.globals) delete r;
    world = World{};
    ++g_epoch;
    g_app_caller = true;
    g_writable_ok = true;
    g_read_only_ok = true;
    g_baseline_fails = false;
}

// Installs over a fresh fake table.
Table* install(bool shadows = true) {
    reset_world();
    Table* table = new Table;
    require(install_jni_wrappers(table->slots, kPlatform, shadows ? g_shadow_region : nullptr,
                                 shadows ? sizeof(g_shadow_region) : 0),
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
    expect("ExceptionClear", slot::ExceptionClear);
    expect("PushLocalFrame", slot::PushLocalFrame);
    expect("PopLocalFrame", slot::PopLocalFrame);
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

Fixture make(bool shadows = true) {
    Fixture f;
    f.table = install(shadows);
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
    require(counters().shadows_live == 0 && world.globals.empty(), "DeleteLocalRef freed the block and its global reference");
    require(!is_shadow_block(shadow), "a freed block is no shadow");
    require(fn<jsize (*)(Env*, jarray)>(*f.table, slot::GetArrayLength)(f.env, shadow) == 0, "a freed block has no length");
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
    world.methods[unknown] = Method{"(I[B)V"};
    call_static_void_v(f, unknown, 5, made.shadow);
    require(world.describe_calls == 1, "the signature of an unknown ID is asked for once");
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == made.object, "and the shadow still became its array");
    jvalue args[2];
    args[0].i = 1;
    args[1].l = made.shadow;
    fn<void (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kVoid, kArray))(f.env, f.cls, unknown, args);
    require(world.describe_calls == 1 && world.last_array_args[0] == made.object, "the answer is kept (array form)");

    void* hopeless = new int(0);
    world.methods[hopeless] = Method{"(I)V"};
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

// Both sides changed the same chunk since the last synchronization: the application's bytes win for the chunk.
void check_both_sides_changed_the_native_bytes_win() {
    Fixture f = make();
    Made made = make_shadow_of(f, std::vector<uint8_t>(8, 0x10));
    uint8_t* raw = raw_of(made);
    raw[0] = 0xAA;                // the application, through the pointer
    made.object->bytes[1] = 0xBB;  // Java, in the same chunk
    jbyte buffer[8] = {};
    fn<GetRegion>(*f.table, slot::GetByteArrayRegion)(f.env, made.shadow, 0, 8, buffer);
    require(static_cast<uint8_t>(buffer[0]) == 0xAA && made.object->bytes[0] == 0xAA, "the application's byte reached the array");
    require(static_cast<uint8_t>(buffer[1]) == 0x10 && made.object->bytes[1] == 0x10,
            "Java's byte in that chunk is replaced by the block's");
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

// A run of chunks that are alike is one region call, and a clean chunk is never written.
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
    require(world.get_calls == 1, "and the six chunks came back in one");
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
    check_both_sides_changed_the_native_bytes_win();
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
    check_install_rules();
    check_no_shadows_when_disabled();
    std::puts("old-application JNI: ok");
    return 0;
}

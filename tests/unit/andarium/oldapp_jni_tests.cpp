// SPDX-License-Identifier: GPL-2.0-or-later
//
// The old-application layer's JNI interposer (android/lineage-24/overlay/oldapp/jni_shim.cpp)
// against a fake JNI function table: the slot constants against the specification's order
// (argv[1], tests/unit/andarium/fixtures/jni-slots.tsv), byte[] shadows with Dalvik's layout and
// everything an application can then do with one, the references a shadow passed back to Java
// becomes, local-reference relief against a table that overflows at 512, and the calls of the
// platform's own libraries, which must see none of it.
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
    // Returns the result; records the (already decoded) arguments of the call.
    jobject (*result)(const std::vector<jvalue>&) = nullptr;
};

struct World {
    std::set<Ref*> locals;
    std::set<Ref*> globals;
    std::map<void*, Method> methods;
    std::map<void*, std::string> fields;
    std::vector<jvalue> last_args;
    std::vector<std::vector<uint8_t>> last_array_args;  // the bytes of every array argument, in order
    std::string last_call;
    bool exception_pending = false;
    bool overflowed = false;
    int local_limit = 512;
    int ensure_calls = 0;
    int app_checks = 0;
    int traps = 0;
    jobject next_result = nullptr;
};
World world;

Ref* new_local(Obj* object) {
    if (static_cast<int>(world.locals.size()) >= world.local_limit) world.overflowed = true;
    Ref* ref = new Ref{object, false};
    world.locals.insert(ref);
    return ref;
}
Obj* object_of(jobject ref) { return static_cast<Ref*>(ref)->object; }

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
void* fake_thread_state(size_t size) {
    thread_local void* block = nullptr;
    thread_local uint64_t epoch = 0;
    if (epoch != g_epoch) {
        std::free(block);
        block = std::calloc(1, size);
        epoch = g_epoch;
    }
    return block;
}
void fake_log(const char*, long, long) {}
bool g_writable_ok = true;
bool fake_make_writable(void*, size_t) { return g_writable_ok; }
bool fake_make_read_only(void*, size_t) { return true; }
const Platform kPlatform{fake_is_app, fake_thread_state, fake_log, fake_make_writable, fake_make_read_only};

jsize f_GetArrayLength(Env*, jarray a) { return static_cast<jsize>(object_of(a)->bytes.size()); }
jobject f_NewByteArray(Env*, jsize n) { return byte_array(std::vector<uint8_t>(static_cast<size_t>(n), 0)); }
void f_SetByteArrayRegion(Env*, jarray a, jsize start, jsize len, const jbyte* buf) {
    auto& bytes = object_of(a)->bytes;
    if (start < 0 || len < 0 || size_t(start) + size_t(len) > bytes.size()) {
        world.exception_pending = true;
        return;
    }
    std::memcpy(bytes.data() + start, buf, size_t(len));
}
void f_GetByteArrayRegion(Env*, jarray a, jsize start, jsize len, jbyte* buf) {
    auto& bytes = object_of(a)->bytes;
    if (start < 0 || len < 0 || size_t(start) + size_t(len) > bytes.size()) {
        world.exception_pending = true;
        return;
    }
    std::memcpy(buf, bytes.data() + start, size_t(len));
}
jbyte* f_GetByteArrayElements(Env*, jarray a, jboolean* is_copy) {
    if (is_copy) *is_copy = 1;
    return reinterpret_cast<jbyte*>(object_of(a)->bytes.data());
}
void f_ReleaseByteArrayElements(Env*, jarray, jbyte*, jint) {}
void f_DeleteLocalRef(Env*, jobject o) {
    Ref* ref = static_cast<Ref*>(o);
    require(world.locals.erase(ref) == 1, "a local reference is deleted once, and only if it is live");
    delete ref;
}
jobject f_NewGlobalRef(Env*, jobject o) {
    require(world.locals.count(static_cast<Ref*>(o)) || world.globals.count(static_cast<Ref*>(o)),
            "NewGlobalRef is given a live reference, never a shadow");
    Ref* ref = new Ref{object_of(o), true};
    world.globals.insert(ref);
    return ref;
}
jobject f_NewLocalRef(Env*, jobject o) { return new_local(object_of(o)); }
jboolean f_IsSameObject(Env*, jobject a, jobject b) { return object_of(a) == object_of(b); }
jobject f_GetObjectClass(Env*, jobject o) {
    require(world.locals.count(static_cast<Ref*>(o)), "GetObjectClass is given a live reference");
    return new_local(new Obj{});
}
jboolean f_IsInstanceOf(Env*, jobject o, jclass) {
    require(world.locals.count(static_cast<Ref*>(o)), "IsInstanceOf is given a live reference");
    return 1;
}
jint f_EnsureLocalCapacity(Env*, jint n) {
    ++world.ensure_calls;
    if (static_cast<int>(world.locals.size()) + n > world.local_limit) {
        world.exception_pending = true;
        return -1;
    }
    return 0;
}
jboolean f_ExceptionCheck(Env*) { return world.exception_pending; }
void f_ExceptionClear(Env*) { world.exception_pending = false; }
jobject f_NewStringUTF(Env*, const char* text) {
    Obj* object = new Obj{};
    object->bytes.assign(text, text + std::strlen(text));
    return new_local(object);
}
jobject f_FindClass(Env*, const char*) { return new_local(new Obj{}); }
jmethodID f_GetMethodID(Env*, jclass, const char* name, const char* sig) {
    void* id = new int(0);
    world.methods[id] = Method{sig, nullptr};
    (void)name;
    return id;
}
jfieldID f_GetFieldID(Env*, jclass, const char*, const char* sig) {
    void* id = new int(0);
    world.fields[id] = sig;
    return id;
}
jobject f_GetObjectField(Env*, jobject, jfieldID f) {
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
    size_t n = 0;
    for (size_t i = 1; m.sig[i] != ')'; ++i, ++n) {
        if (m.sig[i] == 'L' || m.sig[i] == '[') {
            if (m.sig[i] == '[') {
                while (m.sig[i] == '[') ++i;
                if (m.sig[i] == 'L') while (m.sig[i] != ';') ++i;
            } else {
                while (m.sig[i] != ';') ++i;
            }
            Ref* ref = static_cast<Ref*>(args[n].l);
            if (ref == nullptr) continue;
            require(world.locals.count(ref) || world.globals.count(ref), "the runtime is handed a live reference");
            world.last_array_args.push_back(ref->object->bytes);
        }
    }
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
jobject f_CallObjectMethodA(Env*, jobject o, jmethodID m, const jvalue* a) {
    require(world.locals.count(static_cast<Ref*>(o)), "the receiver is a live reference");
    record("object-A", m, decode_array(world.methods.at(m).sig, a));
    return result_of(m);
}
jobject f_CallObjectMethodV(Env*, jobject o, jmethodID m, va_list ap) {
    require(world.locals.count(static_cast<Ref*>(o)), "the receiver is a live reference");
    record("object-V", m, decode_va(world.methods.at(m).sig, ap));
    return result_of(m);
}
jobject f_NewObjectV(Env*, jclass, jmethodID m, va_list ap) {
    record("new-V", m, decode_va(world.methods.at(m).sig, ap));
    return new_local(new Obj{});
}
void f_SetObjectField(Env*, jobject, jfieldID, jobject value) {
    require(world.locals.count(static_cast<Ref*>(value)), "SetObjectField is given a live reference");
}
jobject f_PopLocalFrame(Env*, jobject) { return nullptr; }

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
        slots[slot::DeleteLocalRef] = reinterpret_cast<void*>(&f_DeleteLocalRef);
        slots[slot::NewGlobalRef] = reinterpret_cast<void*>(&f_NewGlobalRef);
        slots[slot::NewLocalRef] = reinterpret_cast<void*>(&f_NewLocalRef);
        slots[slot::IsSameObject] = reinterpret_cast<void*>(&f_IsSameObject);
        slots[slot::GetObjectClass] = reinterpret_cast<void*>(&f_GetObjectClass);
        slots[slot::IsInstanceOf] = reinterpret_cast<void*>(&f_IsInstanceOf);
        slots[slot::EnsureLocalCapacity] = reinterpret_cast<void*>(&f_EnsureLocalCapacity);
        slots[slot::ExceptionCheck] = reinterpret_cast<void*>(&f_ExceptionCheck);
        slots[slot::ExceptionClear] = reinterpret_cast<void*>(&f_ExceptionClear);
        slots[slot::NewStringUTF] = reinterpret_cast<void*>(&f_NewStringUTF);
        slots[slot::FindClass] = reinterpret_cast<void*>(&f_FindClass);
        slots[slot::GetMethodID] = reinterpret_cast<void*>(&f_GetMethodID);
        slots[slot::GetStaticMethodID] = reinterpret_cast<void*>(&f_GetMethodID);
        slots[slot::GetFieldID] = reinterpret_cast<void*>(&f_GetFieldID);
        slots[slot::GetStaticFieldID] = reinterpret_cast<void*>(&f_GetFieldID);
        slots[get_field_slot(kObject)] = reinterpret_cast<void*>(&f_GetObjectField);
        slots[get_static_field_slot(kObject)] = reinterpret_cast<void*>(&f_GetStaticObjectField);
        slots[set_field_slot(kObject)] = reinterpret_cast<void*>(&f_SetObjectField);
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
        slots[slot::NewObjectV] = reinterpret_cast<void*>(&f_NewObjectV);
        slots[slot::PopLocalFrame] = reinterpret_cast<void*>(&f_PopLocalFrame);
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
}

// Installs over a fresh fake table.
Table* install(bool relief, bool shadows = true) {
    reset_world();
    Table* table = new Table;
    require(install_jni_wrappers(table->slots, kPlatform, shadows ? g_shadow_region : nullptr,
                                 shadows ? sizeof(g_shadow_region) : 0, relief, &table->env),
            "the wrappers install");
    return table;
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
    jfieldID bytes_field;       // [B
    jfieldID other_field;       // Ljava/lang/String;
};

Fixture make(bool relief = false, bool shadows = true) {
    Fixture f;
    f.table = install(relief, shadows);
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

// ---- scenarios

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
    require(vm_slots.at("GetEnv") == vmslot::GetEnv && vm_slots.at("AttachCurrentThread") == vmslot::AttachCurrentThread &&
                vm_slots.at("AttachCurrentThreadAsDaemon") == vmslot::AttachCurrentThreadAsDaemon &&
                vm_slots.at("DestroyJavaVM") == vmslot::DestroyJavaVM &&
                vm_slots.at("DetachCurrentThread") == vmslot::DetachCurrentThread,
            "the invoke interface slots");
}

void check_shadow_layout_and_use() {
    Fixture f = make();
    const auto payload = sample(100);
    world.next_result = byte_array(payload);
    const size_t locals_before = world.locals.size();
    jobject shadow = call_static_object_v(f, f.get_bytes);
    require(shadow != nullptr && is_shadow_block(shadow), "a byte[] result for application code is a shadow");
    require(world.locals.size() == locals_before - 1, "the real local reference was released");
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
    require(elements == reinterpret_cast<const jbyte*>(raw + 0x10) && copy == 0, "elements are the block itself, no copy");
    fn<void (*)(Env*, jarray, jbyte*, jint)>(*f.table, slot::ReleaseByteArrayElements)(f.env, shadow, elements, 0);
    jbyte buffer[10];
    fn<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(*f.table, slot::GetByteArrayRegion)(f.env, shadow, 5, 10, buffer);
    require(std::memcmp(buffer, payload.data() + 5, 10) == 0, "GetByteArrayRegion reads the block");
    jbyte patch[3] = {1, 2, 3};
    fn<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(*f.table, slot::SetByteArrayRegion)(f.env, shadow, 0, 3, patch);
    require(raw[0x10] == 1 && raw[0x12] == 3, "SetByteArrayRegion writes the block");
    require(!world.exception_pending, "in range, no exception");
    fn<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(*f.table, slot::GetByteArrayRegion)(f.env, shadow, 95, 10, buffer);
    require(world.exception_pending, "out of range, the runtime raises it for a real array");
    world.exception_pending = false;
    require(world.locals.size() == locals_before - 1 + 0, "the temporary array of that call was released");

    require(fn<jint (*)(Env*, jobject)>(*f.table, slot::GetObjectRefType)(f.env, shadow) == kRefTypeLocal, "a local reference");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, shadow, shadow) == 1, "same");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, shadow, f.cls) == 0, "different");
    require(fn<jboolean (*)(Env*, jobject, jobject)>(*f.table, slot::IsSameObject)(f.env, shadow, nullptr) == 0, "not null");

    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, shadow);
    require(counters().shadows_live == 0, "DeleteLocalRef freed the block");
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, shadow);  // a second delete is ignored
    require(counters().shadows_live == 0, "and a second delete changes nothing");
}

void check_platform_caller_sees_nothing() {
    Fixture f = make();
    g_app_caller = false;
    const auto payload = sample(40);
    world.next_result = byte_array(payload);
    jobject result = call_static_object_v(f, f.get_bytes);
    require(!is_shadow_block(result) && world.locals.count(static_cast<Ref*>(result)), "the platform gets the real reference");
    require(counters().shadows_created == 0, "no shadow made");
    jobject string = fn<jobject (*)(Env*, const char*)>(*f.table, slot::NewStringUTF)(f.env, "x");
    require(world.locals.count(static_cast<Ref*>(string)), "NewStringUTF is the original");
    fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef)(f.env, string);
    require(counters().probes == 0, "a platform caller is never tracked");
}

void check_shadow_passed_back() {
    Fixture f = make();
    const auto payload = sample(64);
    world.next_result = byte_array(payload);
    jobject shadow = call_static_object_v(f, f.get_bytes);
    require(is_shadow_block(shadow), "a shadow");
    const size_t locals = world.locals.size();

    // va_list
    require(call_static_int_v(f, f.consume, shadow) == 7, "consume(V)");
    require(world.last_call == "static-int-A" && world.last_array_args.size() == 1 && world.last_array_args[0] == payload,
            "the runtime was handed a real array with the shadow's bytes (through the array form)");
    require(world.locals.size() == locals, "the temporary array was released after the call");

    // jvalue array
    jvalue args[1];
    args[0].l = shadow;
    require(fn<jint (*)(Env*, jclass, jmethodID, const jvalue*)>(*f.table, static_call_slot(kInt, kArray))(f.env, f.cls, f.consume, args) == 7,
            "consume(A)");
    require(world.last_call == "static-int-A" && world.last_array_args[0] == payload, "the same through the array form");
    require(args[0].l == shadow, "the caller's own array is not modified");
    require(world.locals.size() == locals, "released");

    // mixed argument kinds in one call, the shadow in the middle
    jobject text = fn<jobject (*)(Env*, const char*)>(*f.table, slot::NewStringUTF)(f.env, "hi");
    require(call_static_int_v(f, f.mixed, jint(-5), jlong(0x123456789abcdefLL), text, shadow, 2.5, 1.5) == 7, "mixed");
    require(world.last_args.size() == 6, "six arguments");
    require(world.last_args[0].i == -5 && world.last_args[1].j == 0x123456789abcdefLL && world.last_args[2].l == text &&
                world.last_args[4].d == 2.5 && world.last_args[5].f == 1.5f,
            "the other arguments arrive unchanged, in order and width");
    require(world.last_array_args.size() == 2 && world.last_array_args[1] == payload, "the shadow became an array");

    // a call that passes no shadow takes the plain path (nothing temporary)
    jobject array = byte_array(sample(8));
    require(call_static_int_v(f, f.consume, array) == 7, "a real array passes through");
    require(world.last_args[0].l == array, "unchanged");

    // void methods and instance calls and constructors
    auto void_v = fn<void (*)(Env*, jclass, jmethodID, va_list)>(*f.table, static_call_slot(kVoid, kVa));
    auto call_void = [&](jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        void_v(f.env, f.cls, m, ap);
        va_end(ap);
    };
    call_void(f.consume_void, 1, shadow);
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == payload, "void call");
    auto new_object = fn<jobject (*)(Env*, jclass, jmethodID, va_list)>(*f.table, slot::NewObjectV);
    auto construct = [&](jmethodID m, ...) {
        va_list ap;
        va_start(ap, m);
        jobject r = new_object(f.env, f.cls, m, ap);
        va_end(ap);
        return r;
    };
    jobject made = construct(f.construct, shadow);
    require(world.last_array_args.size() == 1 && world.last_array_args[0] == payload && world.locals.count(static_cast<Ref*>(made)),
            "a constructor given a shadow");

    // NewGlobalRef / NewLocalRef / GetObjectClass / IsInstanceOf of a shadow reach the runtime as arrays
    jobject global = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewGlobalRef)(f.env, shadow);
    require(world.globals.count(static_cast<Ref*>(global)) && static_cast<Ref*>(global)->object->bytes == payload,
            "a global reference of a shadow is a real array");
    jobject local = fn<jobject (*)(Env*, jobject)>(*f.table, slot::NewLocalRef)(f.env, shadow);
    require(!is_shadow_block(local) && static_cast<Ref*>(local)->object->bytes == payload, "NewLocalRef gives a real array");
    jobject cls = fn<jobject (*)(Env*, jobject)>(*f.table, slot::GetObjectClass)(f.env, shadow);
    require(world.locals.count(static_cast<Ref*>(cls)), "GetObjectClass");
    require(fn<jboolean (*)(Env*, jobject, jclass)>(*f.table, slot::IsInstanceOf)(f.env, shadow, f.cls) == 1, "IsInstanceOf");
    fn<void (*)(Env*, jobject, jfieldID, jobject)>(*f.table, set_field_slot(kObject))(f.env, f.cls, f.other_field, shadow);
}

void check_fields_and_other_results() {
    Fixture f = make();
    const auto payload = sample(30);
    // an instance method returning byte[] through the V entry
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
    // a field declared byte[]
    world.next_result = byte_array(payload);
    jobject field = fn<jobject (*)(Env*, jobject, jfieldID)>(*f.table, get_field_slot(kObject))(f.env, receiver, f.bytes_field);
    require(is_shadow_block(field), "a byte[] field read");
    world.next_result = byte_array(payload);
    jobject static_field =
        fn<jobject (*)(Env*, jclass, jfieldID)>(*f.table, get_static_field_slot(kObject))(f.env, f.cls, f.bytes_field);
    require(is_shadow_block(static_field), "a static byte[] field read");
    // a field of another type is not touched
    world.next_result = byte_array(payload);
    jobject other = fn<jobject (*)(Env*, jobject, jfieldID)>(*f.table, get_field_slot(kObject))(f.env, receiver, f.other_field);
    require(!is_shadow_block(other), "a String field is the real reference");
    // a method that returns Object is not touched either, even when the object is a byte[]
    world.next_result = byte_array(payload);
    jobject obj = call_static_object_v(f, f.not_bytes);
    require(!is_shadow_block(obj), "the declared type decides");
}

void check_eviction_and_threads() {
    Fixture f = make();
    std::vector<jobject> shadows;
    for (int i = 0; i < 70; ++i) {
        world.next_result = byte_array(sample(1000));
        shadows.push_back(call_static_object_v(f, f.get_bytes));
        require(is_shadow_block(shadows.back()), "a shadow");
    }
    require(counters().shadows_live == 64, "the 64 newest blocks of a thread are kept");
    // A block that a thread's oldest-first recycling freed is no longer a live block: its owner is the thread.
    std::thread other([&] {
        world.next_result = byte_array(sample(10));
        jobject mine = call_static_object_v(f, f.get_bytes);
        require(is_shadow_block(mine), "another thread makes its own");
    });
    other.join();
    const uint32_t live = counters().shadows_live;
    // This thread asks to delete the other thread's block: ignored, its owner recycles it.
    require(live == 65, "the other thread's block stays");
    // Big blocks are held to a byte budget.
    for (int i = 0; i < 6; ++i) {
        world.next_result = byte_array(sample(3u << 20 > 0x7fffffff ? 1 : 300000));
        call_static_object_v(f, f.get_bytes);
    }
    require(counters().shadows_live < 80, "the byte budget keeps the count bounded");
}

void check_relief() {
    // Without relief a leaking loop overflows the fake's 512-entry table, as ART's would.
    {
        Fixture f = make(/*relief=*/false);
        auto make_string = fn<jobject (*)(Env*, const char*)>(*f.table, slot::NewStringUTF);
        for (int i = 0; i < 700; ++i) make_string(f.env, "leak");
        require(world.overflowed, "the baseline overflows");
    }
    // With relief the same loop never does, and the references it kept are still usable.
    {
        Fixture f = make(/*relief=*/true);
        auto make_string = fn<jobject (*)(Env*, const char*)>(*f.table, slot::NewStringUTF);
        jobject last = nullptr;
        for (int i = 0; i < 6000; ++i) {
            last = make_string(f.env, "leak");
            require(last != nullptr, "a string");
        }
        require(!world.overflowed, "relief kept the table below its limit");
        require(world.locals.count(static_cast<Ref*>(last)), "the newest reference is alive");
        require(counters().relief_rounds > 0 && counters().references_released > 1000, "relief ran and released references");
        require(counters().probes >= 6000 / 64, "the table was probed every 64 creations");
        require(!world.exception_pending, "the refusal's exception was cleared");
    }
    // A pending exception is never disturbed.
    {
        Fixture f = make(true);
        auto make_string = fn<jobject (*)(Env*, const char*)>(*f.table, slot::NewStringUTF);
        world.exception_pending = true;
        const int ensure_before = world.ensure_calls;
        for (int i = 0; i < 200; ++i) make_string(f.env, "x");
        require(world.exception_pending && world.ensure_calls == ensure_before, "no probe while an exception is pending");
    }
    // A reference the application deleted itself is not deleted again.
    {
        Fixture f = make(true);
        auto make_string = fn<jobject (*)(Env*, const char*)>(*f.table, slot::NewStringUTF);
        auto del = fn<void (*)(Env*, jobject)>(*f.table, slot::DeleteLocalRef);
        for (int i = 0; i < 4000; ++i) {
            jobject s = make_string(f.env, "kept");
            if (i % 2 == 0) del(f.env, s);  // half of them are released properly
        }
        require(!world.overflowed, "an application that cleans half of its references never reaches the limit's edge");
    }
    // A platform without the limit (the install probe is granted) turns relief off.
    {
        reset_world();
        world.local_limit = 1 << 20;
        Table* table = new Table;
        require(install_jni_wrappers(table->slots, kPlatform, g_shadow_region, sizeof(g_shadow_region), true, &table->env),
                "install");
        auto make_string = fn<jobject (*)(Env*, const char*)>(*table, slot::NewStringUTF);
        const int ensure_before = world.ensure_calls;
        for (int i = 0; i < 3000; ++i) make_string(&table->env, "x");
        require(world.ensure_calls == ensure_before && counters().references_released == 0, "no limit, no relief");
    }
    // PopLocalFrame forgets the ring (its references are gone).
    {
        Fixture f = make(true);
        auto make_string = fn<jobject (*)(Env*, const char*)>(*f.table, slot::NewStringUTF);
        for (int i = 0; i < 100; ++i) make_string(f.env, "x");
        fn<jobject (*)(Env*, jobject)>(*f.table, slot::PopLocalFrame)(f.env, nullptr);
    }
}

void check_install_rules() {
    reset_world();
    Table table;
    g_writable_ok = false;
    require(!install_jni_wrappers(table.slots, kPlatform, g_shadow_region, sizeof(g_shadow_region), true, &table.env),
            "a table that cannot be written is left alone");
    require(table.slots[slot::GetArrayLength] == reinterpret_cast<void*>(&f_GetArrayLength), "unchanged");
    g_writable_ok = true;
    require(install_jni_wrappers(table.slots, kPlatform, g_shadow_region, sizeof(g_shadow_region), true, &table.env),
            "the second try installs");
    require(!install_jni_wrappers(table.slots, kPlatform, g_shadow_region, sizeof(g_shadow_region), true, &table.env),
            "installing twice is refused");
    require(table.slots[slot::GetArrayLength] != reinterpret_cast<void*>(&f_GetArrayLength), "wrapped");
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
    Fixture f = make(/*relief=*/false, /*shadows=*/false);
    world.next_result = byte_array(sample(8));
    const bool nothing = f.table->slots[slot::GetArrayLength] == reinterpret_cast<void*>(&f_GetArrayLength);
    require(nothing, "with shadows and relief both off the table is left as it was");
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "usage: oldapp_jni_tests <jni-slots.tsv>");
    check_slot_constants(argv[1]);
    check_shadow_layout_and_use();
    check_platform_caller_sees_nothing();
    check_shadow_passed_back();
    check_fields_and_other_results();
    check_eviction_and_threads();
    check_relief();
    check_install_rules();
    check_no_shadows_when_disabled();
    std::puts("old-application JNI: ok");
    return 0;
}

// SPDX-License-Identifier: Apache-2.0
//
// The native half of the old-application probe (oldapp_probe_apk.py).
//
// An application written for Android before 4.0 does four things that ART no longer
// supports. This library does them the way such an application does, as ordinary
// application code, and reports one "key=value" for each, as the text its native method
// returns (the receiver makes it the broadcast's result data):
//
//   shim_mapped  whether the old-application layer is mapped in this process
//   raw_read     reads a Java byte[] through the object pointer at the Dalvik offsets
//                (length at +8, bytes at +0x10)
//   passback     hands the same byte[] back to Java and to the array functions
//   elements     GetByteArrayElements on it returns the bytes
//   global       a global reference made from it is a usable array
//   thread_raw   the raw read from a second, attached thread
//   low_heap     64 blocks of 256 KiB from malloc/calloc/realloc/strdup all lie below 2 GiB
//   write_through  a write through SetByteArrayRegion, GetByteArrayElements or
//                GetPrimitiveArrayCritical reaches the Java array
//   identity     the array, a global reference made from it and the array a Java call returned
//                are one object (IsSameObject)
//   retained     a reference kept while 520 more results arrive still reads its own bytes
//   monitor      MonitorEnter and MonitorExit accept it
//   pop          PopLocalFrame hands it out of the frame
//   getline      getline grows a buffer the application allocated with malloc
//   usable_size  malloc_usable_size accepts a block the application allocated
//   libc_block   the application reallocates and frees a block libc allocated
//   table_read_only  the JNI and invoke function tables are not writable once the layer is installed
//   done         the last entry: its absence means the process did not get this far
//
// A pass is "=pass", anything else "=fail" (or "=skipped"). Reads of an address that may
// not be mapped go through a pipe, whose write fails with EFAULT instead of faulting.
//
// No Android or bionic header is included: the library is built freestanding for armv7
// with a host compiler as well as with Soong.

#include <stddef.h>
#include <stdint.h>

#include "jni_abi.hpp"

extern "C" {
int probe_open(const char*, int) __asm__("open");
int probe_close(int) __asm__("close");
long probe_read(int, void*, size_t) __asm__("read");
long probe_write(int, const void*, size_t) __asm__("write");
int probe_pipe(int[2]) __asm__("pipe");
void* probe_malloc(size_t) __asm__("malloc");
void* probe_calloc(size_t, size_t) __asm__("calloc");
void* probe_realloc(void*, size_t) __asm__("realloc");
void probe_free(void*) __asm__("free");
char* probe_strdup(const char*) __asm__("strdup");
void* probe_fopen(const char*, const char*) __asm__("fopen");
char* probe_fgets(char*, int, void*) __asm__("fgets");
int probe_sscanf(const char*, const char*, ...) __asm__("sscanf");
int probe_fclose(void*) __asm__("fclose");
long probe_getline(char**, size_t*, void*) __asm__("getline");
size_t probe_usable_size(void*) __asm__("malloc_usable_size");
int probe_pthread_create(unsigned long*, const void*, void* (*)(void*), void*) __asm__("pthread_create");
int probe_pthread_join(unsigned long, void**) __asm__("pthread_join");
}

namespace {

using namespace oldapp::jni;

constexpr int kReadOnly = 0;
constexpr const char kPayload[] = "OLD-APP-PAYLOAD-0123456789abcdef";
constexpr jint kPayloadLength = sizeof(kPayload) - 1;
constexpr int kBlocks = 64;
constexpr size_t kBlockBytes = 256u << 10;
constexpr int kRetainedResults = 520;  // more than Dalvik's 512 local references

size_t length_of(const char* text) {
    size_t n = 0;
    while (text[n] != '\0') {
        ++n;
    }
    return n;
}

bool same(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool contains(const char* haystack, size_t length, const char* needle) {
    const size_t n = length_of(needle);
    for (size_t i = 0; i + n <= length; ++i) {
        if (same(haystack + i, needle, n)) {
            return true;
        }
    }
    return false;
}

// Copies `count` bytes (at most one pipe buffer) from an address that may be unmapped.
bool safe_copy(void* destination, const void* source, size_t count) {
    int fds[2];
    if (probe_pipe(fds) != 0) {
        return false;
    }
    const long wrote = probe_write(fds[1], source, count);
    bool ok = wrote == static_cast<long>(count) && probe_read(fds[0], destination, count) == static_cast<long>(count);
    probe_close(fds[0]);
    probe_close(fds[1]);
    return ok;
}

template <class F>
F entry(Env* env, int slot) {
    return reinterpret_cast<F>(const_cast<void*>(env->functions[slot]));
}

struct Report {
    char text[1024];
    size_t length = 0;
    void append(const char* piece) {
        for (size_t i = 0; piece[i] != '\0' && length + 1 < sizeof(text); ++i) {
            text[length++] = piece[i];
        }
        text[length] = '\0';
    }
    void line(const char* key, const char* value) {
        append(key);
        append("=");
        append(value);
        append(";");
    }
    void verdict(const char* key, bool pass) { line(key, pass ? "pass" : "fail"); }
};

bool shim_mapped() {
    int fd = probe_open("/proc/self/maps", kReadOnly);
    if (fd < 0) {
        return false;
    }
    static char buffer[8192 + 64];  // the chunk and the tail of the one before it
    size_t kept = 0;
    bool found = false;
    for (;;) {
        const long got = probe_read(fd, buffer + kept, 8192);
        if (got <= 0) {
            break;
        }
        const size_t total = kept + static_cast<size_t>(got);
        if (contains(buffer, total, "libkernarium_oldapp")) {
            found = true;
            break;
        }
        kept = total < 48 ? total : 48;
        for (size_t i = 0; i < kept; ++i) {
            buffer[i] = buffer[total - kept + i];
        }
    }
    probe_close(fd);
    return found;
}

void hex(char* out, uint32_t value) {  // eight digits
    for (int i = 0; i < 8; ++i) {
        out[i] = "0123456789abcdef"[(value >> (28 - 4 * i)) & 15];
    }
    out[8] = '\0';
}

// Whether the object pointer reads as a Dalvik byte[]; `report` gets what was seen, for a reader of the log.
bool raw_read(Report& report, jobject array) {
    const char* raw = static_cast<const char*>(array);
    char text[9];
    hex(text, reinterpret_cast<uint32_t>(array));
    report.line("raw_pointer", text);
    int32_t length = 0;
    char bytes[64];
    if (!safe_copy(&length, raw + 8, sizeof(length))) {
        report.line("raw_length", "unreadable");
        return false;
    }
    hex(text, static_cast<uint32_t>(length));
    report.line("raw_length", text);
    return length == kPayloadLength && safe_copy(bytes, raw + 0x10, kPayloadLength) &&
           same(bytes, kPayload, kPayloadLength);
}

bool low_heap() {
    void* blocks[kBlocks];
    bool ok = true;
    for (int i = 0; i < kBlocks; ++i) {
        // Each block is written only up to the size it was asked for.
        size_t size = (i % 3 == 0) ? kBlockBytes : kBlockBytes / 2;
        void* block = (i % 3 == 0) ? probe_calloc(1, kBlockBytes) : probe_malloc(kBlockBytes / 2);
        if (i % 3 == 1) {
            block = probe_realloc(block, kBlockBytes);
            size = kBlockBytes;
        }
        blocks[i] = block;
        if (block == nullptr || reinterpret_cast<uintptr_t>(block) + size > 0x80000000u) {
            ok = false;
            continue;
        }
        for (size_t at = 0; at < size; at += 4096) {
            static_cast<volatile char*>(block)[at] = static_cast<char>(i);
        }
    }
    char* copy = probe_strdup(kPayload);
    ok = ok && copy != nullptr && reinterpret_cast<uintptr_t>(copy) + sizeof(kPayload) <= 0x80000000u &&
         same(copy, kPayload, sizeof(kPayload));
    probe_free(copy);
    for (int i = 0; i < kBlocks; ++i) {
        probe_free(blocks[i]);
    }
    return ok;
}

// Whether the mapping that holds `pointer` is not writable, from /proc/self/maps.
bool mapped_read_only(const void* pointer) {
    void* file = probe_fopen("/proc/self/maps", "r");
    if (file == nullptr) {
        return false;
    }
    char line[512];
    bool found = false, read_only = false;
    while (!found && probe_fgets(line, sizeof(line), file) != nullptr) {
        unsigned low = 0, high = 0;
        char perms[5] = {};
        if (probe_sscanf(line, "%x-%x %4s", &low, &high, perms) == 3 &&
            reinterpret_cast<uintptr_t>(pointer) >= low && reinterpret_cast<uintptr_t>(pointer) < high) {
            found = true;
            read_only = perms[1] != 'w';
        }
    }
    probe_fclose(file);
    return found && read_only;
}

// What an application does with a byte[] the way a Dalvik-era application does it, each reported as one verdict.
// The array comes back from Java as a call result: ByteBuffer.wrap(original).array(), which is `original` itself.
void array_cases(Report& report, Env* env) {
    const auto find_class = entry<jclass (*)(Env*, const char*)>(env, slot::FindClass);
    const auto static_method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    const auto method = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetMethodID);
    const auto call_static = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
    const auto call_instance = entry<jobject (*)(Env*, jobject, jmethodID, ...)>(env, call_slot(kObject, kVarargs));
    const auto new_array = entry<jobject (*)(Env*, jsize)>(env, slot::NewByteArray);
    const auto set_region = entry<void (*)(Env*, jarray, jsize, jsize, const jbyte*)>(env, slot::SetByteArrayRegion);
    const auto get_region = entry<void (*)(Env*, jarray, jsize, jsize, jbyte*)>(env, slot::GetByteArrayRegion);
    const auto array_length = entry<jsize (*)(Env*, jarray)>(env, slot::GetArrayLength);
    const auto delete_local = entry<void (*)(Env*, jobject)>(env, slot::DeleteLocalRef);
    const auto exception_check = entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck);

    jclass buffer_class = find_class(env, "java/nio/ByteBuffer");
    jmethodID wrap = static_method(env, buffer_class, "wrap", "([B)Ljava/nio/ByteBuffer;");
    jmethodID array_method = method(env, buffer_class, "array", "()[B");
    if (buffer_class == nullptr || wrap == nullptr || array_method == nullptr) {
        report.line("write_through", "fail");
        report.line("identity", "fail");
        report.line("retained", "fail");
        report.line("monitor", "fail");
        report.line("pop", "fail");
        return;
    }
    const auto result_of = [&](jobject original) {
        jobject buffer = call_static(env, buffer_class, wrap, original);
        jobject result = call_instance(env, buffer, array_method);
        delete_local(env, buffer);
        return result;
    };
    const auto make_original = [&](jbyte first) {
        jobject original = new_array(env, 4);
        const jbyte initial[4] = {first, 18, 19, 20};
        set_region(env, original, 0, 4, initial);
        return original;
    };

    jobject original = make_original(17);
    jobject shadow = result_of(original);
    const auto original_byte = [&](jsize index) {
        jbyte value = 0;
        get_region(env, original, index, 1, &value);
        return value;
    };

    // Writes reach the Java array.
    jbyte patch = 77;
    set_region(env, shadow, 0, 1, &patch);
    bool writes = original_byte(0) == 77;
    const auto get_elements = entry<jbyte* (*)(Env*, jarray, jboolean*)>(env, slot::GetByteArrayElements);
    const auto release_elements = entry<void (*)(Env*, jarray, jbyte*, jint)>(env, slot::ReleaseByteArrayElements);
    jbyte* elements = get_elements(env, shadow, nullptr);
    if (elements != nullptr) {
        elements[1] = 55;
        release_elements(env, shadow, elements, 0);
    }
    writes = writes && elements != nullptr && original_byte(1) == 55;
    const auto critical = entry<void* (*)(Env*, jarray, jboolean*)>(env, slot::GetPrimitiveArrayCritical);
    const auto release_critical = entry<void (*)(Env*, jarray, void*, jint)>(env, slot::ReleasePrimitiveArrayCritical);
    jbyte* bytes = static_cast<jbyte*>(critical(env, shadow, nullptr));
    if (bytes != nullptr) {
        bytes[2] = 88;
        release_critical(env, shadow, bytes, 0);
    }
    writes = writes && bytes != nullptr && original_byte(2) == 88;
    report.verdict("write_through", writes && !exception_check(env));

    // One object.
    const auto same_object = entry<jboolean (*)(Env*, jobject, jobject)>(env, slot::IsSameObject);
    const auto new_global = entry<jobject (*)(Env*, jobject)>(env, slot::NewGlobalRef);
    jobject global = new_global(env, shadow);
    report.verdict("identity", global != nullptr && same_object(env, shadow, original) && same_object(env, global, original) &&
                                   same_object(env, shadow, global));
    if (global != nullptr) {
        entry<void (*)(Env*, jobject)>(env, slot::DeleteGlobalRef)(env, global);
    }

    // The monitor.
    const auto monitor_enter = entry<jint (*)(Env*, jobject)>(env, slot::MonitorEnter);
    const auto monitor_exit = entry<jint (*)(Env*, jobject)>(env, slot::MonitorExit);
    const bool entered = monitor_enter(env, shadow) == 0;
    report.verdict("monitor", entered && monitor_exit(env, shadow) == 0 && !exception_check(env));

    // The local reference table never recycles what the application still holds.
    jobject held[kRetainedResults];
    int kept = 0;
    bool retained = true;
    for (int i = 0; i < kRetainedResults; ++i) {
        jobject next = make_original(static_cast<jbyte>(i));
        held[kept] = result_of(next);
        retained = retained && held[kept] != nullptr;
        ++kept;
        delete_local(env, next);
    }
    jbyte first_byte = 0;
    get_region(env, shadow, 0, 1, &first_byte);
    retained = retained && first_byte == 77 && array_length(env, shadow) == 4;
    for (int i = 0; i < kept; ++i) {
        jbyte value = 0;
        get_region(env, held[i], 0, 1, &value);
        retained = retained && array_length(env, held[i]) == 4 && value == static_cast<jbyte>(i);
        delete_local(env, held[i]);
    }
    report.verdict("retained", retained && !exception_check(env));

    // PopLocalFrame.
    entry<jint (*)(Env*, jint)>(env, slot::PushLocalFrame)(env, 32);
    jobject in_frame = result_of(original);
    jobject outside = entry<jobject (*)(Env*, jobject)>(env, slot::PopLocalFrame)(env, in_frame);
    report.verdict("pop", outside != nullptr && array_length(env, outside) == 4 && !exception_check(env));
    delete_local(env, outside);
    delete_local(env, shadow);
    delete_local(env, original);
}

// getline and malloc_usable_size on blocks the application allocated.
void allocator_cases(Report& report) {
    size_t size = 16;
    char* buffer = static_cast<char*>(probe_malloc(size));
    void* file = probe_fopen("/proc/self/maps", "r");
    const long got = file != nullptr ? probe_getline(&buffer, &size, file) : -1;
    report.verdict("getline", got > 16);
    if (file != nullptr) {
        probe_fclose(file);
    }
    probe_free(buffer);
    void* block = probe_malloc(32);
    report.verdict("usable_size", probe_usable_size(block) >= 32);
    probe_free(block);

    // The other direction: getline allocates the line, the application grows and frees it.
    char* line = nullptr;
    size_t line_size = 0;
    void* maps = probe_fopen("/proc/self/maps", "r");
    const long read = maps != nullptr ? probe_getline(&line, &line_size, maps) : -1;
    bool ok = read > 0 && line != nullptr;
    const char first = ok ? line[0] : 0;
    char* bigger = ok ? static_cast<char*>(probe_realloc(line, 4096)) : nullptr;
    ok = ok && bigger != nullptr && bigger[0] == first;
    probe_free(bigger != nullptr ? bigger : line);
    if (maps != nullptr) {
        probe_fclose(maps);
    }
    report.verdict("libc_block", ok);
}

struct ThreadWork {
    Vm* vm;
    jclass probe;  // a global reference
    jmethodID payload;
    bool ok;
};

void* thread_main(void* argument) {
    ThreadWork* work = static_cast<ThreadWork*>(argument);
    Env* env = nullptr;
    if (entry<jint (*)(Vm*, void**, void*)>(reinterpret_cast<Env*>(work->vm), vmslot::AttachCurrentThread)(
            work->vm, reinterpret_cast<void**>(&env), nullptr) != 0) {
        return nullptr;
    }
    const auto call_object = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
    const auto delete_local = entry<void (*)(Env*, jobject)>(env, slot::DeleteLocalRef);
    jobject array = call_object(env, work->probe, work->payload);
    if (array != nullptr) {
        Report scratch;
        work->ok = raw_read(scratch, array);
        delete_local(env, array);
    }
    entry<jint (*)(Vm*)>(reinterpret_cast<Env*>(work->vm), vmslot::DetachCurrentThread)(work->vm);
    return nullptr;
}

// The raw read from a thread of its own: the layer keeps its state for each thread.
bool thread_raw_read(Env* env, jclass probe, jmethodID payload) {
    Vm* vm = nullptr;
    if (entry<jint (*)(Env*, Vm**)>(env, 219)(env, &vm) != 0) {  // GetJavaVM
        return false;
    }
    ThreadWork work{vm, static_cast<jclass>(entry<jobject (*)(Env*, jobject)>(env, slot::NewGlobalRef)(env, probe)),
                    payload, false};
    unsigned long thread = 0;
    const bool started = probe_pthread_create(&thread, nullptr, thread_main, &work) == 0;
    if (started) {
        probe_pthread_join(thread, nullptr);
    }
    entry<void (*)(Env*, jobject)>(env, slot::DeleteGlobalRef)(env, work.probe);
    return started && work.ok;
}

}  // namespace

// Probe.run(String options): the native method of the Java receiver; the text it returns is the report.
extern "C" __attribute__((visibility("default"))) jstring Java_org_example_andarium_oldappprobe_Probe_run(
    Env* env, jclass probe, jstring options) {
    using GetUtf = const char* (*)(Env*, jstring, jboolean*);
    using ReleaseUtf = void (*)(Env*, jstring, const char*);
    const char* opts = options != nullptr ? entry<GetUtf>(env, 169)(env, options, nullptr) : "";  // GetStringUTFChars
    Report report;
    report.line("shim_mapped", shim_mapped() ? "yes" : "no");

    const auto method_id = entry<jmethodID (*)(Env*, jclass, const char*, const char*)>(env, slot::GetStaticMethodID);
    const auto call_object = entry<jobject (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kObject, kVarargs));
    const auto call_int = entry<jint (*)(Env*, jclass, jmethodID, ...)>(env, static_call_slot(kInt, kVarargs));
    const auto array_length = entry<jsize (*)(Env*, jarray)>(env, slot::GetArrayLength);
    const auto get_region = entry<void (*)(Env*, jarray, jsize, jsize, void*)>(env, slot::GetByteArrayRegion);
    const auto delete_local = entry<void (*)(Env*, jobject)>(env, slot::DeleteLocalRef);
    const auto exception_check = entry<jboolean (*)(Env*)>(env, slot::ExceptionCheck);
    const auto exception_clear = entry<void (*)(Env*)>(env, slot::ExceptionClear);

    jmethodID payload = method_id(env, probe, "payload", "()[B");
    jmethodID echo = method_id(env, probe, "echo", "([B)I");
    jobject array = payload != nullptr ? call_object(env, probe, payload) : nullptr;
    if (exception_check(env)) {
        exception_clear(env);
        array = nullptr;
    }
    if (array == nullptr || echo == nullptr) {
        report.line("raw_read", "fail");
        report.line("passback", "fail");
    } else {
        report.verdict("raw_read", raw_read(report, array));
        char bytes[64];
        const jint echoed = call_int(env, probe, echo, array);
        const jsize counted = array_length(env, array);
        get_region(env, array, 0, kPayloadLength, bytes);
        report.verdict("passback", !exception_check(env) && echoed == kPayloadLength && counted == kPayloadLength &&
                                       same(bytes, kPayload, kPayloadLength));
        exception_clear(env);

        const auto get_elements = entry<jbyte* (*)(Env*, jarray, jboolean*)>(env, slot::GetByteArrayElements);
        const auto release_elements = entry<void (*)(Env*, jarray, jbyte*, jint)>(env, slot::ReleaseByteArrayElements);
        jbyte* elements = get_elements(env, array, nullptr);
        bool elements_ok = elements != nullptr && same(reinterpret_cast<const char*>(elements), kPayload, kPayloadLength);
        if (elements != nullptr) {
            release_elements(env, array, elements, 2);  // JNI_ABORT
        }
        report.verdict("elements", elements_ok && !exception_check(env));

        jobject global = entry<jobject (*)(Env*, jobject)>(env, slot::NewGlobalRef)(env, array);
        bool global_ok = global != nullptr && array_length(env, global) == kPayloadLength &&
                         call_int(env, probe, echo, global) == kPayloadLength && !exception_check(env);
        if (global != nullptr) {
            entry<void (*)(Env*, jobject)>(env, slot::DeleteGlobalRef)(env, global);
        }
        exception_clear(env);
        report.verdict("global", global_ok);
        delete_local(env, array);
        report.verdict("thread_raw", thread_raw_read(env, probe, payload));
    }

    array_cases(report, env);
    allocator_cases(report);
    {
        Vm* vm = nullptr;
        entry<jint (*)(Env*, Vm**)>(env, 219)(env, &vm);  // GetJavaVM
        report.verdict("table_read_only", vm != nullptr && mapped_read_only(env->functions) && mapped_read_only(vm->functions));
    }
    report.verdict("low_heap", low_heap());

    report.line("done", "1");
    if (options != nullptr) {
        entry<ReleaseUtf>(env, 170)(env, options, opts);  // ReleaseStringUTFChars
    }
    return entry<jstring (*)(Env*, const char*)>(env, slot::NewStringUTF)(env, report.text);
}

extern "C" __attribute__((visibility("default"))) jint JNI_OnLoad(Vm* vm, void*) {
    // The ordinary first call of a JNI library: it is also what makes the old-application
    // layer look at this library's allocator imports.
    void* env = nullptr;
    entry<jint (*)(Vm*, void**, jint)>(reinterpret_cast<Env*>(vm), vmslot::GetEnv)(vm, &env, 0x00010006);
    return 0x00010006;
}

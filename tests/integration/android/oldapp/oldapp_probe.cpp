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
//   leak         (only when asked) local references from one native call, never deleted:
//                "leak" makes 4000, "leak=<count>" that many
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
constexpr int kLeaks = 4000;  // the default of "leak"; "leak=<count>" asks for another number

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

// The number after "leak=" in the options, or the default.
int leak_count(const char* options) {
    const size_t n = length_of(options);
    for (size_t i = 0; i + 5 <= n; ++i) {
        if (same(options + i, "leak=", 5)) {
            int value = 0;
            for (size_t k = i + 5; k < n && options[k] >= '0' && options[k] <= '9'; ++k) {
                value = value * 10 + (options[k] - '0');
            }
            return value > 0 ? value : kLeaks;
        }
    }
    return kLeaks;
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
        void* block = (i % 3 == 0) ? probe_calloc(1, kBlockBytes) : probe_malloc(kBlockBytes / 2);
        if (i % 3 == 1) {
            block = probe_realloc(block, kBlockBytes);
        }
        blocks[i] = block;
        if (block == nullptr || reinterpret_cast<uintptr_t>(block) + kBlockBytes > 0x80000000u) {
            ok = false;
            continue;
        }
        for (size_t at = 0; at < kBlockBytes; at += 4096) {
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

    report.verdict("low_heap", low_heap());

    if (contains(opts, length_of(opts), "leak")) {
        const auto new_utf = entry<jstring (*)(Env*, const char*)>(env, slot::NewStringUTF);
        const auto utf_length = entry<jsize (*)(Env*, jstring)>(env, 168);  // GetStringUTFLength
        bool ok = true;
        const int leaks = leak_count(opts);
        for (int i = 0; i < leaks && ok; ++i) {
            ok = new_utf(env, "leaked") != nullptr && !exception_check(env);
        }
        jstring last = ok ? new_utf(env, "last") : nullptr;
        ok = ok && last != nullptr && utf_length(env, last) == 4 && !exception_check(env);
        report.verdict("leak", ok);
    } else {
        report.line("leak", "skipped");
    }
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

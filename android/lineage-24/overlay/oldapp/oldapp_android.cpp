// SPDX-License-Identifier: Apache-2.0
//
// The old-application layer, bound to Android.
//
// ActivityThread loads this library into an application process only for a 32-bit
// application that targets an API level below 14 (Android before 4.0), the applications
// that Dalvik gave direct object pointers and a 32-bit heap below 2 GiB. JNI_OnLoad
// installs the interposer on ART's shared function table (jni_shim.cpp) and keeps an
// allocator below 2 GiB (low_arena.hpp) for the libraries the application ships: their
// allocator imports are redirected when the library is first seen. The platform's own
// libraries keep every original behaviour; a caller is told apart by the mapping its code
// is in.
//
// This file declares the few libc and linker functions it uses itself, so it includes no
// Android header.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "elf_scan.hpp"
#include "jni_shim.hpp"
#include "low_arena.hpp"

extern "C" {
void* oldapp_mmap(void*, size_t, int, int, int, long) __asm__("mmap");
int oldapp_munmap(void*, size_t) __asm__("munmap");
int oldapp_mprotect(void*, size_t, int) __asm__("mprotect");
void* oldapp_malloc(size_t) __asm__("malloc");
void* oldapp_calloc(size_t, size_t) __asm__("calloc");
void* oldapp_realloc(void*, size_t) __asm__("realloc");
void oldapp_free(void*) __asm__("free");
size_t oldapp_usable_size(void*) __asm__("malloc_usable_size");
int oldapp_pthread_key_create(int*, void (*)(void*)) __asm__("pthread_key_create");
void* oldapp_pthread_getspecific(int) __asm__("pthread_getspecific");
int oldapp_pthread_setspecific(int, const void*) __asm__("pthread_setspecific");
int oldapp_log_print(int, const char*, const char*, ...) __asm__("__android_log_print");

// bionic's struct dl_phdr_info, 32-bit layout.
struct OldappPhdrInfo {
    uint32_t addr;
    const char* name;
    const oldapp::Elf32Phdr* phdr;
    uint16_t phnum;
    uint64_t adds;
    uint64_t subs;
    size_t tls_modid;
    void* tls_data;
};
int oldapp_dl_iterate_phdr(int (*)(OldappPhdrInfo*, size_t, void*), void*) __asm__("dl_iterate_phdr");
}

namespace {

using namespace oldapp;

constexpr size_t kArenaBytes = 64u << 20;
constexpr size_t kShadowBytes = 16u << 20;
constexpr uintptr_t kLowLimit = 0x7ff00000u;
constexpr unsigned kNegativeCache = 64;
constexpr int kLogInfo = 4;
constexpr int kLogWarn = 5;
constexpr const char* kTag = "kernarium_oldapp";

struct Module {
    uintptr_t lo;
    uintptr_t hi;
    bool app;
};
struct Snapshot {
    unsigned count;
    Module modules[1];  // sorted by lo; really `count` entries
};

Snapshot* volatile g_snapshot = nullptr;
uintptr_t g_negative[kNegativeCache];
uint32_t g_refresh_lock = 0;
uint32_t g_patched[256];  // load biases of the libraries whose allocator imports are already redirected
unsigned g_patched_count = 0;

Arena g_arena;
bool g_arena_ready = false;
int g_thread_key = 0;
bool g_have_thread_key = false;  // a key is any int: bionic sets its top bit
bool g_installed = false;

void log_line(const char* message, long a, long b) { oldapp_log_print(kLogInfo, kTag, "%s %ld %ld", message, a, b); }

// ---- the allocator the old libraries are pointed at

void* low_malloc(size_t size) {
    void* pointer = g_arena.allocate(size);
    return pointer != nullptr ? pointer : oldapp_malloc(size);
}
void low_free(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    if (g_arena.contains(pointer)) {
        g_arena.release(pointer);
    } else {
        oldapp_free(pointer);
    }
}
void* low_calloc(size_t count, size_t size) {
    if (size != 0 && count > 0xffffffffu / size) {
        return nullptr;
    }
    const size_t bytes = count * size;
    void* pointer = g_arena.allocate(bytes);
    if (pointer == nullptr) {
        return oldapp_calloc(count, size);
    }
    __builtin_memset(pointer, 0, bytes);
    return pointer;
}
void* low_realloc(void* pointer, size_t size) {
    if (pointer == nullptr) {
        return low_malloc(size);
    }
    if (g_arena.contains(pointer)) {
        void* moved = g_arena.reallocate(pointer, size);
        if (moved != nullptr) {
            return moved;
        }
        void* outside = oldapp_malloc(size);  // the arena is full: the block moves out of it
        if (outside != nullptr) {
            const size_t old = g_arena.usable_size(pointer);
            __builtin_memcpy(outside, pointer, old < size ? old : size);
            g_arena.release(pointer);
        }
        return outside;
    }
    void* inside = g_arena.allocate(size);  // a block from before the redirection moves in
    if (inside == nullptr) {
        return oldapp_realloc(pointer, size);
    }
    const size_t old = oldapp_usable_size(pointer);
    __builtin_memcpy(inside, pointer, old < size ? old : size);
    oldapp_free(pointer);
    return inside;
}
char* low_strndup(const char* text, size_t limit) {
    size_t length = 0;
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    char* copy = static_cast<char*>(low_malloc(length + 1));
    if (copy != nullptr) {
        __builtin_memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}
char* low_strdup(const char* text) { return low_strndup(text, 0xffffffffu); }
void* low_new(size_t size) {
    void* pointer = low_malloc(size != 0 ? size : 1);
    if (pointer == nullptr) {
        __builtin_trap();  // out of memory: operator new would have thrown std::bad_alloc
    }
    return pointer;
}
void* low_new_nothrow(size_t size, const void*) { return low_malloc(size != 0 ? size : 1); }
void low_delete(void* pointer) { low_free(pointer); }
void low_delete_nothrow(void* pointer, const void*) { low_free(pointer); }
void low_delete_sized(void* pointer, size_t) { low_free(pointer); }

void* replacement_for(Import what) {
    switch (what) {
        case Import::kMalloc: return reinterpret_cast<void*>(&low_malloc);
        case Import::kFree: return reinterpret_cast<void*>(&low_free);
        case Import::kCalloc: return reinterpret_cast<void*>(&low_calloc);
        case Import::kRealloc: return reinterpret_cast<void*>(&low_realloc);
        case Import::kStrdup: return reinterpret_cast<void*>(&low_strdup);
        case Import::kStrndup: return reinterpret_cast<void*>(&low_strndup);
        case Import::kNew:
        case Import::kNewArray: return reinterpret_cast<void*>(&low_new);
        case Import::kNewNothrow:
        case Import::kNewArrayNothrow: return reinterpret_cast<void*>(&low_new_nothrow);
        case Import::kDelete:
        case Import::kDeleteArray: return reinterpret_cast<void*>(&low_delete);
        case Import::kDeleteNothrow:
        case Import::kDeleteArrayNothrow: return reinterpret_cast<void*>(&low_delete_nothrow);
        case Import::kDeleteSized:
        case Import::kDeleteArraySized: return reinterpret_cast<void*>(&low_delete_sized);
        case Import::kCount: break;
    }
    return nullptr;
}

// ---- the modules of the process

bool platform_path(const char* name) {
    static const char* const kPrefixes[] = {"/system/", "/system_ext/", "/vendor/", "/product/", "/odm/", "/apex/",
                                            "/linker", "/data/dalvik-cache/"};
    if (name == nullptr || name[0] != '/') {
        return true;  // the main program, the vdso, a name with no path
    }
    for (const char* prefix : kPrefixes) {
        size_t i = 0;
        while (prefix[i] != '\0' && name[i] == prefix[i]) {
            ++i;
        }
        if (prefix[i] == '\0') {
            return true;
        }
    }
    return false;
}

void lock_refresh() {
    while (__atomic_exchange_n(&g_refresh_lock, 1u, __ATOMIC_ACQUIRE) != 0) {
    }
}
void unlock_refresh() { __atomic_store_n(&g_refresh_lock, 0u, __ATOMIC_RELEASE); }

bool already_patched(uint32_t bias) {
    for (unsigned i = 0; i < g_patched_count; ++i) {
        if (g_patched[i] == bias) {
            return true;
        }
    }
    return false;
}

// Writes the arena's functions into the allocator imports of an application library.
void redirect_allocator(const OldappPhdrInfo& info) {
    if (!g_arena_ready || already_patched(info.addr) || g_patched_count >= 256) {
        return;
    }
    g_patched[g_patched_count++] = info.addr;
    ImportSlot slots[96];
    const size_t found = scan_imports(info.addr, info.phdr, info.phnum, slots, 96);
    const size_t count = found < 96 ? found : 96;
    for (size_t i = 0; i < count; ++i) {
        void* replacement = replacement_for(slots[i].what);
        uintptr_t address = info.addr + slots[i].vaddr;
        const uintptr_t page = address & ~uintptr_t{4095};
        const int protection = original_protection(info.phdr, info.phnum, slots[i].vaddr);
        if (oldapp_mprotect(reinterpret_cast<void*>(page), 4096, 3) != 0) {
            continue;
        }
        __atomic_store_n(reinterpret_cast<void**>(address), replacement, __ATOMIC_RELEASE);
        oldapp_mprotect(reinterpret_cast<void*>(page), 4096, protection);
    }
    oldapp_log_print(kLogInfo, kTag, "allocator redirected in %s: %ld imports", info.name, static_cast<long>(count));
}

struct Collect {
    Module modules[512];
    unsigned count;
};

int collect_module(OldappPhdrInfo* info, size_t, void* user) {
    Collect* collect = static_cast<Collect*>(user);
    uintptr_t lo = ~uintptr_t{0};
    uintptr_t hi = 0;
    for (unsigned i = 0; i < info->phnum; ++i) {
        if (info->phdr[i].p_type == kPtLoad) {
            const uintptr_t start = info->addr + info->phdr[i].p_vaddr;
            const uintptr_t end = start + info->phdr[i].p_memsz;
            lo = start < lo ? start : lo;
            hi = end > hi ? end : hi;
        }
    }
    if (hi == 0 || collect->count >= 512) {
        return 0;
    }
    // This library's own code is never the application's, wherever it was installed from.
    const uintptr_t self = reinterpret_cast<uintptr_t>(&collect_module);
    const bool app = !platform_path(info->name) && !(self >= lo && self < hi);
    collect->modules[collect->count++] = Module{lo, hi, app};
    if (app) {
        redirect_allocator(*info);
    }
    return 0;
}

void sort_modules(Module* modules, unsigned count) {
    for (unsigned i = 1; i < count; ++i) {  // insertion sort: the list is short and nearly sorted
        Module key = modules[i];
        unsigned j = i;
        while (j > 0 && modules[j - 1].lo > key.lo) {
            modules[j] = modules[j - 1];
            --j;
        }
        modules[j] = key;
    }
}

void refresh_modules() {
    lock_refresh();
    static Collect collect;  // under the lock
    collect.count = 0;
    oldapp_dl_iterate_phdr(collect_module, &collect);
    sort_modules(collect.modules, collect.count);
    const size_t bytes = sizeof(unsigned) + sizeof(Module) * (collect.count != 0 ? collect.count : 1);
    Snapshot* snapshot = static_cast<Snapshot*>(oldapp_malloc(bytes));
    if (snapshot != nullptr) {  // earlier snapshots are not freed: a reader may still be using one
        snapshot->count = collect.count;
        __builtin_memcpy(snapshot->modules, collect.modules, sizeof(Module) * collect.count);
        __atomic_store_n(&g_snapshot, snapshot, __ATOMIC_RELEASE);
    }
    unlock_refresh();
}

// 1: application code, 0: platform code, -1: no mapping known for the address.
int classify(uintptr_t address) {
    const Snapshot* snapshot = __atomic_load_n(&g_snapshot, __ATOMIC_ACQUIRE);
    if (snapshot == nullptr) {
        return -1;
    }
    unsigned low = 0;
    unsigned high = snapshot->count;
    while (low < high) {
        const unsigned middle = (low + high) / 2;
        if (address < snapshot->modules[middle].lo) {
            high = middle;
        } else if (address >= snapshot->modules[middle].hi) {
            low = middle + 1;
        } else {
            return snapshot->modules[middle].app ? 1 : 0;
        }
    }
    return -1;
}

bool is_app_caller(const void* return_address) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(return_address) & ~uintptr_t{1};  // the Thumb bit
    int verdict = classify(address);
    if (verdict >= 0) {
        return verdict == 1;
    }
    uintptr_t& cached = g_negative[(address >> 12) % kNegativeCache];
    if (cached == (address >> 12)) {
        return false;
    }
    refresh_modules();  // a library may have been loaded since the last look
    verdict = classify(address);
    if (verdict >= 0) {
        return verdict == 1;
    }
    cached = address >> 12;  // code in no mapping the linker knows (generated code): not the application's
    return false;
}

// ---- protection of the interposed tables

struct Opened {
    uintptr_t page;
    int protection;
};
Opened g_opened[8];
unsigned g_opened_count = 0;

struct Lookup {
    uintptr_t address;
    int protection;
};
int find_protection(OldappPhdrInfo* info, size_t, void* user) {
    Lookup* lookup = static_cast<Lookup*>(user);
    for (unsigned i = 0; i < info->phnum; ++i) {
        if (info->phdr[i].p_type == kPtLoad && lookup->address >= info->addr + info->phdr[i].p_vaddr &&
            lookup->address < info->addr + info->phdr[i].p_vaddr + info->phdr[i].p_memsz) {
            lookup->protection = original_protection(info->phdr, info->phnum, lookup->address - info->addr);
            return 1;
        }
    }
    return 0;
}

bool make_writable(void* address, size_t length) {
    g_opened_count = 0;
    const uintptr_t first = reinterpret_cast<uintptr_t>(address) & ~uintptr_t{4095};
    const uintptr_t last = (reinterpret_cast<uintptr_t>(address) + length - 1) & ~uintptr_t{4095};
    for (uintptr_t page = first; page <= last; page += 4096) {
        Lookup lookup{page, -1};
        oldapp_dl_iterate_phdr(find_protection, &lookup);
        if (lookup.protection < 0) {
            lookup.protection = 3;  // not in a library: plain data, nothing to restore
        }
        if (oldapp_mprotect(reinterpret_cast<void*>(page), 4096, 3) != 0) {
            return false;
        }
        if (g_opened_count < 8) {
            g_opened[g_opened_count++] = Opened{page, lookup.protection};
        }
    }
    return true;
}

bool make_read_only(void*, size_t) {
    for (unsigned i = 0; i < g_opened_count; ++i) {
        oldapp_mprotect(reinterpret_cast<void*>(g_opened[i].page), 4096, g_opened[i].protection);
    }
    g_opened_count = 0;
    return true;
}

// ---- per-thread state

void thread_exit(void* state) {
    release_thread_state(state);
    oldapp_free(state);
}

void* thread_state(size_t size) {
    if (!g_have_thread_key) {
        return nullptr;
    }
    void* state = oldapp_pthread_getspecific(g_thread_key);
    if (state == nullptr) {
        state = oldapp_calloc(1, size);
        if (state != nullptr) {
            oldapp_pthread_setspecific(g_thread_key, state);
        }
    }
    return state;
}

// ---- the region below 2 GiB

uint8_t* map_low_region(size_t bytes) {
    static const uintptr_t kHints[] = {0x30000000u, 0x40000000u, 0x50000000u, 0x58000000u, 0x20000000u,
                                       0x10000000u, 0x60000000u};
    constexpr int kProtReadWrite = 3;
    constexpr int kMapPrivateAnonymousNoReserve = 0x02 | 0x20 | 0x4000;
    constexpr int kMapFixedNoReplace = 0x100000;
    for (uintptr_t hint : kHints) {
        void* mapped = oldapp_mmap(reinterpret_cast<void*>(hint), bytes, kProtReadWrite,
                                   kMapPrivateAnonymousNoReserve | kMapFixedNoReplace, -1, 0);
        if (mapped == reinterpret_cast<void*>(-1)) {
            continue;
        }
        const uintptr_t start = reinterpret_cast<uintptr_t>(mapped);
        if (start >= 0x10000u && start + bytes <= kLowLimit) {
            return static_cast<uint8_t*>(mapped);
        }
        oldapp_munmap(mapped, bytes);  // an older kernel ignores the flag and may place it anywhere
    }
    return nullptr;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int JNI_OnLoad(void* vm_pointer, void*) {
    constexpr int kJniVersion = 0x00010006;
    auto* vm = static_cast<jni::Vm*>(vm_pointer);
    if (g_installed) {
        return kJniVersion;
    }
    g_installed = true;
    jni::Env* env = nullptr;
    if (reinterpret_cast<int (*)(jni::Vm*, void**, int)>(vm->functions[jni::vmslot::GetEnv])(
            vm, reinterpret_cast<void**>(&env), kJniVersion) != 0 || env == nullptr) {
        oldapp_log_print(kLogWarn, kTag, "no JNIEnv on the loading thread: the layer is not installed");
        return kJniVersion;
    }
    g_have_thread_key = oldapp_pthread_key_create(&g_thread_key, thread_exit) == 0;
    uint8_t* region = map_low_region(kArenaBytes + kShadowBytes);
    if (region != nullptr) {
        g_arena_ready = g_arena.init(region, kArenaBytes);
        oldapp_log_print(kLogInfo, kTag, "region below 2 GiB at %p", static_cast<void*>(region));
    } else {
        oldapp_log_print(kLogWarn, kTag, "no region below 2 GiB: allocator imports are left alone");
    }
    const Platform platform{is_app_caller, thread_state, log_line, make_writable, make_read_only};
    refresh_modules();  // the libraries the application loaded before this one
    void** table = const_cast<void**>(env->functions);
    const bool jni_ok = install_jni_wrappers(table, platform, region != nullptr ? region + kArenaBytes : nullptr,
                                             region != nullptr ? kShadowBytes : 0, true, env);
    install_vm_wrappers(const_cast<void**>(vm->functions), platform);
    oldapp_log_print(kLogInfo, kTag, "installed: jni=%ld arena=%ld", static_cast<long>(jni_ok),
                     static_cast<long>(g_arena_ready));
    return kJniVersion;
}

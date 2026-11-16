// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A guest program that runs the Android image's rewritten libz.so in many
 * processes, for the rows that exercise how the framework shares what it
 * verified between address spaces.
 *
 * It is built like aot_guest_debug.cpp (Android arm64, the image's own libc.so
 * and libz.so, no headers, no runtime of its own) and loaded into a stock
 * process: AOT_SHARE_MODE=<mode> LD_PRELOAD=<this library> toybox true. The
 * parent never runs libz itself, except where a mode says so: its children do,
 * so that the parent maps few of the library's pages and the page cache can
 * drop the rest. The mode names what the children run:
 *
 *   fork      the system libz.so, twelve children one after another and then
 *             four at once: the pages of a read-only file system, which the
 *             kernel declares immutable, are verified once for all of them.
 *   copy      a byte-identical copy of libz.so on the writable data file
 *             system, which binds by content like the original but is not
 *             immutable, so every child verifies its pages.
 *   cow       the system libz.so after a write of the same bytes copied one
 *             of its text pages, then a child that patches the copy and
 *             others that do not; and after it an untouched child.
 *   mprotect  children that change the protection of a text page to writable
 *             and back, and one that then writes it.
 *   pressure  rounds of children, each followed by a child that takes more
 *             memory than is free, so that the kernel reclaims file pages (the
 *             library's among them) and reuses their frames; the next round
 *             runs the library again. The guest is not rooted, so there is no
 *             drop_caches or compaction to do it exactly.
 *
 * Every line it prints is a function of the guest's architectural behaviour
 * alone: the runner (run_aot_guest.py) requires the same lines with the
 * component on and off, and, in a diagnostics build, reads how often the
 * framework hashed a page and how often it did not.
 */
#include <stddef.h>
#include <stdint.h>

extern "C" {
void *mmap(void *, size_t, int, int, int, long);
int munmap(void *, size_t);
int mprotect(void *, size_t, int);
int mincore(void *, size_t, unsigned char *);
int open(const char *, int, ...);
long read(int, void *, size_t);
long write(int, const void *, size_t);
int close(int);
int pipe(int *);
int unlink(const char *);
int dprintf(int, const char *, ...);
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
int strcmp(const char *, const char *);
char *getenv(const char *);
void _exit(int) __attribute__((noreturn));
int fork();
int waitpid(int, int *, int);
unsigned alarm(unsigned);

// libz.so (zlib 1.3.x); z_stream is the LP64 layout.
struct ZStream {
    const uint8_t *next_in;
    uint32_t avail_in;
    uint64_t total_in;
    uint8_t *next_out;
    uint32_t avail_out;
    uint64_t total_out;
    const char *msg;
    void *state;
    void *zalloc;
    void *zfree;
    void *opaque;
    int data_type;
    uint64_t adler;
    uint64_t reserved;
};
unsigned long adler32(unsigned long, const uint8_t *, unsigned);
unsigned long crc32(unsigned long, const uint8_t *, unsigned);
const char *zlibVersion();
int deflateInit_(ZStream *, int, const char *, int);
int deflate(ZStream *, int);
int deflateEnd(ZStream *);
long lseek(int, long, int);
}

namespace {

constexpr size_t kPage = 4096;
constexpr int kRead = 1;
constexpr int kWrite = 2;
constexpr int kExec = 4;
constexpr int kMapPrivateAnonymous = 0x22;
constexpr int kMapShared = 1;
constexpr int kOpenWriteCreateTruncate = 1 | 0x40 | 0x200;
constexpr int kRtldNow = 2;

constexpr const char *kSystemZ = "/system/lib64/libz.so";
constexpr const char *kCopyZ = "/data/local/tmp/libz_share_copy.so";

void fail(const char *what) {
    dprintf(1, "FAIL %s\n", what);
}

// A pool for libz's own allocations, so that no allocator state reaches the
// output.
alignas(16) uint8_t pool[1 << 20];
size_t pool_used;
extern "C" void *pool_alloc(void *, unsigned items, unsigned size) {
    const size_t bytes = (static_cast<size_t>(items) * size + 63U) & ~size_t{63};
    if (pool_used + bytes > sizeof(pool)) {
        return nullptr;
    }
    void *p = pool + pool_used;
    pool_used += bytes;
    memset(p, 0, bytes);
    return p;
}
extern "C" void pool_free(void *, void *) {}

// The entry points of the libz.so a child runs.
struct Zlib {
    unsigned long (*adler)(unsigned long, const uint8_t *, unsigned);
    unsigned long (*crc)(unsigned long, const uint8_t *, unsigned);
    const char *(*version)();
    int (*deflate_init)(ZStream *, int, const char *, int);
    int (*deflate_run)(ZStream *, int);
    int (*deflate_end)(ZStream *);
};

Zlib system_zlib() {
    return {adler32, crc32, zlibVersion, deflateInit_, deflate, deflateEnd};
}

template <typename T>
bool symbol(void *handle, const char *name, T &out) {
    void *address = dlsym(handle, name);
    out = reinterpret_cast<T>(address);
    return address != nullptr;
}

bool copy_zlib(void *handle, Zlib &z) {
    return symbol(handle, "adler32", z.adler) && symbol(handle, "crc32", z.crc) &&
           symbol(handle, "zlibVersion", z.version) && symbol(handle, "deflateInit_", z.deflate_init) &&
           symbol(handle, "deflate", z.deflate_run) && symbol(handle, "deflateEnd", z.deflate_end);
}

uint8_t *data;  // deterministic content the work reads
constexpr size_t kDataPages = 4;
constexpr unsigned kWorkBytes = 3 * 1024 + 129;

void fill_data() {
    data = static_cast<uint8_t *>(mmap(nullptr, 8 * kPage, kRead | kWrite, kMapPrivateAnonymous, -1, 0));
    uint32_t state = 0x2545f491U;
    for (size_t i = 0; i < kDataPages * kPage; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[i] = (state & 7U) == 0U ? static_cast<uint8_t>(state >> 8) : "etaoin shrdlu"[(state >> 4) % 13U];
    }
}

// The covered calls a child makes: the same for a round and a function of the
// data alone.
unsigned long work(const Zlib &z, unsigned round) {
    uint8_t out[4096];
    const size_t at = (round * 37U) % 512U;
    unsigned long sum = z.crc(round, data + at, kWorkBytes);
    sum ^= z.adler(round + 1, data + at, kWorkBytes);
    ZStream stream{};
    pool_used = 0;
    stream.zalloc = reinterpret_cast<void *>(pool_alloc);
    stream.zfree = reinterpret_cast<void *>(pool_free);
    if (z.deflate_init(&stream, 6, z.version(), sizeof(ZStream)) == 0) {
        stream.next_in = data + at;
        stream.avail_in = 700;
        stream.next_out = out;
        stream.avail_out = sizeof(out);
        z.deflate_run(&stream, 4);
        sum += z.crc(0, out, static_cast<unsigned>(stream.total_out));
        z.deflate_end(&stream);
    }
    return sum;
}

// What a child does to its own copy of the library before it runs.
enum class Prep {
    None,
    // A write of the byte already there: the page is copied and stays equal.
    CopyPage,
    // The same, then the entry of adler32 is replaced.
    PatchAdler,
    // The text page is made writable and back, with no store.
    Protect,
    // The text page is made writable, written, and made executable again.
    ProtectAndWrite,
};

// Make the stores to [start, start + length) visible to instruction fetch.
void flush_code(void *start, size_t length) {
    const uintptr_t first = reinterpret_cast<uintptr_t>(start) & ~uintptr_t{15};
    const uintptr_t last = reinterpret_cast<uintptr_t>(start) + length;
    for (uintptr_t a = first; a < last; a += 16) {
        asm volatile("dc cvau, %0" ::"r"(a) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
    for (uintptr_t a = first; a < last; a += 16) {
        asm volatile("ic ivau, %0" ::"r"(a) : "memory");
    }
    asm volatile("dsb ish\n\tisb" ::: "memory");
}

uint8_t *page_of(const void *function) {
    return reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(function) & ~(kPage - 1));
}

void prepare(Prep prep, const Zlib &z) {
    if (prep == Prep::None) {
        return;
    }
    uint8_t *entry = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(z.adler));
    uint8_t *page = page_of(entry);
    if (mprotect(page, kPage, kRead | kWrite | kExec) != 0) {
        fail("mprotect writable");
        return;
    }
    volatile uint8_t *byte = page;
    switch (prep) {
    case Prep::CopyPage:
    case Prep::ProtectAndWrite:
        *byte = *byte;
        break;
    case Prep::PatchAdler: {
        // mov x0, #7 ; ret
        const uint32_t patch[2] = {0xd28000e0U, 0xd65f03c0U};
        memcpy(entry, patch, sizeof(patch));
        flush_code(entry, sizeof(patch));
        break;
    }
    default:
        break;
    }
    if (mprotect(page, kPage, kRead | kExec) != 0) {
        fail("mprotect executable");
    }
}

// One child: it prepares, runs two rounds and sends the sum to the parent.
bool child_start(int index, const Zlib &z, Prep prep, int &read_end, int &pid) {
    int fds[2];
    if (pipe(fds) != 0) {
        fail("pipe");
        return false;
    }
    pid = fork();
    if (pid < 0) {
        fail("fork");
        return false;
    }
    if (pid == 0) {
        close(fds[0]);
        prepare(prep, z);
        unsigned long sum = work(z, static_cast<unsigned>(index)) + work(z, static_cast<unsigned>(index) + 100U);
        write(fds[1], &sum, sizeof(sum));
        _exit(0);
    }
    close(fds[1]);
    read_end = fds[0];
    return true;
}

bool child_finish(const char *label, int index, int read_end, int pid) {
    unsigned long sum = 0;
    const long got = read(read_end, &sum, sizeof(sum));
    close(read_end);
    int status = 0;
    waitpid(pid, &status, 0);
    if (got != static_cast<long>(sizeof(sum)) || status != 0) {
        dprintf(1, "FAIL %s child %d: read %ld status %d\n", label, index, got, status);
        return false;
    }
    dprintf(1, "%s child %d sum=%#lx\n", label, index, sum);
    return true;
}

bool children_in_turn(const char *label, int first, int count, const Zlib &z, Prep prep) {
    for (int i = first; i < first + count; ++i) {
        int read_end = -1;
        int pid = -1;
        if (!child_start(i, z, prep, read_end, pid) || !child_finish(label, i, read_end, pid)) {
            return false;
        }
    }
    return true;
}

bool children_together(const char *label, int first, int count, const Zlib &z) {
    int read_ends[8];
    int pids[8];
    for (int i = 0; i < count; ++i) {
        if (!child_start(first + i, z, Prep::None, read_ends[i], pids[i])) {
            return false;
        }
    }
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        ok = child_finish(label, first + i, read_ends[i], pids[i]) && ok;
    }
    return ok;
}

// ---- modes -----------------------------------------------------------------

void fork_mode() {
    const Zlib z = system_zlib();
    if (children_in_turn("sequential", 0, 12, z, Prep::None)) {
        children_together("together", 12, 4, z);
    }
}

bool copy_file(const char *from, const char *to) {
    const int in = open(from, 0);
    if (in < 0) {
        return false;
    }
    const int out = open(to, kOpenWriteCreateTruncate, 0755);
    if (out < 0) {
        close(in);
        return false;
    }
    static uint8_t buffer[1 << 16];
    bool ok = true;
    for (;;) {
        const long got = read(in, buffer, sizeof(buffer));
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        if (write(out, buffer, static_cast<size_t>(got)) != got) {
            ok = false;
            break;
        }
    }
    close(in);
    close(out);
    return ok;
}

void copy_mode() {
    if (!copy_file(kSystemZ, kCopyZ)) {
        fail("copy libz.so");
        return;
    }
    void *handle = dlopen(kCopyZ, kRtldNow);
    Zlib z{};
    if (handle == nullptr || !copy_zlib(handle, z)) {
        fail("dlopen the copy");
        unlink(kCopyZ);
        return;
    }
    if (children_in_turn("copy", 0, 12, z, Prep::None)) {
        children_together("copy together", 12, 4, z);
    }
    unlink(kCopyZ);
}

void cow_mode() {
    const Zlib z = system_zlib();
    // The children copy the page in turn; the one that patches it changes
    // only its own copy.
    children_in_turn("copy-page", 0, 2, z, Prep::CopyPage);
    children_in_turn("patched", 2, 1, z, Prep::PatchAdler);
    children_in_turn("after-patch", 3, 3, z, Prep::None);
    children_together("together", 6, 4, z);
}

void protect_mode() {
    const Zlib z = system_zlib();
    children_in_turn("protect", 0, 3, z, Prep::Protect);
    children_in_turn("protect-write", 3, 3, z, Prep::ProtectAndWrite);
    children_in_turn("plain", 6, 3, z, Prep::None);
}

// How many of the file's pages the page cache holds, and how many it has.
long cached_pages(const char *path, long *total) {
    const int fd = open(path, 0);
    if (fd < 0) {
        return -1;
    }
    const long size = lseek(fd, 0, 2);
    *total = (size + static_cast<long>(kPage) - 1) / static_cast<long>(kPage);
    void *map = size > 0 ? mmap(nullptr, static_cast<size_t>(size), kRead, kMapShared, fd, 0)
                         : reinterpret_cast<void *>(-1L);
    if (map == reinterpret_cast<void *>(-1L)) {
        close(fd);
        return -1;
    }
    static unsigned char vec[4096];
    long count = -1;
    if (*total <= static_cast<long>(sizeof(vec)) && mincore(map, static_cast<size_t>(size), vec) == 0) {
        count = 0;
        for (long i = 0; i < *total; ++i) {
            count += vec[i] & 1;
        }
    }
    munmap(map, static_cast<size_t>(size));
    close(fd);
    return count;
}

// A number from /proc/meminfo, in kB, or -1.
long meminfo_kb(const char *key) {
    const int fd = open("/proc/meminfo", 0);
    if (fd < 0) {
        return -1;
    }
    static char text[4096];
    const long got = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (got <= 0) {
        return -1;
    }
    text[got] = 0;
    for (long i = 0; i < got; ++i) {
        if (i != 0 && text[i - 1] != '\n') {
            continue;
        }
        long k = 0;
        while (key[k] != 0 && text[i + k] == key[k]) {
            ++k;
        }
        if (key[k] != 0 || text[i + k] != ':') {
            continue;
        }
        long value = 0;
        for (long j = i + k + 1; j < got && text[j] != '\n'; ++j) {
            if (text[j] >= '0' && text[j] <= '9') {
                value = value * 10 + (text[j] - '0');
            }
        }
        return value;
    }
    return -1;
}

// A child that takes memory from the page cache: it touches anonymous pages
// up to what is free and a share of what the page cache holds, so that the
// kernel reclaims file pages, mapped ones included, and later reuses their
// frames. The guest is not rooted (no drop_caches, no compaction), and a page
// that another process maps stays through fadvise, so reclaim under pressure
// is the way a frame of the library is freed.
void pressure_child(long extra_kb) {
    const long free_kb = meminfo_kb("MemFree");
    if (free_kb < 0) {
        _exit(1);
    }
    const size_t bytes = static_cast<size_t>(free_kb + extra_kb) * 1024UL;
    uint8_t *region = static_cast<uint8_t *>(mmap(nullptr, bytes, kRead | kWrite, kMapPrivateAnonymous, -1, 0));
    if (region == reinterpret_cast<uint8_t *>(-1L)) {
        _exit(2);
    }
    for (size_t at = 0; at < bytes; at += kPage) {
        region[at] = 1;
    }
    _exit(0);
}

// How many pages of the library the page cache holds.
long libz_cached() {
    long total = 0;
    return cached_pages(kSystemZ, &total);
}

// The rounds are comments the runner prints and does not compare: how many
// pages of libz.so the page cache held before and after the pressure depends
// on the guest's memory, not on the engine.
void pressure_mode() {
    const Zlib z = system_zlib();
    for (int round = 0; round < 4; ++round) {
        char label[16];
        label[0] = 'r';
        label[1] = static_cast<char>('0' + round);
        label[2] = 0;
        if (!children_in_turn(label, round * 3, 3, z, Prep::None)) {
            return;
        }
        const long before = libz_cached();
        const int pid = fork();
        if (pid == 0) {
            pressure_child(static_cast<long>(meminfo_kb("Cached")) / 2);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        dprintf(1, "# round %d: libz.so pages cached %ld before, %ld after; the pressure child exited %d\n", round,
                before, libz_cached(), status);
    }
    children_in_turn("last", 12, 3, z, Prep::None);
}

__attribute__((constructor)) void run() {
    const char *mode = getenv("AOT_SHARE_MODE");
    if (mode == nullptr) {
        return;
    }
    alarm(280);
    fill_data();
    if (strcmp(mode, "fork") == 0) {
        fork_mode();
    } else if (strcmp(mode, "copy") == 0) {
        copy_mode();
    } else if (strcmp(mode, "cow") == 0) {
        cow_mode();
    } else if (strcmp(mode, "mprotect") == 0) {
        protect_mode();
    } else if (strcmp(mode, "pressure") == 0) {
        pressure_mode();
    } else {
        dprintf(1, "FAIL unknown mode\n");
    }
    dprintf(1, "done mode=%s\n", mode);
    _exit(0);
}

}  // namespace

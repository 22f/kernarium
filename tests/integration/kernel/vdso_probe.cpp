// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest probe: an arm64 task has the guest kernel's vDSO, as native arm64
// Linux gives one, and its entries are plain system calls.
//
// The probe uses no C library, so what it finds is what the kernel put there
// and not what a loader made of it. It reads the auxiliary vector for
// AT_SYSINFO_EHDR and checks:
//
// - the vector names the vDSO, and AT_MINSIGSTKSZ is still beside it where
//   the guest has SME;
// - an ELF shared object is at that address: one read-only, executable
//   segment, a dynamic section with both hash tables and a version
//   definition, a Linux note, and no relocation, so that the image works
//   wherever it is mapped;
// - /proc/self/maps shows it once, as r-xp [vdso], beside the [sigpage];
// - __kernel_clock_gettime, __kernel_gettimeofday and __kernel_clock_getres
//   under LINUX_2.6.39 resolve through the SysV hash and through the GNU
//   hash to the same addresses, and the signal-return entry and a wrong
//   version do not resolve;
// - the unwind tables cover each entry, so an unwinder that stops in one
//   finds its caller through the link register;
// - each entry agrees with the raw system call it stands for: for every
//   clock the vDSO's time lies between two reads by the call, an invalid
//   clock and a bad pointer fail as the call fails, and the resolution is the
//   call's;
// - a fork and an exec (twice) keep the vDSO.
//
// The probe prints one line per case and exits with the number of failures.
// It needs no libc: build it in a guest with
//   c++ -std=c++20 -O2 -ffreestanding -nostdlib -static -no-pie
//       -fno-stack-protector -fno-exceptions -fno-rtti vdso_probe.cpp
// or with the host's clang and ld.lld for aarch64-linux-gnu.
//
// With --exec-stage=N it is the program that an exec of itself starts.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

extern "C" {

void *memcpy(void *destination, const void *source, size_t size);
void *memmove(void *destination, const void *source, size_t size);
void *memset(void *destination, int byte, size_t size);
int memcmp(const void *left, const void *right, size_t size);

// The compiler may emit these calls whatever the flags; the empty asm keeps
// it from turning the loops into calls to themselves.
void *memcpy(void *destination, const void *source, size_t size)
{
	auto *to = static_cast<unsigned char *>(destination);
	auto *from = static_cast<const unsigned char *>(source);
	while (size--) {
		*to++ = *from++;
		asm volatile("" ::: "memory");
	}
	return destination;
}

void *memmove(void *destination, const void *source, size_t size)
{
	auto *to = static_cast<unsigned char *>(destination);
	auto *from = static_cast<const unsigned char *>(source);
	if (to < from) {
		while (size--) {
			*to++ = *from++;
			asm volatile("" ::: "memory");
		}
	} else {
		to += size;
		from += size;
		while (size--) {
			*--to = *--from;
			asm volatile("" ::: "memory");
		}
	}
	return destination;
}

void *memset(void *destination, int byte, size_t size)
{
	auto *to = static_cast<unsigned char *>(destination);
	while (size--) {
		*to++ = static_cast<unsigned char>(byte);
		asm volatile("" ::: "memory");
	}
	return destination;
}

int memcmp(const void *left, const void *right, size_t size)
{
	auto *a = static_cast<const unsigned char *>(left);
	auto *b = static_cast<const unsigned char *>(right);
	for (; size; size--, a++, b++)
		if (*a != *b)
			return *a < *b ? -1 : 1;
	return 0;
}

}

namespace {

// ---- System calls (the arm64 numbers) -------------------------------------

constexpr long nr_openat = 56;
constexpr long nr_close = 57;
constexpr long nr_read = 63;
constexpr long nr_write = 64;
constexpr long nr_exit_group = 94;
constexpr long nr_clock_gettime = 113;
constexpr long nr_clock_getres = 114;
constexpr long nr_gettimeofday = 169;
constexpr long nr_getpid = 172;
constexpr long nr_clone = 220;
constexpr long nr_execve = 221;
constexpr long nr_mmap = 222;
constexpr long nr_munmap = 215;
constexpr long nr_mremap = 216;
constexpr long nr_mprotect = 226;
constexpr long nr_wait4 = 260;

constexpr long error_perm = 1;
constexpr long error_fault = 14;
constexpr long error_inval = 22;

[[gnu::noinline]] long syscall6(long number, long a0 = 0, long a1 = 0,
				long a2 = 0, long a3 = 0, long a4 = 0,
				long a5 = 0)
{
	register long x8 asm("x8") = number;
	register long x0 asm("x0") = a0;
	register long x1 asm("x1") = a1;
	register long x2 asm("x2") = a2;
	register long x3 asm("x3") = a3;
	register long x4 asm("x4") = a4;
	register long x5 asm("x5") = a5;

	asm volatile("svc #0"
		     : "+r"(x0)
		     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
		     : "memory", "cc");
	return x0;
}

// ---- Output ---------------------------------------------------------------

constexpr size_t line_capacity = 512;

void put(const char *text, size_t size)
{
	while (size) {
		long written = syscall6(nr_write, 1, reinterpret_cast<long>(text),
					static_cast<long>(size));
		if (written <= 0)
			return;
		text += written;
		size -= static_cast<size_t>(written);
	}
}

size_t length(const char *text)
{
	size_t size = 0;
	while (text[size])
		size++;
	return size;
}

// A small printf: %s %c %d %ld %lu %lx %%, with a zero-padded width for the
// hexadecimal one (%.16lx).
void say(const char *format, ...)
{
	char line[line_capacity];
	size_t at = 0;
	va_list arguments;

	va_start(arguments, format);
	auto emit = [&](char c) {
		if (at < sizeof(line) - 1)
			line[at++] = c;
	};
	auto digits = [&](unsigned long value, unsigned base, int width) {
		char reversed[24];
		int count = 0;
		do {
			unsigned digit = static_cast<unsigned>(value % base);
			reversed[count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
			value /= base;
		} while (value);
		for (int pad = count; pad < width; pad++)
			emit('0');
		while (count)
			emit(reversed[--count]);
	};
	for (const char *p = format; *p; p++) {
		if (*p != '%') {
			emit(*p);
			continue;
		}
		p++;
		int width = 0;
		if (*p == '.') {
			p++;
			while (*p >= '0' && *p <= '9')
				width = width * 10 + (*p++ - '0');
		}
		bool is_long = false;
		if (*p == 'l') {
			is_long = true;
			p++;
		}
		switch (*p) {
		case 's': {
			const char *text = va_arg(arguments, const char *);
			while (*text)
				emit(*text++);
			break;
		}
		case 'c':
			emit(static_cast<char>(va_arg(arguments, int)));
			break;
		case 'd': {
			long value = is_long ? va_arg(arguments, long) : va_arg(arguments, int);
			if (value < 0) {
				emit('-');
				digits(0UL - static_cast<unsigned long>(value), 10, 0);
			} else {
				digits(static_cast<unsigned long>(value), 10, 0);
			}
			break;
		}
		case 'u':
			digits(is_long ? va_arg(arguments, unsigned long) : va_arg(arguments, unsigned), 10, 0);
			break;
		case 'x':
			digits(is_long ? va_arg(arguments, unsigned long) : va_arg(arguments, unsigned), 16, width);
			break;
		case '%':
			emit('%');
			break;
		default:
			emit('?');
			break;
		}
	}
	va_end(arguments);
	put(line, at);
}

int failures;
// A forked or exec'd copy reports only what fails.
bool quiet;
constexpr int sealed_status = 40;

#define CHECK(condition, ...)                                     \
	do {                                                      \
		if (condition) {                                  \
			if (!quiet) {                             \
				say("ok   ");                     \
				say(__VA_ARGS__);                 \
				say("\n");                        \
			}                                         \
		} else {                                          \
			failures++;                               \
			say("FAIL ");                             \
			say(__VA_ARGS__);                         \
			say("\n");                                \
		}                                                 \
	} while (0)

// ---- ELF (the 64-bit little-endian parts the vDSO uses) -------------------

struct Ehdr {
	unsigned char ident[16];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint64_t entry;
	uint64_t phoff;
	uint64_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phentsize;
	uint16_t phnum;
	uint16_t shentsize;
	uint16_t shnum;
	uint16_t shstrndx;
};

struct Phdr {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
};

struct Dyn {
	int64_t tag;
	uint64_t value;
};

struct Sym {
	uint32_t name;
	unsigned char info;
	unsigned char other;
	uint16_t shndx;
	uint64_t value;
	uint64_t size;
};

struct Verdef {
	uint16_t version;
	uint16_t flags;
	uint16_t ndx;
	uint16_t cnt;
	uint32_t hash;
	uint32_t aux;
	uint32_t next;
};

struct Verdaux {
	uint32_t name;
	uint32_t next;
};

struct Nhdr {
	uint32_t namesz;
	uint32_t descsz;
	uint32_t type;
};

constexpr uint32_t pt_load = 1;
constexpr uint32_t pt_dynamic = 2;
constexpr uint32_t pt_interp = 3;
constexpr uint32_t pt_note = 4;
constexpr uint32_t pt_gnu_eh_frame = 0x6474e550;
constexpr uint32_t pf_x = 1, pf_w = 2, pf_r = 4;

constexpr int64_t dt_null = 0, dt_needed = 1, dt_hash = 4, dt_strtab = 5,
		  dt_symtab = 6, dt_rela = 7, dt_strsz = 10, dt_syment = 11,
		  dt_soname = 14, dt_rel = 17, dt_textrel = 22, dt_jmprel = 23,
		  dt_gnu_hash = 0x6ffffef5, dt_versym = 0x6ffffff0,
		  dt_verdef = 0x6ffffffc, dt_verdefnum = 0x6ffffffd;

constexpr unsigned long at_pagesz = 6;
constexpr unsigned long at_minsigstksz = 51;
constexpr unsigned long at_hwcap2 = 26;
constexpr unsigned long hwcap2_sme = 1UL << 23;
constexpr unsigned long at_sysinfo_ehdr = 33;

constexpr const char *version_name = "LINUX_2.6.39";

// ---- The process's own view ----------------------------------------------

struct Timespec {
	long seconds;
	long nanoseconds;
};

struct Timeval {
	long seconds;
	long microseconds;
};

struct Timezone {
	int minuteswest;
	int dsttime;
};

long argument_count;
char **argument_vector;
char **environment;
unsigned long auxiliary[64][2];
size_t auxiliary_count;

unsigned long auxiliary_value(unsigned long type)
{
	for (size_t i = 0; i < auxiliary_count; i++)
		if (auxiliary[i][0] == type)
			return auxiliary[i][1];
	return 0;
}

bool string_equal(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

bool starts_with(const char *text, const char *prefix)
{
	while (*prefix && *text == *prefix) {
		text++;
		prefix++;
	}
	return !*prefix;
}

// The text of a whole file, up to the capacity.
size_t read_file(const char *path, char *buffer, size_t capacity)
{
	long descriptor = syscall6(nr_openat, -100, reinterpret_cast<long>(path), 0);
	size_t total = 0;

	if (descriptor < 0)
		return 0;
	while (total < capacity) {
		long count = syscall6(nr_read, descriptor,
				      reinterpret_cast<long>(buffer + total),
				      static_cast<long>(capacity - total));
		if (count <= 0)
			break;
		total += static_cast<size_t>(count);
	}
	syscall6(nr_close, descriptor);
	return total;
}

struct Mapping {
	unsigned long start;
	unsigned long end;
	char permissions[5];
	char name[64];
};

constexpr size_t maps_capacity = 96 * 1024;
char maps_text[maps_capacity];

unsigned long parse_hex(const char *&p)
{
	unsigned long value = 0;
	for (;; p++) {
		char c = *p;
		if (c >= '0' && c <= '9')
			value = value * 16 + static_cast<unsigned>(c - '0');
		else if (c >= 'a' && c <= 'f')
			value = value * 16 + static_cast<unsigned>(c - 'a' + 10);
		else
			return value;
	}
}

// The next line of /proc/self/maps from `*cursor`, or false at the end.
bool next_mapping(const char *&cursor, const char *end, Mapping &mapping)
{
	while (cursor < end && *cursor == '\n')
		cursor++;
	if (cursor >= end)
		return false;
	const char *p = cursor;
	mapping.start = parse_hex(p);
	if (*p == '-')
		p++;
	mapping.end = parse_hex(p);
	while (*p == ' ')
		p++;
	for (int i = 0; i < 4 && *p && *p != ' '; i++)
		mapping.permissions[i] = *p++;
	mapping.permissions[4] = 0;
	// offset, device, inode
	for (int field = 0; field < 3; field++) {
		while (*p == ' ')
			p++;
		while (*p && *p != ' ' && *p != '\n')
			p++;
	}
	while (*p == ' ')
		p++;
	size_t at = 0;
	while (p < end && *p != '\n' && at < sizeof(mapping.name) - 1)
		mapping.name[at++] = *p++;
	mapping.name[at] = 0;
	while (p < end && *p != '\n')
		p++;
	cursor = p;
	return true;
}

// ---- The vDSO ------------------------------------------------------------

struct Image {
	unsigned long base;	// AT_SYSINFO_EHDR
	unsigned long length;	// its mapping, from /proc/self/maps
	long bias;		// base minus the first segment's address
	const Ehdr *header;
	const Phdr *segments;
	const Dyn *dynamic;
	const uint32_t *sysv_hash;
	const uint32_t *gnu_hash;
	const Sym *symbols;
	const char *strings;
	size_t string_size;
	const uint16_t *versions;
	const Verdef *definitions;
	unsigned long definition_count;
};

Image image;

template <typename T> const T *at(unsigned long address)
{
	return reinterpret_cast<const T *>(address);
}

// Where the image's pointer `value` (an address in its own, unrelocated
// space) is in memory.
unsigned long in_memory(uint64_t value)
{
	return static_cast<unsigned long>(static_cast<long>(value) + image.bias);
}

bool inside(unsigned long address, unsigned long size)
{
	return address >= image.base && size <= image.length &&
	       address - image.base <= image.length - size;
}

uint32_t elf_hash(const char *name)
{
	uint32_t h = 0;
	for (const auto *p = reinterpret_cast<const unsigned char *>(name); *p; p++) {
		h = (h << 4) + *p;
		uint32_t g = h & 0xf0000000;
		if (g)
			h ^= g >> 24;
		h &= ~g;
	}
	return h;
}

uint32_t gnu_hash(const char *name)
{
	uint32_t h = 5381;
	for (const auto *p = reinterpret_cast<const unsigned char *>(name); *p; p++)
		h = h * 33 + *p;
	return h;
}

const char *dynamic_string(uint32_t offset)
{
	return offset < image.string_size ? image.strings + offset : "";
}

// Whether version index `index` of the symbol table names `wanted`.
bool version_is(uint16_t index, const char *wanted)
{
	index &= 0x7fff;
	const Verdef *definition = image.definitions;
	for (unsigned long i = 0; i < image.definition_count && definition; i++) {
		if (definition->ndx == index && definition->cnt >= 1) {
			const auto *aux = at<Verdaux>(reinterpret_cast<unsigned long>(definition) + definition->aux);
			return string_equal(dynamic_string(aux->name), wanted) &&
			       definition->hash == elf_hash(wanted);
		}
		if (!definition->next)
			break;
		definition = at<Verdef>(reinterpret_cast<unsigned long>(definition) + definition->next);
	}
	return false;
}

bool symbol_matches(uint32_t index, const char *name, const char *version)
{
	const Sym &symbol = image.symbols[index];

	if (!symbol.shndx || !string_equal(dynamic_string(symbol.name), name))
		return false;
	return version_is(image.versions[index], version);
}

// The address of `name` at `version` through the SysV hash table, or 0.
unsigned long find_sysv(const char *name, const char *version, const Sym **found)
{
	uint32_t buckets = image.sysv_hash[0];
	const uint32_t *bucket = image.sysv_hash + 2;
	const uint32_t *chain = bucket + buckets;

	uint32_t chains = image.sysv_hash[1];

	if (!buckets)
		return 0;
	for (uint32_t i = bucket[elf_hash(name) % buckets]; i && i < chains; i = chain[i]) {
		if (symbol_matches(i, name, version)) {
			*found = &image.symbols[i];
			return in_memory(image.symbols[i].value);
		}
	}
	return 0;
}

// The same through the GNU hash table.
unsigned long find_gnu(const char *name, const char *version, const Sym **found)
{
	uint32_t buckets = image.gnu_hash[0];
	uint32_t first = image.gnu_hash[1];
	uint32_t bloom_size = image.gnu_hash[2];
	uint32_t bloom_shift = image.gnu_hash[3];
	const auto *bloom = reinterpret_cast<const uint64_t *>(image.gnu_hash + 4);
	const uint32_t *bucket = reinterpret_cast<const uint32_t *>(bloom + bloom_size);
	const uint32_t *chain = bucket + buckets;
	uint32_t h = gnu_hash(name);

	if (!buckets || !bloom_size)
		return 0;
	uint64_t word = bloom[(h / 64) % bloom_size];
	uint64_t mask = (1ULL << (h % 64)) | (1ULL << ((h >> bloom_shift) % 64));
	if ((word & mask) != mask)
		return 0;
	uint32_t n = bucket[h % buckets];
	if (n < first)
		return 0;
	for (int steps = 0; steps < 64; steps++, n++) {
		uint32_t value = chain[n - first];
		if ((value | 1) == (h | 1) && symbol_matches(n, name, version)) {
			*found = &image.symbols[n];
			return in_memory(image.symbols[n].value);
		}
		if (value & 1)
			return 0;
	}
	return 0;
}

using ClockGettime = long (*)(long, Timespec *);
using Gettimeofday = long (*)(Timeval *, Timezone *);
using ClockGetres = long (*)(long, Timespec *);

ClockGettime vdso_clock_gettime;
Gettimeofday vdso_gettimeofday;
ClockGetres vdso_clock_getres;

// ---- The cases ------------------------------------------------------------

bool read_maps_and_find(unsigned long address, Mapping &out, int &count_named)
{
	size_t size = read_file("/proc/self/maps", maps_text, maps_capacity - 1);
	const char *cursor = maps_text;
	const char *end = maps_text + size;
	Mapping mapping;
	bool found = false;

	count_named = 0;
	while (next_mapping(cursor, end, mapping)) {
		if (string_equal(mapping.name, "[vdso]"))
			count_named++;
		if (address >= mapping.start && address < mapping.end) {
			out = mapping;
			found = true;
		}
	}
	return found;
}

bool collect_auxiliary(const char *source, const char *label)
{
	// The kernel's copy, as a task reads it from /proc.
	unsigned long words[130];
	size_t size = read_file(source, reinterpret_cast<char *>(words), sizeof(words));

	(void)label;
	if (size < 16)
		return false;
	size_t count = size / sizeof(unsigned long) / 2;
	size_t kept = 0;
	for (size_t i = 0; i < count && kept < 64; i++) {
		auxiliary[kept][0] = words[2 * i];
		auxiliary[kept][1] = words[2 * i + 1];
		kept++;
		if (!words[2 * i])
			break;
	}
	auxiliary_count = kept;
	return true;
}

void check_auxiliary()
{
	unsigned long from_stack = auxiliary_value(at_sysinfo_ehdr);
	unsigned long page = auxiliary_value(at_pagesz);
	unsigned long minsigstksz = auxiliary_value(at_minsigstksz);
	const auto saved_count = auxiliary_count;
	unsigned long saved[64][2];

	for (size_t i = 0; i < saved_count; i++) {
		saved[i][0] = auxiliary[i][0];
		saved[i][1] = auxiliary[i][1];
	}
	bool read = collect_auxiliary("/proc/self/auxv", "proc");
	unsigned long from_proc = auxiliary_value(at_sysinfo_ehdr);
	for (size_t i = 0; i < saved_count; i++) {
		auxiliary[i][0] = saved[i][0];
		auxiliary[i][1] = saved[i][1];
	}
	auxiliary_count = saved_count;

	CHECK(from_stack != 0, "AT_SYSINFO_EHDR is in the auxiliary vector (0x%lx)", from_stack);
	CHECK(page == 4096 && (from_stack & (page - 1)) == 0,
	      "the address is page aligned (page size %lu)", page);
	CHECK(read && from_proc == from_stack,
	      "/proc/self/auxv names the same address (0x%lx)", from_proc);
	// AT_MINSIGSTKSZ is the SME guest's (ARCH_DLINFO adds it beside the vDSO's
	// entry); a guest without SME has none, as before.
	const bool sme = (auxiliary_value(at_hwcap2) & hwcap2_sme) != 0;
	CHECK(!sme || (minsigstksz >= 2048 && minsigstksz <= (1UL << 20)),
	      "AT_MINSIGSTKSZ is still there with SME (%lu, SME %d)", minsigstksz, sme ? 1 : 0);
}

// Find the vDSO's mapping and set up `image`; false if it is not there.
bool locate_image()
{
	Mapping mapping;
	int named = 0;

	image = {};
	image.base = auxiliary_value(at_sysinfo_ehdr);
	if (!image.base)
		return false;
	if (!read_maps_and_find(image.base, mapping, named)) {
		CHECK(false, "a mapping holds the vDSO address");
		return false;
	}
	CHECK(string_equal(mapping.name, "[vdso]") && mapping.start == image.base,
	      "/proc/self/maps names it [vdso] from the vDSO address (%s at 0x%lx)",
	      mapping.name, mapping.start);
	CHECK(string_equal(mapping.permissions, "r-xp"),
	      "it is read-only and executable (%s)", mapping.permissions);
	CHECK(named == 1, "[vdso] appears once (%d)", named);
	CHECK(((mapping.end - mapping.start) & 4095) == 0 && mapping.end > mapping.start,
	      "its length is whole pages (%lu bytes)", mapping.end - mapping.start);
	image.length = mapping.end - mapping.start;
	return true;
}

void check_neighbours()
{
	size_t size = read_file("/proc/self/maps", maps_text, maps_capacity - 1);
	const char *cursor = maps_text;
	const char *end = maps_text + size;
	Mapping mapping;
	int sigpages = 0;
	int overlaps = 0;
	int vvars = 0;

	while (next_mapping(cursor, end, mapping)) {
		if (string_equal(mapping.name, "[sigpage]"))
			sigpages++;
		if (string_equal(mapping.name, "[vvar]") || string_equal(mapping.name, "[vvar_vclock]"))
			vvars++;
		if (!string_equal(mapping.name, "[vdso]") && mapping.start < image.base + image.length &&
		    image.base < mapping.end)
			overlaps++;
	}
	CHECK(sigpages == 1, "the [sigpage] is still mapped once (%d)", sigpages);
	CHECK(overlaps == 0, "nothing else overlaps the [vdso] (%d)", overlaps);
	CHECK(vvars == 0, "there is no data page: every entry is a system call (%d)", vvars);
}

bool check_header()
{
	const auto *header = at<Ehdr>(image.base);
	const unsigned char expected[] = { 0x7f, 'E', 'L', 'F', 2, 1, 1 };
	bool ok = true;

	image.header = header;
	CHECK(memcmp(header->ident, expected, sizeof(expected)) == 0,
	      "ELF magic, 64-bit, little endian, version 1");
	CHECK(header->type == 3 && header->machine == 183 && header->version == 1,
	      "a shared object for AArch64 (type %d, machine %d)", header->type, header->machine);
	CHECK(header->ehsize == sizeof(Ehdr) && header->phentsize == sizeof(Phdr) && header->phnum >= 3 &&
	      header->phoff + header->phnum * sizeof(Phdr) <= image.length,
	      "its program headers are in the mapping (%d of them)", header->phnum);
	if (header->phoff + header->phnum * sizeof(Phdr) > image.length || header->phentsize != sizeof(Phdr))
		return false;
	image.segments = at<Phdr>(image.base + header->phoff);

	int loads = 0, dynamics = 0, notes = 0, frames = 0, interpreters = 0, writable = 0;
	bool load_ok = true;
	const Phdr *load = nullptr;
	for (int i = 0; i < header->phnum; i++) {
		const Phdr &segment = image.segments[i];
		if (segment.type == pt_load) {
			loads++;
			load = &segment;
			load_ok = segment.vaddr == 0 && segment.offset == 0 && segment.filesz == segment.memsz &&
				  segment.memsz <= image.length && (segment.flags & (pf_r | pf_x)) == (pf_r | pf_x);
			writable += (segment.flags & pf_w) != 0;
		} else if (segment.type == pt_dynamic) {
			dynamics++;
			writable += (segment.flags & pf_w) != 0;
			image.dynamic = at<Dyn>(image.base + segment.vaddr);
		} else if (segment.type == pt_note) {
			notes++;
		} else if (segment.type == pt_gnu_eh_frame) {
			frames++;
		} else if (segment.type == pt_interp) {
			interpreters++;
		}
	}
	CHECK(loads == 1 && load_ok,
	      "one read-only, executable segment from address 0 holds the image");
	CHECK(writable == 0 && interpreters == 0, "no segment is writable and none names an interpreter");
	CHECK(dynamics == 1 && notes == 1 && frames == 1, "a dynamic section, a note and the unwind table are there");
	if (!load || !dynamics)
		return false;
	image.bias = static_cast<long>(image.base) - static_cast<long>(load->vaddr);
	ok = load_ok;
	return ok;
}

void check_note()
{
	bool linux_note = false;
	uint32_t code = 0;

	for (int i = 0; i < image.header->phnum; i++) {
		const Phdr &segment = image.segments[i];
		if (segment.type != pt_note)
			continue;
		unsigned long cursor = image.base + segment.vaddr;
		unsigned long end = cursor + segment.filesz;
		while (cursor + sizeof(Nhdr) <= end && inside(cursor, sizeof(Nhdr))) {
			const auto *note = at<Nhdr>(cursor);
			const char *name = at<char>(cursor + sizeof(Nhdr));
			if (note->type == 0 && starts_with(name, "Linux") && note->descsz == 4) {
				code = *at<uint32_t>(cursor + sizeof(Nhdr) + ((note->namesz + 3) & ~3U));
				// KERNEL_VERSION(6, 18, N), N clamped to 255.
				linux_note = (code >> 16) == 6 && ((code >> 8) & 255) == 18;
			}
			cursor += sizeof(Nhdr) + ((note->namesz + 3) & ~3U) + ((note->descsz + 3) & ~3U);
		}
	}
	CHECK(linux_note, "the Linux note carries the kernel version (0x%x)", code);
}

bool check_dynamic()
{
	unsigned long hash = 0, gnu = 0, symtab = 0, strtab = 0, versym = 0, verdef = 0, soname = 0;
	unsigned long strsz = 0, syment = 0, verdefnum = 0;
	int relocations = 0, needed = 0;

	for (const Dyn *entry = image.dynamic; inside(reinterpret_cast<unsigned long>(entry), sizeof(Dyn)) &&
	     entry->tag != dt_null; entry++) {
		switch (entry->tag) {
		case dt_hash: hash = entry->value; break;
		case dt_gnu_hash: gnu = entry->value; break;
		case dt_symtab: symtab = entry->value; break;
		case dt_strtab: strtab = entry->value; break;
		case dt_strsz: strsz = entry->value; break;
		case dt_syment: syment = entry->value; break;
		case dt_versym: versym = entry->value; break;
		case dt_verdef: verdef = entry->value; break;
		case dt_verdefnum: verdefnum = entry->value; break;
		case dt_soname: soname = entry->value; break;
		case dt_needed: needed++; break;
		case dt_rela: case dt_rel: case dt_jmprel: case dt_textrel: relocations++; break;
		default: break;
		}
	}
	CHECK(hash && gnu && symtab && strtab && strsz && versym && verdef && verdefnum &&
	      syment == sizeof(Sym),
	      "the dynamic section has the SysV and GNU hash tables, symbols, strings and versions");
	CHECK(relocations == 0 && needed == 0,
	      "it has no relocation and needs no other object (%d, %d)", relocations, needed);
	if (!(hash && gnu && symtab && strtab && strsz && versym && verdef && verdefnum))
		return false;
	if (!inside(in_memory(hash), 8) || !inside(in_memory(gnu), 16) || !inside(in_memory(symtab), sizeof(Sym)) ||
	    !inside(in_memory(strtab), strsz) || !inside(in_memory(versym), 2) || !inside(in_memory(verdef), 20)) {
		CHECK(false, "every table is inside the mapping");
		return false;
	}
	image.sysv_hash = at<uint32_t>(in_memory(hash));
	image.gnu_hash = at<uint32_t>(in_memory(gnu));
	image.symbols = at<Sym>(in_memory(symtab));
	image.strings = at<char>(in_memory(strtab));
	image.string_size = strsz;
	image.versions = at<uint16_t>(in_memory(versym));
	image.definitions = at<Verdef>(in_memory(verdef));
	image.definition_count = verdefnum;
	CHECK(soname && string_equal(dynamic_string(static_cast<uint32_t>(soname)), "linux-vdso.so.1"),
	      "its soname is linux-vdso.so.1");
	return true;
}

unsigned long resolved[3];

void check_symbols()
{
	static const char *const names[] = {
		"__kernel_clock_gettime", "__kernel_gettimeofday", "__kernel_clock_getres",
	};

	for (int i = 0; i < 3; i++) {
		const Sym *by_sysv = nullptr, *by_gnu = nullptr;
		unsigned long sysv = find_sysv(names[i], version_name, &by_sysv);
		unsigned long gnu = find_gnu(names[i], version_name, &by_gnu);
		bool shape = by_sysv && (by_sysv->info >> 4) == 1 && (by_sysv->info & 15) == 2 && by_sysv->size == 12 &&
			     sysv >= image.base + 0 && inside(sysv, 12);
		CHECK(sysv && gnu && sysv == gnu,
		      "%s@%s resolves through the SysV and the GNU hash to one address (0x%lx, 0x%lx)",
		      names[i], version_name, sysv, gnu);
		CHECK(shape, "%s is a global function of 12 bytes inside the mapping", names[i]);
		resolved[i] = sysv && sysv == gnu ? sysv : 0;
		const Sym *unused = nullptr;
		CHECK(!find_sysv(names[i], "LINUX_2.6", &unused) && !find_gnu(names[i], "LINUX_2.6", &unused),
		      "%s does not resolve at another version", names[i]);
	}
	const Sym *unused = nullptr;
	CHECK(!find_sysv("__kernel_rt_sigreturn", version_name, &unused) &&
	      !find_gnu("__kernel_rt_sigreturn", version_name, &unused),
	      "__kernel_rt_sigreturn is not exported (a signal returns through the [sigpage])");
	CHECK(!find_sysv("__kernel_getrandom", version_name, &unused) &&
	      !find_gnu("__kernel_getrandom", version_name, &unused),
	      "__kernel_getrandom is not exported");
	CHECK(!find_sysv("clock_gettime", version_name, &unused) && !find_gnu("clock_gettime", version_name, &unused),
	      "no name without the __kernel_ prefix is exported");
	vdso_clock_gettime = reinterpret_cast<ClockGettime>(resolved[0]);
	vdso_gettimeofday = reinterpret_cast<Gettimeofday>(resolved[1]);
	vdso_clock_getres = reinterpret_cast<ClockGetres>(resolved[2]);
}

// One signed 32-bit value of a table entry, and an unsigned LEB128.
int32_t read_s32(unsigned long address)
{
	return *at<int32_t>(address);
}

unsigned long read_uleb(unsigned long &cursor)
{
	unsigned long value = 0;
	unsigned shift = 0;
	unsigned char byte;
	do {
		byte = *at<unsigned char>(cursor++);
		value |= static_cast<unsigned long>(byte & 0x7f) << shift;
		shift += 7;
	} while (byte & 0x80);
	return value;
}

void check_unwind()
{
	const Phdr *segment = nullptr;
	for (int i = 0; i < image.header->phnum; i++)
		if (image.segments[i].type == pt_gnu_eh_frame)
			segment = &image.segments[i];
	if (!segment || !inside(image.base + segment->vaddr, 12)) {
		CHECK(false, "the unwind table header is in the mapping");
		return;
	}
	unsigned long table = image.base + segment->vaddr;
	const unsigned char *header = at<unsigned char>(table);
	// version 1, eh_frame_ptr pcrel|sdata4, fde_count udata4, table datarel|sdata4
	bool format = header[0] == 1 && header[1] == 0x1b && header[2] == 0x03 && header[3] == 0x3b;
	CHECK(format, "the unwind table header has the encodings this probe reads (%x %x %x %x)",
	      header[0], header[1], header[2], header[3]);
	if (!format)
		return;
	unsigned long frames = table + 4 + read_s32(table + 4);
	uint32_t count = *at<uint32_t>(table + 8);
	CHECK(count >= 3 && inside(table + 12, count * 8UL) && inside(frames, 16),
	      "the table lists %u frame descriptions inside the mapping", count);
	if (count < 3 || !inside(table + 12, count * 8UL) || !inside(frames, 16))
		return;

	// The common information entry that starts the frames: CFA is the stack
	// pointer, and the return address is in x30.
	unsigned long cie = frames;
	uint32_t cie_length = *at<uint32_t>(cie);
	uint32_t cie_id = *at<uint32_t>(cie + 4);
	unsigned long p = cie + 8;
	unsigned char cie_version = *at<unsigned char>(p++);
	const char *augmentation = at<char>(p);
	p += length(augmentation) + 1;
	read_uleb(p);			// code alignment
	read_uleb(p);			// data alignment (signed, one byte here)
	unsigned long return_column = cie_version == 1 ? *at<unsigned char>(p++) : read_uleb(p);
	bool zr = string_equal(augmentation, "zR");
	unsigned long augmentation_size = zr ? read_uleb(p) : 0;
	unsigned char pointer_format = zr ? *at<unsigned char>(p) : 0;
	p += augmentation_size;
	const unsigned char *initial = at<unsigned char>(p);
	CHECK(cie_length != 0 && cie_length != 0xffffffffU && cie_id == 0 && cie_version == 1 && zr &&
	      return_column == 30 && pointer_format == 0x1b,
	      "the frames start with a CIE: return address in x30, pc-relative pointers");
	// DW_CFA_def_cfa r31 (sp) +0
	CHECK(initial[0] == 0x0c && initial[1] == 31 && initial[2] == 0,
	      "the CIE sets the CFA to the stack pointer, which the entries never move");

	for (int i = 0; i < 3; i++) {
		bool covered = false;
		bool sorted = true;
		uint64_t previous = 0;
		for (uint32_t n = 0; n < count; n++) {
			unsigned long entry = table + 12 + n * 8UL;
			long start = static_cast<long>(table) + read_s32(entry);
			unsigned long fde = table + read_s32(entry + 4);
			if (static_cast<uint64_t>(start) < previous)
				sorted = false;
			previous = static_cast<uint64_t>(start);
			if (!inside(fde, 16))
				continue;
			uint32_t fde_pointer = *at<uint32_t>(fde + 4);
			unsigned long location = fde + 8;
			long pc_begin = static_cast<long>(location) + read_s32(location);
			uint32_t pc_range = *at<uint32_t>(location + 4);
			// The FDE's own CIE pointer leads back to the CIE.
			bool to_cie = fde + 4 - fde_pointer == cie;
			if (to_cie && pc_begin == static_cast<long>(resolved[i]) && pc_range == 12 &&
			    start == pc_begin)
				covered = true;
			if (!sorted)
				break;
		}
		CHECK(covered && sorted, "an unwind frame covers entry %d exactly, and the table is sorted", i);
	}
}

bool pointer_fails_alike(long clock, Timespec *pointer)
{
	long by_call = syscall6(nr_clock_gettime, clock, reinterpret_cast<long>(pointer));
	long by_vdso = vdso_clock_gettime(clock, pointer);

	return by_call == by_vdso;
}

bool later_or_equal(const Timespec &a, const Timespec &b)
{
	return b.seconds > a.seconds || (b.seconds == a.seconds && b.nanoseconds >= a.nanoseconds);
}

bool later_or_equal(const Timeval &a, const Timeval &b)
{
	return b.seconds > a.seconds || (b.seconds == a.seconds && b.microseconds >= a.microseconds);
}

long seconds_between(const Timespec &a, const Timespec &b)
{
	return b.seconds - a.seconds;
}

void check_clocks()
{
	static const char *const names[] = {
		"REALTIME", "MONOTONIC", "PROCESS_CPUTIME_ID", "THREAD_CPUTIME_ID", "MONOTONIC_RAW",
		"REALTIME_COARSE", "MONOTONIC_COARSE", "BOOTTIME", "REALTIME_ALARM", "BOOTTIME_ALARM",
		"SGI_CYCLE", "TAI",
	};
	int usable = 0;

	for (long clock = 0; clock <= 11; clock++) {
		Timespec before = {}, from_vdso = {}, after = {};
		bool agree = false;

		// A stalled guest may take long between two reads; try again.
		for (int attempt = 0; attempt < 5 && !agree; attempt++) {
			long first = syscall6(nr_clock_gettime, clock, reinterpret_cast<long>(&before));
			long middle = vdso_clock_gettime(clock, &from_vdso);
			long last = syscall6(nr_clock_gettime, clock, reinterpret_cast<long>(&after));

			if (first < 0 || last < 0) {
				// The call refuses the clock, as for SGI_CYCLE, or the task
				// (an alarm clock needs CAP_WAKE_ALARM): so does the entry.
				agree = middle == first && first == last;
				if (agree)
					break;
				continue;
			}
			agree = middle == 0 && later_or_equal(before, from_vdso) && later_or_equal(from_vdso, after) &&
				seconds_between(before, after) < 10 && from_vdso.nanoseconds >= 0 &&
				from_vdso.nanoseconds < 1000000000;
		}
		long refused = syscall6(nr_clock_gettime, clock, reinterpret_cast<long>(&after));
		if (refused >= 0)
			usable++;
		CHECK(agree, "clock %ld (%s): %s", clock, names[clock],
		      refused >= 0 ? "the entry's time lies between two reads by the call"
				   : "the entry refuses as the call does");
	}
	CHECK(usable >= 8, "at least eight of the twelve clocks work (%d)", usable);

	// Invalid clocks fail as the call fails.
	static const long invalid[] = { 12, 13, 16, 100, 0x7fffffff, -1, -2 };
	bool all_same = true, all_inval = true;
	long bad_clock = 0, bad_result = 0;
	for (long clock : invalid) {
		Timespec a = {}, b = {};
		long by_call = syscall6(nr_clock_gettime, clock, reinterpret_cast<long>(&a));
		long by_vdso = vdso_clock_gettime(clock, &b);
		if (by_call != by_vdso)
			all_same = false;
		// A negative id names a dynamic or a CPU-time clock, and ids 16 to 23
		// the auxiliary clocks (6.17 and later), whose errors are the call's own
		// (no such process, no such device).
		if (clock >= 0 && !(clock >= 16 && clock <= 23) && by_call != -error_inval) {
			all_inval = false;
			bad_clock = clock;
			bad_result = by_call;
		}
	}
	CHECK(all_same, "an invalid clock id fails as the call fails");
	CHECK(all_inval, "and the failure of an unassigned id is EINVAL (clock %ld gave %ld)", bad_clock,
	      bad_result);

	// The calling process's own CPU-time clock, by its dynamic id.
	long pid = syscall6(nr_getpid);
	long own = static_cast<long>(static_cast<int>((~static_cast<unsigned>(pid) << 3) | 2));
	Timespec own_call = {}, own_vdso = {}, own_after = {};
	long r1 = syscall6(nr_clock_gettime, own, reinterpret_cast<long>(&own_call));
	long r2 = vdso_clock_gettime(own, &own_vdso);
	long r3 = syscall6(nr_clock_gettime, own, reinterpret_cast<long>(&own_after));
	CHECK(r1 == r2 && r2 == r3 && (r1 < 0 || (later_or_equal(own_call, own_vdso) && later_or_equal(own_vdso, own_after))),
	      "the process's CPU clock by its dynamic id agrees (%ld, %ld, %ld)", r1, r2, r3);

	// A bad pointer fails as the call fails.
	bool fault_same = true, fault_efault = true;
	long mapped = syscall6(nr_mmap, 0, 3 * 4096, 3 /* PROT_READ|PROT_WRITE */,
			       0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
	Timespec *pointers[6] = {
		nullptr, reinterpret_cast<Timespec *>(1), reinterpret_cast<Timespec *>(-4096L),
		reinterpret_cast<Timespec *>(0xffff000000000000UL), nullptr, nullptr,
	};
	if (mapped > 0) {
		// A page the task may only read, and a timespec that straddles the
		// end of the mapping into a page it may not touch.
		syscall6(nr_mprotect, mapped, 4096, 1 /* PROT_READ */);
		syscall6(nr_mprotect, mapped + 2 * 4096, 4096, 0 /* PROT_NONE */);
		pointers[4] = reinterpret_cast<Timespec *>(mapped);
		pointers[5] = reinterpret_cast<Timespec *>(mapped + 2 * 4096 - 8);
	}
	for (size_t i = 0; i < sizeof(pointers) / sizeof(pointers[0]); i++) {
		Timespec *pointer = pointers[i];
		if (!pointer && i != 0)
			continue;
		long by_call = syscall6(nr_clock_gettime, 1, reinterpret_cast<long>(pointer));
		long by_vdso = vdso_clock_gettime(1, pointer);
		if (by_call != by_vdso)
			fault_same = false;
		if (by_call != -error_fault)
			fault_efault = false;
	}
	CHECK(fault_same, "a bad pointer fails as the call fails");
	CHECK(fault_efault, "and the failure is EFAULT");
	CHECK(pointer_fails_alike(1, nullptr) && pointer_fails_alike(12, nullptr),
	      "a null pointer fails as the call fails, with a valid clock and with an invalid one");
	if (mapped > 0)
		syscall6(nr_munmap, mapped, 3 * 4096);
}

void check_resolution()
{
	bool same = true;
	int usable = 0;

	for (long clock = 0; clock <= 12; clock++) {
		Timespec by_call = { -1, -1 }, by_vdso = { -1, -1 };
		long a = syscall6(nr_clock_getres, clock, reinterpret_cast<long>(&by_call));
		long b = vdso_clock_getres(clock, &by_vdso);
		if (a != b || (a == 0 && (by_call.seconds != by_vdso.seconds || by_call.nanoseconds != by_vdso.nanoseconds)))
			same = false;
		if (a == 0) {
			usable++;
			if (by_call.seconds < 0 || by_call.nanoseconds <= 0 || by_call.nanoseconds >= 1000000000)
				same = false;
		}
		// A null result pointer is allowed: both succeed or both fail.
		if (syscall6(nr_clock_getres, clock, 0) != vdso_clock_getres(clock, nullptr))
			same = false;
	}
	CHECK(same && usable >= 8, "clock_getres gives the call's resolution for every clock (%d usable)", usable);

	bool fault_same = true;
	static const long bad_pointers[] = { 1L, -4096L };
	for (long bad : bad_pointers) {
		auto *pointer = reinterpret_cast<Timespec *>(bad);
		if (syscall6(nr_clock_getres, 1, bad) != vdso_clock_getres(1, pointer) ||
		    vdso_clock_getres(1, pointer) != -error_fault)
			fault_same = false;
	}
	CHECK(fault_same, "a bad result pointer fails with EFAULT, as the call does");
}

void check_gettimeofday()
{
	bool ordered = false;

	for (int attempt = 0; attempt < 5 && !ordered; attempt++) {
		Timeval before = {}, from_vdso = {}, after = {};
		Timezone zone_call = { -1, -1 }, zone_vdso = { -2, -2 };
		long a = syscall6(nr_gettimeofday, reinterpret_cast<long>(&before), 0);
		long b = vdso_gettimeofday(&from_vdso, &zone_vdso);
		long c = syscall6(nr_gettimeofday, reinterpret_cast<long>(&after), reinterpret_cast<long>(&zone_call));
		ordered = a == 0 && b == 0 && c == 0 && later_or_equal(before, from_vdso) && later_or_equal(from_vdso, after) &&
			  after.seconds - before.seconds < 10 && from_vdso.microseconds >= 0 &&
			  from_vdso.microseconds < 1000000 && zone_call.minuteswest == zone_vdso.minuteswest &&
			  zone_call.dsttime == zone_vdso.dsttime;
	}
	CHECK(ordered, "gettimeofday lies between two reads by the call, with the same zone");
	CHECK(vdso_gettimeofday(nullptr, nullptr) == 0 && syscall6(nr_gettimeofday, 0, 0) == 0,
	      "gettimeofday with no result succeeds as the call does");
	bool fault_same = true;
	static const long bad_pointers[] = { 1L, -4096L };
	for (long bad : bad_pointers) {
		long by_call = syscall6(nr_gettimeofday, bad, 0);
		long by_vdso = vdso_gettimeofday(reinterpret_cast<Timeval *>(bad), nullptr);
		long zone_call = syscall6(nr_gettimeofday, 0, bad);
		long zone_vdso = vdso_gettimeofday(nullptr, reinterpret_cast<Timezone *>(bad));
		if (by_call != by_vdso || by_vdso != -error_fault || zone_call != zone_vdso || zone_vdso != -error_fault)
			fault_same = false;
	}
	CHECK(fault_same, "a bad pointer fails with EFAULT, as the call does");
}

bool check_vdso(bool whole);

// Fork, as clone(SIGCHLD): the child checks the inherited vDSO and ends with
// the number of its failures.
void check_fork()
{
	long child = syscall6(nr_clone, 17 /* SIGCHLD */, 0, 0, 0, 0);

	if (child == 0) {
		int before = failures;
		quiet = true;
		bool found = check_vdso(false);
		syscall6(nr_exit_group, !found ? 99 : (failures - before > 90 ? 90 : failures - before));
	}
	int status = -1;
	long waited = syscall6(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
	CHECK(child > 0 && waited == child && (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0,
	      "a forked child finds the same vDSO and its entries work (status 0x%x)", status);
}

// Whether /proc/self/smaps marks the [vdso] mapping sealed (VmFlags "sl"), as
// a kernel built with CONFIG_MSEAL_SYSTEM_MAPPINGS does, as Android's is.
bool vdso_is_sealed()
{
	size_t size = read_file("/proc/self/smaps", maps_text, maps_capacity - 1);
	const char *cursor = maps_text;
	const char *end = maps_text + size;
	bool inside_vdso = false;

	while (cursor < end) {
		const char *line_end = cursor;
		while (line_end < end && *line_end != '\n')
			line_end++;
		size_t length = static_cast<size_t>(line_end - cursor);
		// A mapping's first line starts with its address, in lower-case
		// hexadecimal; the lines after it start with a capital letter.
		if (length > 0 && ((cursor[0] >= '0' && cursor[0] <= '9') || (cursor[0] >= 'a' && cursor[0] <= 'f')))
			inside_vdso = length >= 6 && line_end[-1] == ']' && line_end[-2] == 'o' && line_end[-3] == 's' &&
				      line_end[-4] == 'd' && line_end[-5] == 'v' && line_end[-6] == '[';
		else if (inside_vdso && length > 8 && cursor[0] == 'V' && cursor[1] == 'm' && cursor[2] == 'F') {
			for (const char *p = cursor + 8; p + 1 < line_end; p++)
				if (p[-1] == ' ' && p[0] == 's' && p[1] == 'l' && (p + 2 >= line_end || p[2] == ' '))
					return true;
			return false;
		}
		cursor = line_end + 1;
	}
	return false;
}

// The vDSO moved with mremap (as a checkpoint-and-restore tool or a sandbox
// moves it) is where the process put it: the maps name it there and its entry
// works from the new address. A kernel that seals its system mappings refuses
// the move and marks the mapping sealed, so that is the other right answer.
// The child does it, to keep this process's vDSO.
void check_mremap()
{
	long child = syscall6(nr_clone, 17, 0, 0, 0, 0);

	if (child == 0) {
		quiet = true;
		long target = syscall6(nr_mmap, 0, static_cast<long>(image.length), 0 /* PROT_NONE */,
				       0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
		long moved = target > 0 ? syscall6(nr_mremap, static_cast<long>(image.base),
						   static_cast<long>(image.length), static_cast<long>(image.length),
						   3 /* MREMAP_MAYMOVE|MREMAP_FIXED */, target)
					: -1;
		int code = 0;
		Mapping mapping;
		int named = 0;

		if (moved == -error_perm)
			code = vdso_is_sealed() ? sealed_status : 5;
		else if (moved != target)
			code = 1;
		else if (!read_maps_and_find(static_cast<unsigned long>(target), mapping, named) ||
			 !string_equal(mapping.name, "[vdso]") || named != 1)
			code = 2;
		else if (read_maps_and_find(image.base, mapping, named) && string_equal(mapping.name, "[vdso]"))
			code = 3;
		else {
			auto entry = reinterpret_cast<ClockGettime>(resolved[0] - image.base + static_cast<unsigned long>(target));
			Timespec now = {};
			code = entry(1, &now) == 0 && now.seconds >= 0 ? 0 : 4;
		}
		syscall6(nr_exit_group, code);
	}
	int status = -1;
	long waited = syscall6(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
	int code = (status >> 8) & 0xff;
	CHECK(child > 0 && waited == child && (status & 0x7f) == 0 && (code == 0 || code == sealed_status),
	      "mremap of the vDSO moves it and the entry works there, or the sealed mapping refuses (status 0x%x)", status);
}

// Exec the probe again, `stage` times over: each image is a new address
// space, with a vDSO of its own.
void check_exec(int stage)
{
	long child = syscall6(nr_clone, 17, 0, 0, 0, 0);

	if (child == 0) {
		char option[] = "--exec-stage=0";
		char *arguments[] = { argument_vector[0], option, nullptr };
		option[13] = static_cast<char>('0' + stage);
		syscall6(nr_execve, reinterpret_cast<long>("/proc/self/exe"), reinterpret_cast<long>(arguments),
			 reinterpret_cast<long>(environment));
		say("exec failed\n");
		syscall6(nr_exit_group, 98);
	}
	int status = -1;
	long waited = syscall6(nr_wait4, child, reinterpret_cast<long>(&status), 0, 0);
	CHECK(child > 0 && waited == child && (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0,
	      "the program after an exec (stage %d) has a vDSO of its own that works (status 0x%x)", stage, status);
}

// Whether the vDSO is there; the cases that need it run only when it is.
bool check_vdso(bool whole)
{
	check_auxiliary();
	if (!locate_image())
		return false;
	if (whole)
		check_neighbours();
	if (!check_header() || !check_dynamic())
		return false;
	if (whole)
		check_note();
	check_symbols();
	if (whole)
		check_unwind();
	if (!resolved[0] || !resolved[1] || !resolved[2])
		return false;
	check_clocks();
	check_resolution();
	check_gettimeofday();
	return true;
}

int run(int exec_stage, bool staged)
{
	auxiliary_count = 0;
	// The auxiliary vector sits after the environment on the initial stack.
	char **cursor = environment;
	while (*cursor)
		cursor++;
	cursor++;
	auto *entries = reinterpret_cast<unsigned long *>(cursor);
	for (; entries[0] && auxiliary_count < 64; entries += 2) {
		auxiliary[auxiliary_count][0] = entries[0];
		auxiliary[auxiliary_count][1] = entries[1];
		auxiliary_count++;
	}

	bool found = check_vdso(!staged);
	CHECK(found, "the vDSO is there and its entries work");
	if (!found)
		return failures ? failures : 1;
	if (staged) {
		if (exec_stage > 0)
			check_exec(exec_stage - 1);
		return failures;
	}
	check_fork();
	check_mremap();
	check_exec(2);
	return failures;
}

}

extern "C" [[noreturn]] void probe_start(long *stack)
{
	argument_count = stack[0];
	argument_vector = reinterpret_cast<char **>(stack + 1);
	environment = argument_vector + argument_count + 1;

	int stage = -1;
	if (argument_count >= 2 && starts_with(argument_vector[1], "--exec-stage="))
		stage = argument_vector[1][13] - '0';
	quiet = stage >= 0;
	int status = run(stage < 0 ? 0 : stage, stage >= 0);
	syscall6(nr_exit_group, status > 255 ? 255 : status);
	for (;;)
		;
}

asm(".text\n"
    ".globl _start\n"
    ".type _start, %function\n"
    "_start:\n"
    "	mov	x29, xzr\n"
    "	mov	x30, xzr\n"
    "	mov	x0, sp\n"
    "	bl	probe_start\n");

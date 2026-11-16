// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../shared_backing.hpp"
#include "writer_location.hpp"
#if DARWIN_UML_X86_64_GUEST
#include "frame_domain.hpp"
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

struct stat;

namespace uml::memory {

class MappedBacking final {
public:
	static std::shared_ptr<MappedBacking> create(int fd,
		const struct stat &status);
#if DARWIN_UML_X86_64_GUEST
	/* The same, with the frame accounting's metadata for the backing in the
	 * heap of `domain`, the domain of the address space that makes it. */
	static std::shared_ptr<MappedBacking> create(int fd,
		const struct stat &status,
		std::shared_ptr<FrameAccountingDomain> domain);
#endif

	MappedBacking(const MappedBacking &) = delete;
	MappedBacking &operator=(const MappedBacking &) = delete;
	~MappedBacking();

	[[nodiscard]] std::byte *at(std::uint64_t offset) const noexcept;
	/* The address of offset 0: the inverse of at(), so the offset of an
	 * address inside the backing is the address less base(). */
	[[nodiscard]] std::byte *base() const noexcept
	{
		return static_cast<std::byte *>(mapping_.data());
	}
	[[nodiscard]] std::size_t size() const noexcept { return size_; }
	/* Code-page records are shared by every virtual alias of one physical
	 * backing page. Their addresses remain stable for the backing lifetime so
	 * the static-memory TLB can cache the opaque handle between mapping changes.
	 *
	 * Every translating vCPU asks for one before it reads a block's bytes and
	 * again whenever it revalidates a cached translation, so the answer is read
	 * far more often than a page is first seen. Because a record is never
	 * destroyed or moved, a lazily filled two-level index of atomic pointers
	 * answers the repeat without a lock; only a page seen for the first time
	 * takes one.
	 */
	[[nodiscard]] void *code_page_handle(std::uint64_t offset) noexcept;
	[[nodiscard]] static bool load_code_page_stamp(const void *handle,
		std::uint64_t &identity, std::uint64_t &version) noexcept;
	static void invalidate_code_page(void *handle) noexcept;
	/* Moves the stamp of the backing page at `offset` if a translator has
	 * ever asked for it. A page nothing stamped holds no translation whose
	 * stamp could be stale, so it is not given a record here. */
	void retire_code_page(std::uint64_t offset) noexcept;

private:
	struct CodePage;
	/* One index leaf covers this many backing pages. A leaf is 4 KiB of
	 * pointers, and the root is one pointer per leaf. */
	static constexpr std::size_t code_index_leaf_pages = 512;
	using CodeIndexLeaf =
		std::array<std::atomic<CodePage *>, code_index_leaf_pages>;

#if DARWIN_UML_X86_64_GUEST
	/* The x86-64 guest keeps the records, their index and the writer index in
	 * the owning address space's heap (frame_heap.hpp). */
	template <typename T>
	using Allocator = FrameAllocator<T>;
	using RecordOwner = HeapOwned<CodePage>;
	using LeafOwner = HeapOwned<CodeIndexLeaf>;
	using RootOwner = HeapArray<std::atomic<CodeIndexLeaf *>>;
	MappedBacking(SharedBackingView mapping, std::size_t size,
		std::shared_ptr<FrameAccountingDomain> domain) noexcept;
#else
	template <typename T>
	using Allocator = std::allocator<T>;
	using RecordOwner = std::unique_ptr<CodePage>;
	using LeafOwner = std::unique_ptr<CodeIndexLeaf>;
	using RootOwner = std::unique_ptr<std::atomic<CodeIndexLeaf *>[]>;
	MappedBacking(SharedBackingView mapping, std::size_t size) noexcept;
#endif
	[[nodiscard]] void *install_code_page(std::uint64_t page) noexcept;

#if DARWIN_UML_X86_64_GUEST
	/* Declared before everything it serves, so that it outlives it. */
	std::shared_ptr<FrameAccountingDomain> domain_;
#endif
	SharedBackingView mapping_;
	std::size_t size_;
	/* Taken only to publish a page seen for the first time. */
	std::mutex code_pages_mutex_;
	std::unordered_map<std::uint64_t, RecordOwner, std::hash<std::uint64_t>,
		std::equal_to<std::uint64_t>,
		Allocator<std::pair<const std::uint64_t, RecordOwner>>>
		code_pages_;
	std::vector<LeafOwner, Allocator<LeafOwner>> code_index_leaves_;
	RootOwner code_index_;
	std::size_t code_index_pages_{};

#if DARWIN_UML_X86_64_GUEST
public:
	/* The writer words of the x86-64 guest's frame accounting
	 * (frame_accounting.hpp): one 64-bit word per 4 KiB frame, read and
	 * written only under that frame's stripe lock. */

	/* Makes the index leaf of the frame at `offset` and, when `record` is
	 * set, the frame's code-page record, whose writer list a frame with
	 * several writers keeps. False when memory ran out; nothing the frame's
	 * words hold changes. Takes no stripe lock. */
	[[nodiscard]] bool prepare_writers(
		std::uint64_t offset, bool record) noexcept;
	/* The frame's word, or null while its leaf does not exist. */
	[[nodiscard]] std::uint64_t *writer_word(std::uint64_t offset) noexcept;

	/* What the frame accounting keeps per frame beside the stamp: the writer
	 * locations of a frame with several. The list lives in the heap of the
	 * address space that made the backing, as the generations' own indexes
	 * do, and gives its room back when the writers are gone. Guarded by the
	 * frame's stripe lock. */
	struct FrameRecord {
		explicit FrameRecord(DomainHeap *heap) noexcept
			: writers{FrameAllocator<WriterLocation>{heap}}
		{
		}
		std::vector<WriterLocation, FrameAllocator<WriterLocation>> writers;
	};
	/* The frame's record, made if it has none; null when memory ran out.
	 * Allocates and takes the records' mutex: never call under a stripe
	 * lock. */
	[[nodiscard]] FrameRecord *frame_record(std::uint64_t offset) noexcept;
	/* The frame's record if it has one: lock-free, allocating nothing and
	 * making no record, so a stripe lock may be held. Only a backing whose
	 * record index exists has frames with writers (prepare_writers), so the
	 * index is all that is looked in. */
	[[nodiscard]] FrameRecord *find_frame_record(std::uint64_t offset) noexcept;
	/* The heap the frames' lists live in, or null for the process's. */
	[[nodiscard]] DomainHeap *heap() const noexcept { return domain_->heap(); }
	/* The domain whose alias table resolves the locations in the frames. */
	[[nodiscard]] FrameAccountingDomain &domain() const noexcept
	{
		return *domain_;
	}
	/* Calls visit(frame_offset, word) for each frame whose word is not 0.
	 * For a caller that excludes every change, a test. */
	template <typename Visit>
	void for_each_writer_word(Visit &&visit) const
	{
		auto *index = writer_index_.load(std::memory_order_acquire);
		if (index == nullptr)
			return;
		for (std::size_t leaf = 0; leaf < writer_index_leaves_; ++leaf) {
			const auto *words = index[leaf].load(std::memory_order_acquire);
			if (words == nullptr)
				continue;
			for (std::size_t entry = 0; entry < writer_leaf_frames; ++entry)
				if ((*words)[entry] != 0)
					visit((leaf * writer_leaf_frames + entry) *
						frame_bytes, (*words)[entry]);
		}
	}

private:
	static constexpr std::size_t writer_leaf_frames = 512;
	static constexpr std::uint64_t frame_bytes = 4096;
	using WriterLeaf = std::array<std::uint64_t, writer_leaf_frames>;

	/* Made under code_pages_mutex_ when the first location is added, and
	 * published through writer_index_ with the leaf count before it. */
	HeapArray<std::atomic<WriterLeaf *>> writer_index_storage_;
	std::atomic<std::atomic<WriterLeaf *> *> writer_index_{nullptr};
	std::size_t writer_index_leaves_{};
	std::vector<HeapOwned<WriterLeaf>, FrameAllocator<HeapOwned<WriterLeaf>>>
		writer_leaves_;
#endif
};

} // namespace uml::memory

// SPDX-License-Identifier: GPL-2.0-or-later
#include "mapped_backing.hpp"

#include "allocation_fault.hpp"
#include "../shared_backing.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <utility>

namespace uml::memory {
namespace {

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
std::atomic_uint64_t next_code_page_identity{1};

} // namespace

struct MappedBacking::CodePage final {
	CodePage() noexcept
		: identity{next_code_page_identity.fetch_add(
			1, std::memory_order_relaxed)}
	{
	}

	const std::uint64_t identity;
	std::atomic_uint64_t version{1};
};

std::shared_ptr<MappedBacking> MappedBacking::create(
	int fd, const struct stat &status)
{
	if (status.st_size <= 0 ||
	    static_cast<std::uintmax_t>(status.st_size) > SIZE_MAX)
		return {};
	const auto size = static_cast<std::size_t>(status.st_size);
	auto mapping = acquire_shared_backing_view(
		fd, 0, size, PROT_READ | PROT_WRITE);
	if (!mapping)
		return {};
	UML_MEMORY_BACKING_MAPPING_CREATED();
	struct CounterGuard final {
		bool owned{true};
		~CounterGuard()
		{
			if (owned)
				UML_MEMORY_BACKING_MAPPING_DESTROYED();
		}
	} guard;
	UML_MEMORY_ALLOCATION_CHECKPOINT(
		testing::AllocationSite::backing);
	auto *backing = new MappedBacking{std::move(mapping), size};
	guard.owned = false;
	return std::shared_ptr<MappedBacking>{backing};
}

MappedBacking::MappedBacking(
	SharedBackingView mapping, std::size_t size) noexcept
	: mapping_(std::move(mapping)), size_(size)
{
	const auto pages = size_ / page_size + 1U;
	const auto roots = (pages + code_index_leaf_pages - 1U) /
		code_index_leaf_pages;
	/* One pointer per leaf, eight kilobytes for a gibibyte of backing. A
	 * backing that cannot afford it keeps the locked lookup. */
	code_index_.reset(new (std::nothrow)
		std::atomic<CodeIndexLeaf *>[roots]{});
	if (code_index_)
		code_index_pages_ = roots * code_index_leaf_pages;
	/* The same shape for the holder words: eight kilobytes of roots for a
	 * gibibyte, and a backing that cannot afford them is unknown to its
	 * translators and publishers alike. */
	const auto holder_roots = (pages + code_holders::leaf_frames - 1U) /
		code_holders::leaf_frames;
	holder_roots_.reset(new (std::nothrow)
		std::atomic<code_holders::Leaf *>[holder_roots]{});
	if (holder_roots_)
		holder_frames_ = holder_roots * code_holders::leaf_frames;
	else
		mark_holders_unknown();
}

MappedBacking::~MappedBacking()
{
	for (std::size_t root = 0; root < holder_frames_ / code_holders::leaf_frames;
	     ++root)
		delete holder_roots_[root].load(std::memory_order_relaxed);
	if (mapping_)
		UML_MEMORY_BACKING_MAPPING_DESTROYED();
}

code_holders::Word *MappedBacking::holder_word(std::uint64_t offset) noexcept
{
	const auto frame = offset / page_size;
	if (frame >= holder_frames_) {
		/* No root, or a frame the backing does not have: nowhere to
		 * register, so nothing can be told which translators hold it. */
		mark_holders_unknown();
		return nullptr;
	}
	auto &root = holder_roots_[frame / code_holders::leaf_frames];
	auto *leaf = root.load(std::memory_order_acquire);
	if (leaf == nullptr)
		leaf = make_holder_leaf(root);
	if (leaf == nullptr) {
		mark_holders_unknown();
		return nullptr;
	}
	return &(*leaf)[frame % code_holders::leaf_frames];
}

code_holders::Word *MappedBacking::find_holder_word(
	std::uint64_t offset) const noexcept
{
	const auto frame = offset / page_size;
	if (frame >= holder_frames_)
		return nullptr;
	auto *leaf = holder_roots_[frame / code_holders::leaf_frames].load(
		std::memory_order_acquire);
	return leaf != nullptr ? &(*leaf)[frame % code_holders::leaf_frames] :
		nullptr;
}

code_holders::Leaf *MappedBacking::make_holder_leaf(
	std::atomic<code_holders::Leaf *> &root) noexcept
{
	try {
		const std::lock_guard lock{holder_mutex_};
		if (auto *made = root.load(std::memory_order_relaxed))
			return made;
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::holder_leaf);
		auto *leaf = new (std::nothrow) code_holders::Leaf{};
		if (leaf == nullptr)
			return nullptr;
		root.store(leaf, std::memory_order_release);
		code_holders::count(&code_holders::detail::CounterStore::leaves);
		return leaf;
	} catch (...) {
		return nullptr;
	}
}

void MappedBacking::mark_holders_unknown() noexcept
{
	if (!holders_unknown_.exchange(true, std::memory_order_seq_cst))
		code_holders::count(
			&code_holders::detail::CounterStore::unknown_backings);
	/* The raised flag is what a publisher reads after it stores the bytes,
	 * so it goes out before this translator reads them. */
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

std::byte *MappedBacking::at(std::uint64_t offset) const noexcept
{
	auto *data = static_cast<std::byte *>(mapping_.data());
	return offset < size_ ? data + static_cast<std::size_t>(offset) : nullptr;
}

void *MappedBacking::code_page_handle(std::uint64_t offset) noexcept
{
	const auto page = offset / page_size;
	/* A record is created once and never destroyed or moved, so a published
	 * index entry stays true for the backing's lifetime and the repeat ask --
	 * every revalidation of every cached translation on the page -- answers
	 * with two acquiring loads instead of a reader-writer lock whose unlock
	 * signals a condition variable. */
	if (code_index_ && page < code_index_pages_) {
		auto *leaf = code_index_[page / code_index_leaf_pages].load(
			std::memory_order_acquire);
		if (leaf != nullptr) {
			auto *found = (*leaf)[page % code_index_leaf_pages].load(
				std::memory_order_acquire);
			if (found != nullptr)
				return found;
		}
	}
	return install_code_page(page);
}

void *MappedBacking::install_code_page(std::uint64_t page) noexcept
{
	try {
		const std::lock_guard lock{code_pages_mutex_};
		auto &slot = code_pages_[page];
		if (!slot)
			slot = std::make_unique<CodePage>();
		auto *handle = slot.get();
		if (!code_index_ || page >= code_index_pages_)
			return handle;
		auto &root = code_index_[page / code_index_leaf_pages];
		auto *leaf = root.load(std::memory_order_relaxed);
		if (leaf == nullptr) {
			auto owned = std::make_unique<CodeIndexLeaf>();
			leaf = owned.get();
			code_index_leaves_.push_back(std::move(owned));
			root.store(leaf, std::memory_order_release);
		}
		(*leaf)[page % code_index_leaf_pages].store(
			handle, std::memory_order_release);
		return handle;
	} catch (...) {
		/* Without the index every ask takes this lock, which is the
		 * behaviour this replaces. */
		return nullptr;
	}
}

bool MappedBacking::load_code_page_stamp(const void *handle,
	std::uint64_t &identity, std::uint64_t &version) noexcept
{
	const auto *page = static_cast<const CodePage *>(handle);
	if (page == nullptr)
		return false;
	identity = page->identity;
	version = page->version.load(std::memory_order_acquire);
	return true;
}

void MappedBacking::invalidate_code_page(void *handle) noexcept
{
	auto *page = static_cast<CodePage *>(handle);
	if (page != nullptr)
		page->version.fetch_add(1, std::memory_order_release);
}

void MappedBacking::retire_code_page(std::uint64_t offset) noexcept
{
	const auto page = offset / page_size;
	if (code_index_ && page < code_index_pages_) {
		/* Every record of an indexed page is in the index. */
		const auto *leaf = code_index_[page / code_index_leaf_pages].load(
			std::memory_order_acquire);
		if (leaf == nullptr)
			return;
		invalidate_code_page((*leaf)[page % code_index_leaf_pages].load(
			std::memory_order_acquire));
		return;
	}
	try {
		const std::lock_guard lock{code_pages_mutex_};
		const auto found = code_pages_.find(page);
		if (found != code_pages_.end())
			invalidate_code_page(found->second.get());
	} catch (...) {
	}
}

} // namespace uml::memory

#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTALLOCATIONS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTALLOCATIONS_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <compare>
#include <functional>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

namespace GuestAllocations {

struct Range {
    std::uint64_t address;
    std::size_t bytes;
    bool readable;
    bool writable;
    std::uint64_t allocationAddress;
    std::size_t allocationBytes;
    bool releasable = true;
    bool gpu = false;
    std::uint64_t backingSerial = 0;
};

class Lease {
public:
    using Ranges = std::vector<std::shared_ptr<const Range>>;
    using value_type = Ranges::value_type;

    struct Chunk {
        Ranges ranges;
        std::uint64_t low = 0;
        std::uint64_t high = 0;
    };

    struct Body {
        std::vector<std::shared_ptr<const Chunk>> chunks;
        std::vector<std::size_t> starts{0};
    };

    class const_iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = Lease::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;

        const_iterator() = default;
        const_iterator(const Body* body, std::size_t index) : body(body) { locate(index); }

        reference operator*() const { return *current; }
        pointer operator->() const { return current; }
        reference operator[](difference_type offset) const { return *(*this + offset); }
        const_iterator& operator++() {
            if (++current == last) enter(chunk + 1);
            return *this;
        }
        const_iterator operator++(int) {
            auto before = *this;
            ++*this;
            return before;
        }
        const_iterator& operator--() {
            if (current != first) --current;
            else locate(index() - 1);
            return *this;
        }
        const_iterator operator--(int) {
            auto before = *this;
            --*this;
            return before;
        }
        const_iterator& operator+=(difference_type offset) {
            if (current != nullptr && offset >= first - current && offset < last - current) current += offset;
            else if (offset != 0) locate(static_cast<std::size_t>(static_cast<difference_type>(index()) + offset));
            return *this;
        }
        const_iterator& operator-=(difference_type offset) { return *this += -offset; }
        friend const_iterator operator+(const_iterator it, difference_type offset) { return it += offset; }
        friend const_iterator operator+(difference_type offset, const_iterator it) { return it += offset; }
        friend const_iterator operator-(const_iterator it, difference_type offset) { return it += -offset; }
        friend difference_type operator-(const const_iterator& left, const const_iterator& right) { return static_cast<difference_type>(left.index()) - static_cast<difference_type>(right.index()); }
        friend bool operator==(const const_iterator& left, const const_iterator& right) { return left.chunk == right.chunk && left.current == right.current; }
        friend auto operator<=>(const const_iterator& left, const const_iterator& right) { return left.index() <=> right.index(); }

    private:
        std::size_t index() const { return body == nullptr ? 0 : body->starts[chunk] + static_cast<std::size_t>(current - first); }
        void enter(std::size_t target) {
            chunk = target;
            if (body == nullptr || chunk >= body->chunks.size()) {
                first = last = current = nullptr;
                return;
            }
            const auto& ranges = body->chunks[chunk]->ranges;
            first = current = ranges.data();
            last = first + ranges.size();
        }
        void locate(std::size_t index) {
            if (body == nullptr || index >= body->starts.back()) {
                enter(body == nullptr ? 0 : body->chunks.size());
                return;
            }
            const auto& starts = body->starts;
            std::size_t low = 0;
            std::size_t high = body->chunks.size();
            while (high - low > 1) {
                const auto middle = low + (high - low) / 2;
                if (starts[middle] <= index) low = middle;
                else high = middle;
            }
            enter(low);
            current += index - starts[low];
        }

        const Body* body = nullptr;
        std::size_t chunk = 0;
        pointer current = nullptr;
        pointer first = nullptr;
        pointer last = nullptr;
    };
    using iterator = const_iterator;

    Lease() = default;
    explicit Lease(std::shared_ptr<const Body> body) : body(std::move(body)) {}
    explicit Lease(Ranges ranges) {
        auto made = std::make_shared<Body>();
        if (!ranges.empty()) {
            auto chunk = std::make_shared<Chunk>();
            chunk->low = ranges.front()->address;
            chunk->high = ranges.back()->address + ranges.back()->bytes;
            chunk->ranges = std::move(ranges);
            made->starts.push_back(chunk->ranges.size());
            made->chunks.push_back(std::move(chunk));
        }
        body = std::move(made);
    }
    explicit Lease(const std::shared_ptr<const Ranges>& ranges) : Lease(ranges != nullptr ? *ranges : Ranges{}) {}

    const_iterator begin() const { return const_iterator(body.get(), 0); }
    const_iterator end() const { return const_iterator(body.get(), size()); }
    std::size_t size() const { return body != nullptr ? body->starts.back() : 0; }
    bool empty() const { return size() == 0; }
    const value_type& operator[](std::size_t index) const { return *const_iterator(body.get(), index); }
    const value_type& front() const { return body->chunks.front()->ranges.front(); }
    const value_type& back() const { return body->chunks.back()->ranges.back(); }
    void clear() { body.reset(); }
    const std::shared_ptr<const Body>& Snapshot() const { return body; }

private:
    std::shared_ptr<const Body> body;
};
using Mapped = std::vector<std::weak_ptr<const Range>>;

struct LeaseCounts {
    std::uint64_t readableBuilds = 0;
    std::uint64_t retiringBuilds = 0;
    std::uint64_t retiringHits = 0;
    std::uint64_t pinWaits = 0;
    std::uint64_t pinWaitNs = 0;
    std::uint64_t pinWaitsUnderMs = 0;
    std::uint64_t pinWaitsUnderTenMs = 0;
    std::uint64_t pinWaitsLonger = 0;
};

extern "C" {
void* GuestAllocationsBegin_nid_postfix();
void GuestAllocationsRegisterMainImage_nid_postfix(void* mutation);
void GuestAllocationsRegisterImage_nid_postfix(void* mutation, const void* image);
void GuestAllocationsUnregisterImage_nid_postfix(void* mutation, const void* image);
void GuestAllocationsEnd_nid_postfix(void* mutation) noexcept;
void GuestAllocationsAdd_nid_postfix(void* mutation, void* pointer, std::size_t bytes, bool readable, bool writable, bool gpu);
void GuestAllocationsRequireUnpinned_nid_postfix(void* mutation, const void* pointer, std::size_t bytes);
void GuestAllocationsRequireAvailable_nid_postfix(void* mutation, const void* pointer, std::size_t bytes);
bool GuestAllocationsCovers_nid_postfix(void* mutation, const void* pointer, std::size_t bytes);
void GuestAllocationsTrimToRegistered_nid_postfix(void* mutation, std::size_t page, const void** pointer, std::size_t* bytes);
void GuestAllocationsGuestPages_nid_postfix(std::uintptr_t address, std::size_t bytes, std::size_t page, std::uintptr_t* first, std::uintptr_t* end);
bool GuestAllocationsOverlaps_nid_postfix(void* mutation, const void* pointer, std::size_t bytes);
bool GuestAllocationsForRegistered_nid_postfix(void* mutation, const void* pointer, std::size_t bytes, const std::function<void(const void*, std::size_t)>& visit);
Range GuestAllocationsFind_nid_postfix(void* mutation, const void* pointer);
void GuestAllocationsRemove_nid_postfix(void* mutation, const void* pointer);
void GuestAllocationsProtect_nid_postfix(void* mutation, const void* pointer, std::size_t bytes, bool readable, bool writable, bool gpu, const std::function<void()>& apply);
void GuestAllocationsUnmap_nid_postfix(void* mutation, const void* pointer, std::size_t bytes, const std::function<void(const void*, std::size_t, const void*, bool)>& apply);
Lease GuestAllocationsAcquire_nid_postfix();
Lease GuestAllocationsAcquireMapped_nid_no_patch(const Mapped& mapped);
Lease GuestAllocationsAcquireAll_nid_postfix();
Lease GuestAllocationsAcquireRegistered_nid_postfix();
bool GuestAllocationsAcquireSettled_nid_no_patch(Lease* lease, std::uint64_t* published);
bool GuestAllocationsAcquireChanged_nid_no_patch(std::uint64_t since, const std::function<void(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& changed, std::vector<std::pair<std::uint64_t, std::uint64_t>>& cover)>& select, std::vector<std::pair<std::uint64_t, std::uint64_t>>* changed, std::uint64_t* through, Lease* lease, Lease* whole, bool* unsettled = nullptr);
bool GuestAllocationsLeaseCurrent_nid_no_patch(const Lease& lease, std::uint64_t* generation);
bool GuestAllocationsLeaseGeneration_nid_no_patch(const Lease& lease, std::uint64_t* generation, bool* unsettled = nullptr);
std::shared_ptr<const Range> GuestAllocationsLookup_nid_postfix(std::uintptr_t address, bool next);
void GuestAllocationsLookupEach_nid_postfix(const std::uintptr_t* addresses, std::size_t count, std::shared_ptr<const Range>* ranges);
// Changes whenever a mutation ends, so callers can cache facts about guest mappings between changes.
std::uint64_t GuestAllocationsGeneration_nid_postfix();
bool GuestAllocationsChangedSince_nid_postfix(std::uint64_t generation, std::uintptr_t address, std::size_t bytes);
bool GuestAllocationsChangesSince_nid_no_patch(std::uint64_t generation, std::vector<std::pair<std::uint64_t, std::uint64_t>>* spans, std::uint64_t* through);
bool GuestAllocationsAnyGpuRange_nid_postfix(std::uintptr_t address, std::size_t bytes);
bool GuestAllocationsMutationWaiting_nid_postfix();
bool GuestAllocationsRetiring_nid_postfix(std::uintptr_t address, std::size_t bytes);
std::shared_ptr<const Range> GuestAllocationsReadableAtOrBelow_nid_postfix(std::uintptr_t address);
std::uint64_t GuestAllocationsLeaseStamp_nid_postfix();
LeaseCounts GuestAllocationsLeaseCounts_nid_postfix();
// Precise invalidation for such caches: the callback runs, after the generation changed, for every
// range whose mapping or protection changed (registry mutations and guest heap decommits).
void GuestAllocationsSetInvalidator_nid_postfix(void (*callback)(std::uintptr_t address, std::size_t bytes));
void GuestAllocationsInvalidate_nid_postfix(std::uintptr_t address, std::size_t bytes);
// A mutation of a range that GPU work still leases waits for the lease (RequireUnpinned). The
// waiter, set by the GPU driver, is called on the mutating thread with the registry lock released
// and finishes the GPU work that holds leases; it returns whether it finished any (a round that found
// nothing to wait for counts against the same bound as a plain spin). Without one the mutation spins
// until the lease is dropped by another thread.
void GuestAllocationsSetPinWaiter_nid_postfix(bool (*callback)(std::uintptr_t address, std::size_t bytes));
void GuestAllocationsSetGpuMapObserver_nid_postfix(void (*callback)(const Mapped& ranges, std::uint64_t generation));
void GuestAllocationsSetUnmapObserver_nid_postfix(void (*callback)(std::uintptr_t address, std::size_t bytes));
void GuestAllocationsNoteGpuMapping_nid_postfix(void* mutation, const void* pointer, std::size_t bytes);
}

class Mutation {
public:
    Mutation() : handle(GuestAllocationsBegin_nid_postfix()) {}
    ~Mutation() { GuestAllocationsEnd_nid_postfix(handle); }
    Mutation(const Mutation&) = delete;
    Mutation& operator=(const Mutation&) = delete;
    void RegisterMainImage() { GuestAllocationsRegisterMainImage_nid_postfix(handle); }
    void RegisterImage(const void* image) { GuestAllocationsRegisterImage_nid_postfix(handle, image); }
    void UnregisterImage(const void* image) { GuestAllocationsUnregisterImage_nid_postfix(handle, image); }
    void Add(void* pointer, std::size_t bytes, bool readable, bool writable, bool gpu) { GuestAllocationsAdd_nid_postfix(handle, pointer, bytes, readable, writable, gpu); }
    void RequireUnpinned(const void* pointer, std::size_t bytes) const { GuestAllocationsRequireUnpinned_nid_postfix(handle, pointer, bytes); }
    void RequireAvailable(const void* pointer, std::size_t bytes) const { GuestAllocationsRequireAvailable_nid_postfix(handle, pointer, bytes); }
    bool Covers(const void* pointer, std::size_t bytes) const { return GuestAllocationsCovers_nid_postfix(handle, pointer, bytes); }
    void TrimToRegistered(std::size_t page, const void*& pointer, std::size_t& bytes) const { GuestAllocationsTrimToRegistered_nid_postfix(handle, page, &pointer, &bytes); }
    bool Overlaps(const void* pointer, std::size_t bytes) const { return GuestAllocationsOverlaps_nid_postfix(handle, pointer, bytes); }
    bool ForRegistered(const void* pointer, std::size_t bytes, const std::function<void(const void*, std::size_t)>& visit) const { return GuestAllocationsForRegistered_nid_postfix(handle, pointer, bytes, visit); }
    Range Find(const void* pointer) const { return GuestAllocationsFind_nid_postfix(handle, pointer); }
    void Remove(const void* pointer) { GuestAllocationsRemove_nid_postfix(handle, pointer); }
    void Unmap(const void* pointer, std::size_t bytes, const std::function<void(const void*, std::size_t, const void*, bool)>& apply) { GuestAllocationsUnmap_nid_postfix(handle, pointer, bytes, apply); }
    void Protect(const void* pointer, std::size_t bytes, bool readable, bool writable, bool gpu, const std::function<void()>& apply) { GuestAllocationsProtect_nid_postfix(handle, pointer, bytes, readable, writable, gpu, apply); }
    void NoteGpuMapping(const void* pointer, std::size_t bytes) { GuestAllocationsNoteGpuMapping_nid_postfix(handle, pointer, bytes); }

private:
    void* handle;
};

}

#endif

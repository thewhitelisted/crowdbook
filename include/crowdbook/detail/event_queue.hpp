#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "crowdbook/types.hpp"

namespace crowdbook::detail {

// Not part of the library's interface: public classes hold it as a private member, so it is
// installed with them.
//
// The simulation's queue of scheduled items: taken out earliest first and, among items due at the
// same time, in the order they were scheduled. Each item carries a payload, built in place where it
// will stay: a payload neither moves nor is reused from when its item is scheduled until it is
// released, after its item has been taken out, so whoever processes it may schedule more meanwhile.
//
// Nothing is ever scheduled before the item last taken out, as a simulation schedules nothing in
// its past, so the queue is a radix heap (Ahuja, Mehlhorn, Orlin and Tarjan, 1990) rather than a
// binary heap. An item's key is its time and then its sequence number, which is unique; items sit
// in buckets by the highest bit in which their key differs from the key last taken out. Scheduling
// one is a few instructions, and taking one out only looks through the lowest bucket that is not
// empty, sending its other items to lower buckets, so an item moves down a few buckets in its
// life rather than through the whole depth of a heap each time. Since keys are unique, items come
// out in the same order as from any other priority queue.
//
// Keys, and the lists each bucket threads through them, are in one array; payloads are in fixed
// blocks, so that they never move. Once the queue has grown to the most it ever holds, nothing
// allocates. If growing fails to allocate, the queue is left as it was; looking, taking items out
// and releasing them never allocate.
template <typename Payload>
class EventQueue {
public:
    // An item taken out: when it was due, and where its payload is.
    struct Taken {
        Timestamp time = 0;
        std::uint32_t slot = 0;
    };

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // Schedules an item for `time` and returns its payload, for the caller to build. Throws
    // std::logic_error for a time before the item last taken out, and std::bad_alloc or
    // std::length_error if the queue cannot grow, leaving it as it was in each case.
    Payload& push(Timestamp time) {
        if (time < 0 || static_cast<std::uint64_t>(time) < lastTime_) [[unlikely]] {
            throw std::logic_error("an item cannot be scheduled before the one last taken out");
        }
        const std::uint32_t slot = freeSlot();
        Key& key = keys_[slot];
        key.time = static_cast<std::uint64_t>(time);
        key.sequence = nextSequence_++;
        const std::size_t bucket = bucketOf(key);
        link(slot, bucket);
        ++size_;
        // An item in a lower bucket than the earliest found is earlier still, and alone there.
        if (earliest_ != kNone && (bucket < earliestBucket_ ||
                                   (bucket == earliestBucket_ && earlier(key, keys_[earliest_])))) {
            earliest_ = slot;
            earliestBucket_ = bucket;
        }
        return payload(slot);
    }

    // When the earliest item is due; the queue must not be empty.
    [[nodiscard]] Timestamp nextTime() noexcept {
        findEarliest();
        return static_cast<Timestamp>(keys_[earliest_].time);
    }

    // When the earliest item is due, or nullopt if none is.
    [[nodiscard]] std::optional<Timestamp> peekTime() const noexcept {
        if (size_ == 0) {
            return std::nullopt;
        }
        if (earliest_ != kNone) {
            return static_cast<Timestamp>(keys_[earliest_].time);
        }
        return static_cast<Timestamp>(keys_[earliestOf(lowestBucket())].time);
    }

    // Takes out the earliest item; the queue must not be empty. Its payload stays where it is,
    // and its slot is not reused, until it is released.
    Taken pop() noexcept {
        findEarliest();
        const std::uint32_t taken = earliest_;
        const std::size_t bucket = earliestBucket_;
        earliest_ = kNone;
        // It becomes the key last taken out, which sends every other item of its bucket lower.
        lastTime_ = keys_[taken].time;
        lastSequence_ = keys_[taken].sequence;
        std::uint32_t i = heads_[bucket];
        heads_[bucket] = kNone;
        nonEmpty_[bucket / 64] &= ~(std::uint64_t{1} << (bucket % 64));
        while (i != kNone) {
            const std::uint32_t next = keys_[i].next;
            if (i != taken) {
                link(i, bucketOf(keys_[i]));
            }
            i = next;
        }
        --size_;
        return {.time = static_cast<Timestamp>(lastTime_), .slot = taken};
    }

    [[nodiscard]] Payload& payload(std::uint32_t slot) noexcept {
        return blocks_[slot / kBlock][slot % kBlock];
    }

    // Makes a taken item's slot free for another.
    void release(std::uint32_t slot) noexcept {
        keys_[slot].next = free_;
        free_ = slot;
    }

private:
    static constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::size_t kBlock = 64; // payloads to a block
    // Bucket b, from 1 to 64, holds keys of the same time as the key last taken out whose sequence
    // first differs from it in bit b - 1, and bucket 64 + b keys whose time first differs in bit
    // b - 1. Bucket 0 holds a key equal to it: only the first item, if it is due at time 0.
    static constexpr std::size_t kBuckets = 129;

    struct Key {
        std::uint64_t time = 0;
        std::uint64_t sequence = 0;
        std::uint32_t next = kNone; // the next key in its bucket, or among the free slots
    };

    [[nodiscard]] static bool earlier(const Key& a, const Key& b) noexcept {
        return a.time != b.time ? a.time < b.time : a.sequence < b.sequence;
    }

    [[nodiscard]] std::size_t bucketOf(const Key& key) const noexcept {
        if (const std::uint64_t time = key.time ^ lastTime_; time != 0) {
            return 128 - static_cast<std::size_t>(std::countl_zero(time));
        }
        const std::uint64_t sequence = key.sequence ^ lastSequence_;
        return 64 - static_cast<std::size_t>(std::countl_zero(sequence));
    }

    void link(std::uint32_t slot, std::size_t bucket) noexcept {
        keys_[slot].next = heads_[bucket];
        heads_[bucket] = slot;
        nonEmpty_[bucket / 64] |= std::uint64_t{1} << (bucket % 64);
    }

    // The lowest bucket that holds a key; the queue must not be empty. Every key there is earlier
    // than every key in a higher bucket.
    [[nodiscard]] std::size_t lowestBucket() const noexcept {
        for (std::size_t word = 0;; ++word) {
            if (nonEmpty_[word] != 0) {
                return 64 * word + static_cast<std::size_t>(std::countr_zero(nonEmpty_[word]));
            }
        }
    }

    [[nodiscard]] std::uint32_t earliestOf(std::size_t bucket) const noexcept {
        std::uint32_t best = heads_[bucket];
        for (std::uint32_t i = keys_[best].next; i != kNone; i = keys_[i].next) {
            if (earlier(keys_[i], keys_[best])) {
                best = i;
            }
        }
        return best;
    }

    // Finds the earliest item, unless it is known already: looking changes nothing else, so a
    // simulation that stops before an item may still schedule one before it.
    void findEarliest() noexcept {
        if (earliest_ == kNone) {
            earliestBucket_ = lowestBucket();
            earliest_ = earliestOf(earliestBucket_);
        }
    }

    // A slot for a new item, growing the queue if every slot is taken.
    std::uint32_t freeSlot() {
        if (free_ != kNone) {
            const std::uint32_t slot = free_;
            free_ = keys_[slot].next;
            return slot;
        }
        const std::size_t slot = keys_.size();
        if (slot >= kNone) {
            throw std::length_error("the simulation has too many items scheduled at once");
        }
        if (slot / kBlock == blocks_.size()) {
            blocks_.reserve(blocks_.size() + 1);
            blocks_.push_back(std::make_unique<Payload[]>(kBlock));
        }
        keys_.emplace_back();
        return static_cast<std::uint32_t>(slot);
    }

    std::vector<Key> keys_;
    std::vector<std::unique_ptr<Payload[]>> blocks_;
    std::array<std::uint32_t, kBuckets> heads_ = [] {
        std::array<std::uint32_t, kBuckets> heads{};
        heads.fill(kNone);
        return heads;
    }();
    std::array<std::uint64_t, 3> nonEmpty_{}; // a bit for each bucket that holds a key
    std::uint32_t free_ = kNone;              // the first free slot
    std::uint32_t earliest_ = kNone;          // the earliest item, if it has been found
    std::size_t earliestBucket_ = 0;
    std::size_t size_ = 0;
    std::uint64_t nextSequence_ = 0;
    std::uint64_t lastTime_ = 0; // the key last taken out
    std::uint64_t lastSequence_ = 0;
};

} // namespace crowdbook::detail

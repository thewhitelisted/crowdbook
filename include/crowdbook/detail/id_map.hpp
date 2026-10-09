#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

namespace crowdbook::detail {

// Not part of the library's interface: public classes hold it as a private member, so it is
// installed with them.
//
// A hash map from integer ids to small values, for the per-order lookups of the exchange and the
// book: open addressing with linear probing in one array, so that adding and removing an order
// allocate nothing once the table has grown. Values move when the table grows or an entry is
// erased, so a pointer to one lasts only until the next insert or erase. Only audits walk it,
// through forEach, and nothing that changes a run may depend on its order. If growing the table
// fails to allocate, the map is left as it was.
template <typename Key, typename Value>
class IdMap {
public:
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] Value* find(Key key) noexcept {
        if (slots_.empty()) {
            return nullptr;
        }
        for (std::size_t i = home(key);; i = next(i)) {
            Slot& slot = slots_[i];
            if (!slot.used) {
                return nullptr;
            }
            if (slot.key == key) {
                return &slot.value;
            }
        }
    }
    [[nodiscard]] const Value* find(Key key) const noexcept {
        return const_cast<IdMap*>(this)->find(key);
    }
    [[nodiscard]] bool contains(Key key) const noexcept { return find(key) != nullptr; }
    // Throws std::out_of_range for a key that is not there.
    [[nodiscard]] Value& at(Key key) {
        if (Value* value = find(key)) {
            return *value;
        }
        throw std::out_of_range(std::format("no entry for id {}", key));
    }

    // Adds the entry unless the key is there already. Returns the stored value and whether it
    // was added.
    std::pair<Value*, bool> tryEmplace(Key key, const Value& value) {
        if ((size_ + 1) * 2 > slots_.size()) {
            rehash(slots_.empty() ? kFirstCapacity : slots_.size() * 2);
        }
        std::size_t i = home(key);
        for (; slots_[i].used; i = next(i)) {
            if (slots_[i].key == key) {
                return {&slots_[i].value, false};
            }
        }
        slots_[i] = Slot{.key = key, .value = value, .used = true};
        ++size_;
        return {&slots_[i].value, true};
    }

    // Whether `count` more entries fit without the table growing.
    [[nodiscard]] bool hasRoomFor(std::size_t count) const noexcept {
        return (size_ + count) * 2 <= slots_.size();
    }

    // Makes room for `count` entries, so that adding entries up to that many allocates nothing
    // and cannot throw.
    void reserve(std::size_t count) {
        if (count * 2 > slots_.size()) {
            rehash(std::bit_ceil(std::max(count * 2, kFirstCapacity)));
        }
    }

    void insertOrAssign(Key key, const Value& value) {
        if (auto [stored, added] = tryEmplace(key, value); !added) {
            *stored = value;
        }
    }

    // Returns whether the key was there.
    bool erase(Key key) noexcept {
        if (slots_.empty()) {
            return false;
        }
        std::size_t gap = home(key);
        for (; slots_[gap].key != key || !slots_[gap].used; gap = next(gap)) {
            if (!slots_[gap].used) {
                return false;
            }
        }
        // Later entries of the same run move back into the gap, wherever that keeps them
        // reachable from their home slot, so lookups never meet a hole they should pass.
        for (std::size_t i = next(gap); slots_[i].used; i = next(i)) {
            const std::size_t fromHome = (i - home(slots_[i].key)) & mask_;
            if (fromHome >= ((i - gap) & mask_)) {
                slots_[gap] = std::move(slots_[i]);
                gap = i;
            }
        }
        slots_[gap].used = false;
        --size_;
        return true;
    }

    // Calls fn(key, value) for every entry, in no meaningful order.
    template <typename Function>
    void forEach(Function&& function) const {
        for (const Slot& slot : slots_) {
            if (slot.used) {
                function(slot.key, slot.value);
            }
        }
    }

private:
    struct Slot {
        Key key{};
        Value value{};
        bool used = false;
    };

    static constexpr std::size_t kFirstCapacity = 16;

    // Fibonacci hashing: the top bits of the key times 2^64 / golden ratio.
    [[nodiscard]] static std::size_t home(Key key, int shift) noexcept {
        constexpr std::uint64_t kGolden = 0x9E37'79B9'7F4A'7C15U;
        return static_cast<std::size_t>((static_cast<std::uint64_t>(key) * kGolden) >> shift);
    }
    [[nodiscard]] std::size_t home(Key key) const noexcept { return home(key, shift_); }
    [[nodiscard]] std::size_t next(std::size_t i) const noexcept { return (i + 1) & mask_; }

    // Moves every entry into a table of `capacity` slots, a power of two. The new table is built
    // in full before it replaces the old one, so a failure to allocate it changes nothing.
    void rehash(std::size_t capacity) {
        std::vector<Slot> fresh(capacity);
        const std::size_t mask = capacity - 1;
        const int shift = 64 - std::countr_zero(capacity);
        for (const Slot& slot : slots_) {
            if (slot.used) {
                std::size_t i = home(slot.key, shift);
                while (fresh[i].used) {
                    i = (i + 1) & mask;
                }
                fresh[i] = slot;
            }
        }
        slots_ = std::move(fresh);
        mask_ = mask;
        shift_ = shift;
    }

    std::vector<Slot> slots_;
    std::size_t size_ = 0;
    std::size_t mask_ = 0;
    int shift_ = 64;
};

} // namespace crowdbook::detail

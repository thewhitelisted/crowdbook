#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace crowdbook::detail {

// Not part of the library's interface: public classes hold it as a private member, so it is
// installed with them.
//
// A first-in, first-out queue in one array that reuses its slots: once it has grown to the most
// it ever holds, adding and removing allocate nothing, whatever the standard library's deque
// would do. Index 0 is the oldest entry.
template <typename T>
class Ring {
public:
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] T& operator[](std::size_t i) noexcept { return slots_[slot(i)]; }
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept { return slots_[slot(i)]; }
    [[nodiscard]] T& front() noexcept { return (*this)[0]; }
    [[nodiscard]] T& back() noexcept { return (*this)[size_ - 1]; }
    [[nodiscard]] const T& back() const noexcept { return (*this)[size_ - 1]; }

    void pushBack(T value) {
        if (size_ == slots_.size()) {
            grow();
        }
        slots_[slot(size_)] = std::move(value);
        ++size_;
    }

    // Removes the oldest entry, releasing what it held.
    void popFront() noexcept {
        slots_[first_] = T{};
        first_ = first_ + 1 == slots_.size() ? 0 : first_ + 1;
        --size_;
    }

private:
    [[nodiscard]] std::size_t slot(std::size_t i) const noexcept {
        const std::size_t at = first_ + i;
        return at < slots_.size() ? at : at - slots_.size();
    }

    // Doubles the array, with the entries moved to its start in order.
    void grow() {
        std::vector<T> larger(slots_.empty() ? 8 : slots_.size() * 2);
        for (std::size_t i = 0; i < size_; ++i) {
            larger[i] = std::move((*this)[i]);
        }
        slots_ = std::move(larger);
        first_ = 0;
    }

    std::vector<T> slots_;
    std::size_t first_ = 0;
    std::size_t size_ = 0;
};

} // namespace crowdbook::detail

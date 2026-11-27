#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace defb::ai
{

// Minimal lock-free single-producer/single-consumer ring buffer.
// The audio thread only pops, the AI worker only pushes, neither blocks.
template <typename T, int Capacity>
class SpscQueue
{
    static_assert (Capacity >= 2, "capacity must be at least two");
    static_assert ((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    bool push (const T& value) noexcept
    {
        const int write = write_.load (std::memory_order_relaxed);
        const int next  = (write + 1) & (Capacity - 1);

        if (next == read_.load (std::memory_order_acquire))
            return false; // full: drop the newest message instead of waiting

        buffer_[(size_t) write] = value;
        write_.store (next, std::memory_order_release);
        return true;
    }

    bool pop (T& out) noexcept
    {
        const int read = read_.load (std::memory_order_relaxed);

        if (read == write_.load (std::memory_order_acquire))
            return false; // empty

        out = buffer_[(size_t) read];
        read_.store ((read + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept
    {
        return read_.load (std::memory_order_acquire)
            == write_.load (std::memory_order_acquire);
    }

    // Only safe while the producer is stopped (used from reset()).
    void clear() noexcept
    {
        read_.store (write_.load (std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::array<T, Capacity> buffer_ {};
    std::atomic<int> write_ { 0 };
    std::atomic<int> read_  { 0 };
};

} // namespace defb::ai

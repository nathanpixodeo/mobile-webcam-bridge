// Lock-free single-producer / single-consumer triple buffer ("latest value wins").
//
// The producer always has a private slot to fill and never waits for the consumer; the consumer
// always reads a complete, stable slot and never waits for the producer. Publishing a new value
// discards any value the consumer has not picked up yet, which is exactly the policy a live video
// pipe wants: an old frame is worthless once a newer one exists.
//
// Thread-safety contract: exactly one producer thread calls WriteSlot()/Publish(), exactly one
// consumer thread calls HasFresh()/Acquire()/ReadSlot(). The two may run concurrently.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace mwb {

template <typename T>
class TripleBuffer final {
public:
    TripleBuffer() = default;
    TripleBuffer(const TripleBuffer&) = delete;
    TripleBuffer& operator=(const TripleBuffer&) = delete;

    // Producer: the slot to fill next. Stable until the next Publish().
    [[nodiscard]] T& WriteSlot() noexcept { return slots_[back_]; }

    // Producer: makes the slot returned by WriteSlot() the newest value and hands the producer a
    // fresh private slot (the previous "middle" one, possibly never consumed).
    void Publish() noexcept {
        const std::uint32_t previous = middle_.exchange(back_ | kFreshBit, std::memory_order_acq_rel);
        back_ = previous & kIndexMask;
    }

    // Consumer: true when a value newer than ReadSlot() has been published.
    [[nodiscard]] bool HasFresh() const noexcept {
        return (middle_.load(std::memory_order_acquire) & kFreshBit) != 0;
    }

    // Consumer: if a newer value exists, makes it the read slot and returns true.
    bool Acquire() noexcept {
        if (!HasFresh()) return false;
        const std::uint32_t previous = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = previous & kIndexMask;
        return true;
    }

    // Consumer: the value acquired last (default-constructed T before the first Acquire()).
    [[nodiscard]] T& ReadSlot() noexcept { return slots_[front_]; }
    [[nodiscard]] const T& ReadSlot() const noexcept { return slots_[front_]; }

    // Not thread-safe: for one-time setup (e.g. pre-sizing every slot) before the threads start.
    template <typename Fn>
    void ForEachSlotUnsynchronized(Fn&& fn) {
        for (T& slot : slots_) fn(slot);
    }

private:
    static constexpr std::uint32_t kIndexMask = 0x3u;
    static constexpr std::uint32_t kFreshBit = 0x4u;

    std::array<T, 3> slots_{};
    std::uint32_t back_ = 0;                 // producer-owned
    std::atomic<std::uint32_t> middle_{1};   // shared: index of the exchange slot + fresh bit
    std::uint32_t front_ = 2;                // consumer-owned
};

}  // namespace mwb

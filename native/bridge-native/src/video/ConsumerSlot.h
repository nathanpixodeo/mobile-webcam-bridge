// One-item mailbox between the hub and a slow consumer: a newer item replaces the unsent one
// (latest frame wins), so a stalled consumer never makes the hub buffer frames. Not thread-safe;
// the owner serialises access.
#pragma once

#include <optional>
#include <utility>

namespace mwb::native {

template <typename T>
class ConsumerSlot {
public:
    // Stores `item` as the next one to send. Returns true when it replaced an unsent item.
    bool Offer(T item) {
        const bool replaced = pending_.has_value();
        pending_ = std::move(item);
        return replaced;
    }

    // Removes and returns the pending item, if any.
    [[nodiscard]] std::optional<T> Take() {
        std::optional<T> item = std::move(pending_);
        pending_.reset();
        return item;
    }

    [[nodiscard]] bool HasPending() const noexcept { return pending_.has_value(); }

private:
    std::optional<T> pending_;
};

}  // namespace mwb::native

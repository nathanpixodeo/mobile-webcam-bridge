#include "video/FramePool.h"

namespace mwb::native {

namespace {

// Owns a shared frame; hands the buffer back to the pool when the last reference goes away.
struct Lease {
    FrameBytes bytes;
    std::weak_ptr<FramePool> pool;

    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease() {
        if (const std::shared_ptr<FramePool> owner = pool.lock()) owner->Release(std::move(bytes));
    }
};

}  // namespace

std::shared_ptr<FramePool> FramePool::Create(std::size_t frameBytes, std::size_t maxCached) {
    return std::make_shared<FramePool>(PrivateTag{}, frameBytes, maxCached);
}

FrameBytes FramePool::Acquire() {
    std::size_t frameBytes = 0;
    {
        const std::lock_guard lock(mutex_);
        if (!cache_.empty()) {
            FrameBytes buffer = std::move(cache_.back());
            cache_.pop_back();
            buffer.clear();
            return buffer;
        }
        frameBytes = frameBytes_;
    }
    FrameBytes buffer;
    buffer.reserve(frameBytes);
    return buffer;
}

void FramePool::Release(FrameBytes buffer) {
    const std::lock_guard lock(mutex_);
    // Buffers of a previous, larger size would pin memory for nothing (4K frames are 12 MB).
    if (buffer.capacity() < frameBytes_ || buffer.capacity() > 2 * frameBytes_) return;
    if (cache_.size() < maxCached_) cache_.push_back(std::move(buffer));
}

void FramePool::SetFrameBytes(std::size_t frameBytes) {
    const std::lock_guard lock(mutex_);
    if (frameBytes == frameBytes_) return;
    frameBytes_ = frameBytes;
    cache_.clear();
}

SharedFrameBytes FramePool::Share(FrameBytes buffer) {
    auto lease = std::make_shared<Lease>();
    lease->bytes = std::move(buffer);
    lease->pool = weak_from_this();
    const FrameBytes* bytes = &lease->bytes;
    return SharedFrameBytes(std::move(lease), bytes);  // aliasing: owns the lease, points at the bytes
}

}  // namespace mwb::native

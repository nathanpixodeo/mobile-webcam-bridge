// Cuts an arbitrary byte stream into fixed-size PCM chunks (10 ms = 960 bytes at 48 kHz mono
// s16le). Pure logic.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace mwb::native {

class PcmChunker {
public:
    PcmChunker(std::size_t chunkBytes, std::size_t blockAlign) : chunkBytes_(chunkBytes), blockAlign_(blockAlign) {
        if (chunkBytes_ == 0 || blockAlign_ == 0 || chunkBytes_ % blockAlign_ != 0) {
            throw std::invalid_argument("PcmChunker: chunk size must be a positive multiple of the block alignment");
        }
        pending_.reserve(chunkBytes_);
    }

    // Appends `data`; calls `onChunk(std::span<const std::uint8_t>)` for every full chunk.
    template <typename OnChunk>
    void Push(std::span<const std::uint8_t> data, OnChunk&& onChunk) {
        while (!data.empty()) {
            const std::size_t take = std::min(chunkBytes_ - pending_.size(), data.size());
            pending_.insert(pending_.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(take));
            data = data.subspan(take);
            if (pending_.size() == chunkBytes_) {
                onChunk(std::span<const std::uint8_t>(pending_));
                pending_.clear();
            }
        }
    }

    // Emits the remaining whole audio frames (if any) as a final, shorter chunk.
    template <typename OnChunk>
    void Flush(OnChunk&& onChunk) {
        const std::size_t whole = pending_.size() - pending_.size() % blockAlign_;
        if (whole > 0) onChunk(std::span<const std::uint8_t>(pending_.data(), whole));
        pending_.clear();
    }

    [[nodiscard]] std::size_t PendingBytes() const noexcept { return pending_.size(); }

private:
    std::size_t chunkBytes_;
    std::size_t blockAlign_;
    std::vector<std::uint8_t> pending_;
};

}  // namespace mwb::native

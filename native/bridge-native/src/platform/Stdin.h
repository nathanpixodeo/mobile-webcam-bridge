// Blocking reads from stdin, used by the long-running commands. EOF on stdin is the contract's
// shutdown signal: Node closes the pipe (or dies), the helper exits.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mwb::native {

class StdinReader {
public:
    StdinReader();

    // Blocks until data arrives; returns 0 at end of input (pipe closed or broken).
    [[nodiscard]] std::size_t Read(void* buffer, std::size_t size);

private:
    void* handle_;
};

// Splits a byte stream into lines ('\n', optional preceding '\r'). Lines longer than
// `maxLineBytes` are discarded and reported through the callback with `overflow = true`.
class LineSplitter {
public:
    explicit LineSplitter(std::size_t maxLineBytes) : maxLineBytes_(maxLineBytes) {}

    template <typename OnLine>  // void(std::string_view line, bool overflow)
    void Push(std::string_view data, OnLine&& onLine) {
        for (const char c : data) {
            if (c == '\n') {
                if (!pending_.empty() && pending_.back() == '\r') pending_.pop_back();
                onLine(std::string_view(pending_), overflow_);
                pending_.clear();
                overflow_ = false;
                continue;
            }
            if (overflow_) continue;
            if (pending_.size() >= maxLineBytes_) {
                pending_.clear();
                overflow_ = true;
                continue;
            }
            pending_.push_back(c);
        }
    }

    [[nodiscard]] bool HasPartialLine() const noexcept { return !pending_.empty() || overflow_; }

private:
    std::size_t maxLineBytes_;
    std::string pending_;
    bool overflow_ = false;
};

}  // namespace mwb::native

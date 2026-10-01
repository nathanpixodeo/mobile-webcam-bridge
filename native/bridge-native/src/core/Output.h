// Process output channels. stdout carries the JSON contract (one object, or JSON lines);
// stderr carries human-readable diagnostics that Node forwards to its log.
#pragma once

#include <mutex>
#include <string_view>

namespace mwb::native {

class JsonWriter;

// Thread-safe line writer over a raw Win32 handle (no CRT buffering or text translation).
class OutputChannel {
public:
    explicit OutputChannel(void* handle) noexcept : handle_(handle) {}
    OutputChannel(const OutputChannel&) = delete;
    OutputChannel& operator=(const OutputChannel&) = delete;

    // Writes `line` followed by '\n' atomically with respect to other WriteLine calls.
    // Returns false when the reader is gone (broken pipe); callers may then shut down.
    bool WriteLine(std::string_view line);

private:
    void* handle_;
    std::mutex mutex_;
};

class Console {
public:
    Console();

    [[nodiscard]] OutputChannel& Out() noexcept { return out_; }

    // Writes a finished JSON document as one stdout line.
    bool Emit(JsonWriter& document);

    void Info(std::string_view message);
    void Warn(std::string_view message);
    void Error(std::string_view message);

private:
    void Diagnostic(std::string_view level, std::string_view message);

    OutputChannel out_;
    OutputChannel err_;
};

}  // namespace mwb::native

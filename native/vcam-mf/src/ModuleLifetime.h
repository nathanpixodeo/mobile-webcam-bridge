// Live-object and server-lock accounting for DllCanUnloadNow.
#pragma once

#include <atomic>

namespace mwb::vcam {

class ModuleLifetime final {
public:
    static void AddObject() noexcept { objects_.fetch_add(1, std::memory_order_relaxed); }
    static void ReleaseObject() noexcept { objects_.fetch_sub(1, std::memory_order_release); }
    static void Lock() noexcept { locks_.fetch_add(1, std::memory_order_relaxed); }
    static void Unlock() noexcept { locks_.fetch_sub(1, std::memory_order_release); }

    [[nodiscard]] static bool CanUnload() noexcept {
        return objects_.load(std::memory_order_acquire) == 0 && locks_.load(std::memory_order_acquire) == 0;
    }

private:
    static inline std::atomic<long> objects_{0};
    static inline std::atomic<long> locks_{0};
};

// Embed one in every COM object of the DLL: keeps the module loaded while the object lives.
class ModuleObjectToken final {
public:
    ModuleObjectToken() noexcept { ModuleLifetime::AddObject(); }
    ~ModuleObjectToken() { ModuleLifetime::ReleaseObject(); }
    ModuleObjectToken(const ModuleObjectToken&) = delete;
    ModuleObjectToken& operator=(const ModuleObjectToken&) = delete;
};

}  // namespace mwb::vcam

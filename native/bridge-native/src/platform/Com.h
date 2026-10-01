// COM apartment scope that tolerates an apartment already initialised by someone else.
#pragma once

#include <windows.h>
#include <objbase.h>

namespace mwb::native {

class ComApartment {
public:
    ComApartment() noexcept : hr_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(hr_)) ::CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    // S_OK / S_FALSE: this scope owns an MTA reference. RPC_E_CHANGED_MODE: the thread is
    // already in an STA, which COM calls still work in.
    [[nodiscard]] bool Usable() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

}  // namespace mwb::native

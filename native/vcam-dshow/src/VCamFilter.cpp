#include "Framework.h"

#include "VCamFilter.h"

#include "VCamPin.h"

#include <mwb/Identifiers.h>
#include <mwb/um/Guid.h>

#include <new>

namespace mwb::dshow {

namespace {
constexpr GUID kFilterClsid = um::GuidFromString(ids::kDShowFilterClsid);
}

CUnknown* WINAPI VCamFilter::CreateInstance(LPUNKNOWN outer, HRESULT* result) {
    if (result == nullptr) return nullptr;
    try {
        um::CameraSettings settings;
        if (FAILED(um::LoadCameraSettings(settings))) settings = um::CameraSettings{};

        auto* filter = new (std::nothrow) VCamFilter(outer, result, settings);
        if (filter == nullptr) {
            *result = E_OUTOFMEMORY;
            return nullptr;
        }
        if (FAILED(*result)) {
            delete filter;
            return nullptr;
        }
        return filter;
    } catch (...) {
        *result = E_OUTOFMEMORY;
        return nullptr;
    }
}

VCamFilter::VCamFilter(LPUNKNOWN outer, HRESULT* result, const um::CameraSettings& settings)
    : CSource(NAME("MobileWebcamBridge Virtual Camera"), outer, kFilterClsid, result) {
    if (FAILED(*result)) return;
    // The pin registers itself with this filter (CSource::AddPin) and is owned by it.
    auto* pin = new (std::nothrow) VCamPin(result, this, settings);
    if (pin == nullptr) *result = E_OUTOFMEMORY;
}

STDMETHODIMP VCamFilter::NonDelegatingQueryInterface(REFIID riid, void** object) {
    CheckPointer(object, E_POINTER);
    if (riid == IID_IAMFilterMiscFlags) {
        return GetInterface(static_cast<IAMFilterMiscFlags*>(this), object);
    }
    return CSource::NonDelegatingQueryInterface(riid, object);
}

STDMETHODIMP VCamFilter::GetState(DWORD milliseconds, FILTER_STATE* state) {
    const HRESULT hr = CSource::GetState(milliseconds, state);
    if (SUCCEEDED(hr) && *state == State_Paused) return VFW_S_CANT_CUE;
    return hr;
}

STDMETHODIMP_(ULONG) VCamFilter::GetMiscFlags() {
    return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

}  // namespace mwb::dshow

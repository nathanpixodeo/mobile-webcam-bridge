// DirectShow source filter registered in CLSID_VideoInputDeviceCategory: what DirectShow
// applications enumerate as the "Mobile Webcam" device on Windows 10. One capture pin (VCamPin).
#pragma once

#include "Framework.h"

#include <mwb/um/CameraConfig.h>

namespace mwb::dshow {

class VCamFilter final : public CSource, public IAMFilterMiscFlags {
public:
    // CFactoryTemplate entry point.
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN outer, HRESULT* result);

    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** object) override;

    // A live source cannot cue data while paused; VFW_S_CANT_CUE stops the graph from waiting.
    STDMETHODIMP GetState(DWORD milliseconds, FILTER_STATE* state) override;

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override;

    [[nodiscard]] bool IsRunning() const noexcept { return m_State == State_Running; }

private:
    VCamFilter(LPUNKNOWN outer, HRESULT* result, const um::CameraSettings& settings);
};

}  // namespace mwb::dshow

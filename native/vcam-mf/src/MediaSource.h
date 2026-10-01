// Media Foundation virtual camera source (one video stream), activated by Frame Server through
// MFCreateVirtualCamera. Source-level semantics follow Microsoft's Windows-Camera VirtualCamera
// sample (SimpleMediaSource); see NOTICE.md.
#pragma once

#include "Framework.h"
#include "MediaStream.h"
#include "ModuleLifetime.h"

#include <mwb/um/CameraConfig.h>

namespace mwb::vcam {

class MediaSource final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>, IMFGetService,
          IKsControl, IMFSampleAllocatorControl> {
public:
    MediaSource() = default;
    ~MediaSource();

    // `activateAttributes`: the attributes Frame Server set on our IMFActivate (copied).
    HRESULT RuntimeClassInitialize(IMFAttributes* activateAttributes) noexcept;

    [[nodiscard]] bool IsShutdown() const noexcept;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* eventValue) override;

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) override;
    STDMETHODIMP Stop() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

    // IKsControl — no camera controls are exposed; mimic a driver without handlers.
    STDMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID propertyData, ULONG dataLength,
                            ULONG* bytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, LPVOID methodData, ULONG dataLength,
                          ULONG* bytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, LPVOID eventData, ULONG dataLength,
                         ULONG* bytesReturned) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

private:
    enum class State { Stopped, Started, Shutdown };

    static constexpr DWORD kStreamId = 0;

    [[nodiscard]] HRESULT CheckShutdownRequiresLock() const noexcept;
    [[nodiscard]] HRESULT CreateSourceAttributes(IMFAttributes* activateAttributes) noexcept;

    ModuleObjectToken moduleToken_;
    bool mfStarted_ = false;
    mutable wil::srwlock lock_;
    State state_ = State::Stopped;

    um::CameraSettings settings_;
    wil::com_ptr_nothrow<IMFMediaEventQueue> eventQueue_;
    wil::com_ptr_nothrow<IMFPresentationDescriptor> presentationDescriptor_;
    wil::com_ptr_nothrow<IMFAttributes> attributes_;
    wil::com_ptr_nothrow<MediaStream> stream_;
};

}  // namespace mwb::vcam

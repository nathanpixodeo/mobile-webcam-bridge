// The single video stream of the virtual camera source. It offers every advertised mode and
// subscribes to the hub with the one the pipeline selected.
//
// State semantics follow Microsoft's Windows-Camera VirtualCamera sample (SimpleMediaStream):
// Start() from the source sends MEStreamStarted, SetStreamState() switches without events, and a
// stopped stream rejects sample requests. Unlike that sample, requests are paced (FrameDelivery).
//
// Lock order: MediaSource::lock_ → MediaStream::lock_. The stream never calls into the source while
// holding its lock, and the delivery thread never takes either lock.
#pragma once

#include "Framework.h"
#include "FrameDelivery.h"
#include "MediaTypes.h"
#include "ModuleLifetime.h"

#include <mwb/um/CameraConfig.h>

#include <memory>

namespace mwb::vcam {

class MediaStream final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>> {
public:
    MediaStream() = default;
    ~MediaStream();

    HRESULT RuntimeClassInitialize(IMFMediaSource* parent, DWORD streamId, const um::CameraSettings& settings) noexcept;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* eventValue) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // Called by MediaSource (with the source lock held).
    [[nodiscard]] HRESULT Start(IMFMediaType* mediaType) noexcept;
    [[nodiscard]] HRESULT Stop(bool sendEvent) noexcept;
    void Shutdown() noexcept;
    [[nodiscard]] HRESULT SetAllocator(IMFVideoSampleAllocator* allocator) noexcept;
    [[nodiscard]] HRESULT GetAttributes(IMFAttributes** attributes) noexcept;
    [[nodiscard]] DWORD Id() const noexcept { return id_; }

private:
    [[nodiscard]] HRESULT CheckShutdownRequiresLock() const noexcept;
    [[nodiscard]] HRESULT StartRequiresLock(IMFMediaType* newMediaType, bool sendEvent) noexcept;
    void StopRequiresLock() noexcept;
    [[nodiscard]] HRESULT SetStreamAttributes(IMFAttributes* attributes) const noexcept;

    ModuleObjectToken moduleToken_;
    mutable wil::srwlock lock_;

    DWORD id_ = 0;
    um::CameraSettings settings_;
    frame::ModeList modes_;     // advertised, in the descriptor's order
    frame::VideoMode mode_{};   // of the current media type
    bool shutdown_ = false;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;

    wil::com_ptr_nothrow<IMFMediaSource> parent_;  // released on Shutdown (breaks the cycle)
    wil::com_ptr_nothrow<IMFMediaEventQueue> eventQueue_;
    wil::com_ptr_nothrow<IMFAttributes> attributes_;
    wil::com_ptr_nothrow<IMFStreamDescriptor> descriptor_;
    wil::com_ptr_nothrow<IMFVideoSampleAllocator> allocator_;
    bool allocatorInitialized_ = false;
    wil::com_ptr_nothrow<IMFMediaType> mediaType_;
    OutputFormat format_ = OutputFormat::Nv12;
    std::unique_ptr<FrameDelivery> delivery_;
};

}  // namespace mwb::vcam

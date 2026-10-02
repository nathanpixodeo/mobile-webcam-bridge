// Capture output pin of the DirectShow virtual camera.
//
// Threading: FillBuffer/OnThreadCreate/OnThreadDestroy run on the CSourceStream worker thread;
// connection and IAMStreamConfig calls run on application threads under the filter state lock.
// The worker thread must never take the state lock (Stop holds it while waiting for the worker),
// so the negotiated format is mirrored under the separate `formatLock_`. The negotiated mode is
// fixed for a streaming run: the worker subscribes to it when the thread starts.
#pragma once

#include "Framework.h"
#include "FormatCatalog.h"

#include <mwb/um/CameraConfig.h>
#include <mwb/um/PipeFrameReceiver.h>
#include <mwb/um/Placeholder.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace mwb::dshow {

class VCamFilter;

class VCamPin final : public CSourceStream, public IAMStreamConfig, public IKsPropertySet, public IAMPushSource {
public:
    VCamPin(HRESULT* result, VCamFilter* filter, const um::CameraSettings& settings);
    ~VCamPin() override;

    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** object) override;

    // IQualityControl: a live source cannot slow down or speed up.
    STDMETHODIMP Notify(IBaseFilter* sender, Quality quality) override;

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* mediaType) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** mediaType) override;
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** mediaType, BYTE* caps) override;

    // IKsPropertySet: answers PIN_CATEGORY_CAPTURE, which capture graph builders look for.
    STDMETHODIMP Set(REFGUID propertySet, DWORD propertyId, LPVOID instanceData, DWORD instanceSize, LPVOID data,
                     DWORD dataSize) override;
    STDMETHODIMP Get(REFGUID propertySet, DWORD propertyId, LPVOID instanceData, DWORD instanceSize, LPVOID data,
                     DWORD dataSize, DWORD* returned) override;
    STDMETHODIMP QuerySupported(REFGUID propertySet, DWORD propertyId, DWORD* support) override;

    // IAMLatency / IAMPushSource
    STDMETHODIMP GetLatency(REFERENCE_TIME* latency) override;
    STDMETHODIMP GetPushSourceFlags(ULONG* flags) override;
    STDMETHODIMP SetPushSourceFlags(ULONG flags) override;
    STDMETHODIMP SetStreamOffset(REFERENCE_TIME offset) override;
    STDMETHODIMP GetStreamOffset(REFERENCE_TIME* offset) override;
    STDMETHODIMP GetMaxStreamOffset(REFERENCE_TIME* maxOffset) override;
    STDMETHODIMP SetMaxStreamOffset(REFERENCE_TIME maxOffset) override;

protected:
    // CBasePin / CBaseOutputPin / CSourceStream
    HRESULT CheckMediaType(const CMediaType* mediaType) override;
    HRESULT GetMediaType(int position, CMediaType* mediaType) override;
    HRESULT SetMediaType(const CMediaType* mediaType) override;
    HRESULT DecideBufferSize(IMemAllocator* allocator, ALLOCATOR_PROPERTIES* request) override;
    HRESULT FillBuffer(IMediaSample* sample) override;
    HRESULT OnThreadCreate() override;
    HRESULT OnThreadDestroy() override;

private:
    struct NegotiatedFormat {
        CatalogEntry entry{};
        std::uint32_t strideInPixels = 0;  // biWidth; 0 until a type is set
    };

    [[nodiscard]] VCamFilter* Filter() const noexcept;
    [[nodiscard]] NegotiatedFormat CurrentFormat() const;
    [[nodiscard]] CMediaType CopyConnectionType() const;
    void AdoptDynamicFormatChange(IMediaSample* sample);
    void WaitForNextFrame();
    [[nodiscard]] const std::uint8_t* CurrentFrame();
    void Timestamp(IMediaSample* sample);

    const um::CameraSettings settings_;
    const FormatCatalog catalog_;
    int preferredIndex_ = 0;  // entry offered first; guarded by the filter state lock

    mutable CCritSec formatLock_;
    NegotiatedFormat negotiated_;  // guarded by formatLock_

    // Worker-thread state.
    HANDLE mmcss_ = nullptr;  // MMCSS registration of the worker thread
    frame::VideoMode streamMode_{};  // the negotiated mode, snapshotted in OnThreadCreate
    std::unique_ptr<um::PipeFrameReceiver> receiver_;
    std::unique_ptr<um::PlaceholderGenerator> placeholder_;
    std::vector<std::uint8_t> placeholderFrame_;
    std::uint64_t placeholderIndex_ = 0;
    LONGLONG lastDelivery_ = 0;
    REFERENCE_TIME syntheticTime_ = 0;
    REFERENCE_TIME lastStart_ = -1;
    REFERENCE_TIME streamOffsetSnapshot_ = 0;
    bool discontinuity_ = true;

    REFERENCE_TIME streamOffset_ = 0;     // guarded by the filter state lock
    REFERENCE_TIME maxStreamOffset_ = 0;  // guarded by the filter state lock
};

}  // namespace mwb::dshow

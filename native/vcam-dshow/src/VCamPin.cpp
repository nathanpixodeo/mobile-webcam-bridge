#include "Framework.h"

#include "VCamPin.h"

#include "VCamFilter.h"

#include <mwb/um/ColorConvert.h>
#include <mwb/um/Tracing.h>

#include <algorithm>

#include <avrt.h>

namespace mwb::dshow {

namespace {

// Time in 100 ns units from the performance counter (the clock DirectShow reference clocks use).
LONGLONG Now100ns() noexcept {
    static const LONGLONG frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    // Split to avoid overflowing counter × 10^7.
    return (counter.QuadPart / frequency) * 10'000'000 + (counter.QuadPart % frequency) * 10'000'000 / frequency;
}

DWORD ToWaitMilliseconds(LONGLONG duration100ns) noexcept {
    return duration100ns <= 0 ? 0 : static_cast<DWORD>((duration100ns + 9'999) / 10'000);
}

}  // namespace

VCamPin::VCamPin(HRESULT* result, VCamFilter* filter, const um::CameraSettings& settings)
    : CSourceStream(NAME("MobileWebcamBridge Capture Pin"), result, filter, L"Capture"),
      settings_(settings),
      catalog_(settings.AdvertisedModes()) {}

VCamPin::~VCamPin() = default;

VCamFilter* VCamPin::Filter() const noexcept {
    return static_cast<VCamFilter*>(m_pFilter);
}

STDMETHODIMP VCamPin::NonDelegatingQueryInterface(REFIID riid, void** object) {
    CheckPointer(object, E_POINTER);
    if (riid == IID_IAMStreamConfig) return GetInterface(static_cast<IAMStreamConfig*>(this), object);
    if (riid == IID_IKsPropertySet) return GetInterface(static_cast<IKsPropertySet*>(this), object);
    if (riid == IID_IAMPushSource) return GetInterface(static_cast<IAMPushSource*>(this), object);
    if (riid == IID_IAMLatency) return GetInterface(static_cast<IAMLatency*>(static_cast<IAMPushSource*>(this)), object);
    return CSourceStream::NonDelegatingQueryInterface(riid, object);
}

STDMETHODIMP VCamPin::Notify(IBaseFilter* /*sender*/, Quality /*quality*/) {
    return S_OK;
}

// ---- Media type negotiation ----

HRESULT VCamPin::CheckMediaType(const CMediaType* mediaType) {
    CheckPointer(mediaType, E_POINTER);
    return catalog_.Match(*mediaType) ? S_OK : VFW_E_TYPE_NOT_ACCEPTED;
}

HRESULT VCamPin::GetMediaType(int position, CMediaType* mediaType) {
    CheckPointer(mediaType, E_POINTER);
    if (position < 0) return E_INVALIDARG;
    if (position >= catalog_.Count()) return VFW_S_NO_MORE_ITEMS;

    CAutoLock lock(m_pFilter->pStateLock());
    // The preferred entry first, then the remaining entries in catalog order.
    const int index = position == 0 ? preferredIndex_ : (position <= preferredIndex_ ? position - 1 : position);
    return catalog_.GetMediaType(index, mediaType);
}

HRESULT VCamPin::SetMediaType(const CMediaType* mediaType) {
    CheckPointer(mediaType, E_POINTER);
    const std::optional<CatalogEntry> entry = catalog_.Match(*mediaType);
    if (!entry) return VFW_E_TYPE_NOT_ACCEPTED;

    CAutoLock lock(&formatLock_);
    const HRESULT hr = CSourceStream::SetMediaType(mediaType);
    if (FAILED(hr)) return hr;
    const auto* info = reinterpret_cast<const VIDEOINFOHEADER*>(mediaType->Format());
    negotiated_ = NegotiatedFormat{*entry, static_cast<std::uint32_t>(info->bmiHeader.biWidth)};
    return S_OK;
}

VCamPin::NegotiatedFormat VCamPin::CurrentFormat() const {
    CAutoLock lock(&formatLock_);
    return negotiated_;
}

CMediaType VCamPin::CopyConnectionType() const {
    // m_mt can change on the worker thread (dynamic format change), hence formatLock_.
    CAutoLock lock(&formatLock_);
    return m_mt;
}

HRESULT VCamPin::DecideBufferSize(IMemAllocator* allocator, ALLOCATOR_PROPERTIES* request) {
    CheckPointer(allocator, E_POINTER);
    CheckPointer(request, E_POINTER);
    CAutoLock lock(m_pFilter->pStateLock());

    const NegotiatedFormat format = CurrentFormat();
    if (format.strideInPixels == 0) return E_UNEXPECTED;

    // Two buffers: one being filled while the previous one is still downstream.
    request->cBuffers = std::max<long>(request->cBuffers, 2);
    request->cbBuffer = static_cast<long>(
        FormatCatalog::ImageBytes(format.entry.format, format.strideInPixels, format.entry.mode.height));
    if (request->cbAlign <= 0) request->cbAlign = 1;

    ALLOCATOR_PROPERTIES actual{};
    const HRESULT hr = allocator->SetProperties(request, &actual);
    if (FAILED(hr)) return hr;
    return actual.cbBuffer < request->cbBuffer ? E_FAIL : S_OK;
}

// ---- Streaming (worker thread) ----

HRESULT VCamPin::OnThreadCreate() {
    streamMode_ = CurrentFormat().entry.mode;
    try {
        placeholder_ = std::make_unique<um::PlaceholderGenerator>(streamMode_.width, streamMode_.height,
                                                                  streamMode_.fpsNum / streamMode_.fpsDen);
        placeholderFrame_.resize(placeholder_->FrameBytes());
        receiver_ = std::make_unique<um::PipeFrameReceiver>(settings_.pipeName, streamMode_);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    // Connect to the hub only now, while streaming. Without a receiver we still stream the
    // placeholder, so a missing hub never breaks the client's graph.
    if (const HRESULT hr = receiver_->Start(); FAILED(hr)) {
        um::trace::Writef(um::trace::Level::Warning, L"Frame receiver failed to start: 0x{:08X}", static_cast<std::uint32_t>(hr));
        receiver_.reset();
    }

    // Join the MMCSS "Capture" class like real capture drivers' streaming threads.
    DWORD taskIndex = 0;
    mmcss_ = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);

    // The caller (CSourceStream::Active) holds the filter state lock while we run, so reading
    // the IAMPushSource offset here is synchronised; the worker uses this snapshot afterwards.
    streamOffsetSnapshot_ = streamOffset_;
    lastDelivery_ = Now100ns() - FormatCatalog::FrameInterval(streamMode_);
    syntheticTime_ = 0;
    lastStart_ = -1;
    discontinuity_ = true;
    placeholderIndex_ = 0;
    return S_OK;
}

HRESULT VCamPin::OnThreadDestroy() {
    receiver_.reset();  // stops the receive thread and closes the pipe
    if (mmcss_ != nullptr) {
        AvRevertMmThreadCharacteristics(mmcss_);
        mmcss_ = nullptr;
    }
    return S_OK;
}

void VCamPin::WaitForNextFrame() {
    const REFERENCE_TIME interval = FormatCatalog::FrameInterval(streamMode_);
    const REFERENCE_TIME minInterval = interval * 2 / 3;  // at most 1.5 × the nominal rate
    for (;;) {
        const LONGLONG now = Now100ns();
        const LONGLONG elapsed = now - lastDelivery_;
        const bool fresh = receiver_ && receiver_->HasFreshFrame();
        const LONGLONG due = fresh ? minInterval : interval;
        if (elapsed >= due) {
            lastDelivery_ = now;
            return;
        }
        const DWORD waitMs = ToWaitMilliseconds(due - elapsed);
        if (receiver_) {
            WaitForSingleObject(receiver_->FrameAvailableEvent(), waitMs);
        } else {
            Sleep(waitMs);
        }
    }
}

const std::uint8_t* VCamPin::CurrentFrame() {
    if (receiver_) {
        const um::ReceivedFrame& frame = receiver_->AcquireLatest();
        if (receiver_->IsConnected() && frame.valid) {
            placeholderIndex_ = 0;
            return frame.nv12.data();
        }
    }
    placeholder_->Render(placeholderIndex_++, placeholderFrame_.data());
    return placeholderFrame_.data();
}

void VCamPin::AdoptDynamicFormatChange(IMediaSample* sample) {
    // Video renderers announce a new stride by attaching a media type to a sample.
    AM_MEDIA_TYPE* proposed = nullptr;
    if (sample->GetMediaType(&proposed) != S_OK || proposed == nullptr) return;
    CMediaType mediaType(*proposed);
    DeleteMediaType(proposed);
    // Only the stride may change while streaming: the receiver is subscribed to streamMode_.
    const std::optional<CatalogEntry> entry = catalog_.Match(mediaType);
    if (!entry || !(entry->mode == streamMode_) || FAILED(SetMediaType(&mediaType))) {
        um::trace::Write(um::trace::Level::Warning, L"Ignoring unsupported dynamic format change");
    }
}

void VCamPin::Timestamp(IMediaSample* sample) {
    const REFERENCE_TIME interval = FormatCatalog::FrameInterval(streamMode_);
    REFERENCE_TIME start = syntheticTime_;

    // While running with a clock, stamp with the stream time of capture, like a real camera.
    CRefTime streamTime;
    if (Filter()->IsRunning() && SUCCEEDED(m_pFilter->StreamTime(streamTime))) {
        start = static_cast<REFERENCE_TIME>(streamTime) + streamOffsetSnapshot_;
    }
    if (start <= lastStart_) start = lastStart_ + 1;  // strictly increasing
    REFERENCE_TIME stop = start + interval;

    sample->SetTime(&start, &stop);
    lastStart_ = start;
    syntheticTime_ = stop;
}

HRESULT VCamPin::FillBuffer(IMediaSample* sample) {
    CheckPointer(sample, E_POINTER);

    AdoptDynamicFormatChange(sample);
    WaitForNextFrame();

    const NegotiatedFormat format = CurrentFormat();
    const std::uint32_t height = streamMode_.height;
    const PixelFormat pixelFormat = format.entry.format;
    const std::size_t stride = FormatCatalog::StrideBytes(pixelFormat, format.strideInPixels);
    const std::size_t imageBytes = FormatCatalog::ImageBytes(pixelFormat, format.strideInPixels, height);

    BYTE* data = nullptr;
    HRESULT hr = sample->GetPointer(&data);
    if (FAILED(hr)) return hr;
    if (data == nullptr || static_cast<std::size_t>(sample->GetSize()) < imageBytes) return E_UNEXPECTED;

    const um::color::Nv12Image source = um::color::PackedNv12(CurrentFrame(), streamMode_.width, height);
    switch (pixelFormat) {
        case PixelFormat::Yuy2: um::color::Nv12ToYuy2(source, data, stride); break;
        case PixelFormat::Nv12: um::color::CopyNv12(source, data, stride); break;
        case PixelFormat::I420: um::color::Nv12ToI420(source, data, stride); break;
        case PixelFormat::Rgb24: um::color::Nv12ToRgb24BottomUp(source, data, stride); break;
    }

    hr = sample->SetActualDataLength(static_cast<long>(imageBytes));
    if (FAILED(hr)) return hr;
    Timestamp(sample);
    sample->SetSyncPoint(TRUE);
    sample->SetDiscontinuity(discontinuity_ ? TRUE : FALSE);
    discontinuity_ = false;
    return S_OK;
}

// ---- IAMStreamConfig ----

STDMETHODIMP VCamPin::SetFormat(AM_MEDIA_TYPE* mediaType) {
    CAutoLock lock(m_pFilter->pStateLock());
    if (mediaType == nullptr) {  // NULL restores the default format
        preferredIndex_ = 0;
        return S_OK;
    }
    const std::optional<CatalogEntry> entry = catalog_.Match(*mediaType);
    if (!entry) return VFW_E_INVALIDMEDIATYPE;
    preferredIndex_ = catalog_.IndexOf(*entry);

    if (!IsConnected()) return S_OK;  // used on the next connection
    if (CopyConnectionType() == CMediaType(*mediaType)) return S_OK;
    if (m_pFilter->IsActive()) return VFW_E_NOT_STOPPED;

    // Ask the downstream filter first, then reconnect with the new preferred type.
    if (GetConnected()->QueryAccept(mediaType) != S_OK) return VFW_E_INVALIDMEDIATYPE;
    IFilterGraph* graph = m_pFilter->GetFilterGraph();
    return graph != nullptr ? graph->Reconnect(this) : VFW_E_NOT_IN_GRAPH;
}

STDMETHODIMP VCamPin::GetFormat(AM_MEDIA_TYPE** mediaType) {
    CheckPointer(mediaType, E_POINTER);
    CAutoLock lock(m_pFilter->pStateLock());

    CMediaType current;
    if (IsConnected()) {
        current = CopyConnectionType();
    } else {
        const HRESULT hr = catalog_.GetMediaType(preferredIndex_, &current);
        if (FAILED(hr)) return hr;
    }
    *mediaType = CreateMediaType(&current);
    return *mediaType != nullptr ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP VCamPin::GetNumberOfCapabilities(int* count, int* size) {
    CheckPointer(count, E_POINTER);
    CheckPointer(size, E_POINTER);
    *count = catalog_.Count();
    *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP VCamPin::GetStreamCaps(int index, AM_MEDIA_TYPE** mediaType, BYTE* caps) {
    CheckPointer(mediaType, E_POINTER);
    CheckPointer(caps, E_POINTER);
    if (index < 0) return E_INVALIDARG;
    if (index >= catalog_.Count()) return S_FALSE;

    CMediaType entry;
    const HRESULT hr = catalog_.GetMediaType(index, &entry);
    if (FAILED(hr)) return hr;
    *mediaType = CreateMediaType(&entry);
    if (*mediaType == nullptr) return E_OUTOFMEMORY;
    catalog_.FillCaps(index, *reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(caps));
    return S_OK;
}

// ---- IKsPropertySet ----

STDMETHODIMP VCamPin::Set(REFGUID /*propertySet*/, DWORD /*propertyId*/, LPVOID /*instanceData*/, DWORD /*instanceSize*/,
                          LPVOID /*data*/, DWORD /*dataSize*/) {
    return E_NOTIMPL;
}

STDMETHODIMP VCamPin::Get(REFGUID propertySet, DWORD propertyId, LPVOID /*instanceData*/, DWORD /*instanceSize*/,
                          LPVOID data, DWORD dataSize, DWORD* returned) {
    if (propertySet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (propertyId != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (data == nullptr && returned == nullptr) return E_POINTER;
    if (returned != nullptr) *returned = sizeof(GUID);
    if (data == nullptr) return S_OK;  // size query only
    if (dataSize < sizeof(GUID)) return E_UNEXPECTED;
    *static_cast<GUID*>(data) = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

STDMETHODIMP VCamPin::QuerySupported(REFGUID propertySet, DWORD propertyId, DWORD* support) {
    if (propertySet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (propertyId != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (support != nullptr) *support = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

// ---- IAMLatency / IAMPushSource ----

STDMETHODIMP VCamPin::GetLatency(REFERENCE_TIME* latency) {
    CheckPointer(latency, E_POINTER);
    const NegotiatedFormat format = CurrentFormat();
    *latency = FormatCatalog::FrameInterval(format.strideInPixels != 0 ? format.entry.mode : catalog_.DefaultMode());
    return S_OK;
}

STDMETHODIMP VCamPin::GetPushSourceFlags(ULONG* flags) {
    CheckPointer(flags, E_POINTER);
    *flags = 0;  // time stamps are stream times derived from the graph clock
    return S_OK;
}

STDMETHODIMP VCamPin::SetPushSourceFlags(ULONG /*flags*/) {
    return E_NOTIMPL;
}

STDMETHODIMP VCamPin::SetStreamOffset(REFERENCE_TIME offset) {
    CAutoLock lock(m_pFilter->pStateLock());
    streamOffset_ = offset;
    return S_OK;
}

STDMETHODIMP VCamPin::GetStreamOffset(REFERENCE_TIME* offset) {
    CheckPointer(offset, E_POINTER);
    CAutoLock lock(m_pFilter->pStateLock());
    *offset = streamOffset_;
    return S_OK;
}

STDMETHODIMP VCamPin::GetMaxStreamOffset(REFERENCE_TIME* maxOffset) {
    CheckPointer(maxOffset, E_POINTER);
    CAutoLock lock(m_pFilter->pStateLock());
    *maxOffset = maxStreamOffset_;
    return S_OK;
}

STDMETHODIMP VCamPin::SetMaxStreamOffset(REFERENCE_TIME maxOffset) {
    CAutoLock lock(m_pFilter->pStateLock());
    maxStreamOffset_ = maxOffset;
    return S_OK;
}

}  // namespace mwb::dshow

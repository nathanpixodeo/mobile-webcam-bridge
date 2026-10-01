#include "Framework.h"

#include "MediaSource.h"

#include <mwb/um/Tracing.h>

namespace mwb::vcam {

namespace {

PROPVARIANT SystemTimeVariant() noexcept {
    PROPVARIANT value{};
    value.vt = VT_I8;
    value.hVal.QuadPart = MFGetSystemTime();
    return value;  // no heap payload: nothing to PropVariantClear
}

}  // namespace

MediaSource::~MediaSource() {
    if (mfStarted_) MFShutdown();
}

HRESULT MediaSource::RuntimeClassInitialize(IMFAttributes* activateAttributes) noexcept try {
    // Frame Server has Media Foundation running already; this keeps us correct in any other host.
    RETURN_IF_FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    mfStarted_ = true;

    auto lock = lock_.lock_exclusive();

    const HRESULT settingsResult = um::LoadCameraSettings(settings_);
    if (FAILED(settingsResult)) {
        trace::Writef(trace::Level::Warning, L"Camera settings unavailable (0x{:08X}), using defaults",
                      static_cast<std::uint32_t>(settingsResult));
    }

    RETURN_IF_FAILED(CreateSourceAttributes(activateAttributes));
    RETURN_IF_FAILED(MFCreateEventQueue(&eventQueue_));

    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaStream>(stream_.put(), this, kStreamId, settings_));
    // The stream holds a reference to us; if initialisation fails below, break that cycle so the
    // half-built source can still be destroyed.
    auto releaseStream = wil::scope_exit([&]() noexcept {
        stream_->Shutdown();
        stream_.reset();
    });

    wil::com_ptr_nothrow<IMFStreamDescriptor> streamDescriptor;
    RETURN_IF_FAILED(stream_->GetStreamDescriptor(&streamDescriptor));
    IMFStreamDescriptor* descriptors[] = {streamDescriptor.get()};
    RETURN_IF_FAILED(MFCreatePresentationDescriptor(ARRAYSIZE(descriptors), descriptors, &presentationDescriptor_));

    releaseStream.release();
    state_ = State::Stopped;
    return S_OK;
}
CATCH_RETURN()

bool MediaSource::IsShutdown() const noexcept {
    auto lock = lock_.lock_shared();
    return state_ == State::Shutdown;
}

HRESULT MediaSource::CreateSourceAttributes(IMFAttributes* activateAttributes) noexcept {
    RETURN_IF_FAILED(MFCreateAttributes(&attributes_, 4));
    if (activateAttributes != nullptr) {
        RETURN_IF_FAILED(activateAttributes->CopyAllItems(attributes_.get()));
    }

    // The legacy profile is mandatory so applications that know nothing about sensor profiles can
    // still use the camera. It accepts every type we offer, whatever frame rate is installed.
    wil::com_ptr_nothrow<IMFSensorProfileCollection> profiles;
    wil::com_ptr_nothrow<IMFSensorProfile> legacy;
    RETURN_IF_FAILED(MFCreateSensorProfileCollection(&profiles));
    RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &legacy));
    RETURN_IF_FAILED(legacy->AddProfileFilter(kStreamId, L"((RES==;FRT==;SUT==))"));
    RETURN_IF_FAILED(profiles->AddProfile(legacy.get()));
    RETURN_IF_FAILED(attributes_->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.get()));
    return S_OK;
}

HRESULT MediaSource::CheckShutdownRequiresLock() const noexcept {
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return (eventQueue_ && stream_ && presentationDescriptor_) ? S_OK : E_UNEXPECTED;
}

// ---- IMFMediaEventGenerator ----

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    return eventQueue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    return eventQueue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    // GetEvent may block indefinitely: never hold the lock while waiting.
    wil::com_ptr_nothrow<IMFMediaEventQueue> queue;
    {
        auto lock = lock_.lock_exclusive();
        RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
        queue = eventQueue_;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) {
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    return eventQueue_->QueueEventParamVar(type, extendedType, status, value);
}

// ---- IMFMediaSource ----

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    *descriptor = nullptr;
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    return presentationDescriptor_->Clone(descriptor);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    RETURN_HR_IF_NULL(E_POINTER, characteristics);
    *characteristics = 0;
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause() {
    return MF_E_INVALID_STATE_TRANSITION;  // live source: pausing is meaningless
}

STDMETHODIMP MediaSource::Shutdown() {
    auto lock = lock_.lock_exclusive();
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    state_ = State::Shutdown;

    if (stream_) stream_->Shutdown();
    if (eventQueue_) LOG_IF_FAILED(eventQueue_->Shutdown());
    stream_.reset();
    eventQueue_.reset();
    presentationDescriptor_.reset();
    attributes_.reset();
    trace::Write(trace::Level::Info, L"Source shut down");
    return S_OK;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                                const PROPVARIANT* startPosition) {
    RETURN_HR_IF(E_INVALIDARG, descriptor == nullptr || startPosition == nullptr);
    RETURN_HR_IF(MF_E_UNSUPPORTED_TIME_FORMAT, timeFormat != nullptr && *timeFormat != GUID_NULL);

    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());

    DWORD count = 0;
    RETURN_IF_FAILED(descriptor->GetStreamDescriptorCount(&count));
    RETURN_HR_IF(E_INVALIDARG, count != 1);

    BOOL selected = FALSE;
    wil::com_ptr_nothrow<IMFStreamDescriptor> requested;
    RETURN_IF_FAILED(descriptor->GetStreamDescriptorByIndex(0, &selected, &requested));
    DWORD streamId = 0;
    RETURN_IF_FAILED(requested->GetStreamIdentifier(&streamId));
    RETURN_HR_IF(E_INVALIDARG, streamId != kStreamId);

    BOOL wasSelected = FALSE;
    wil::com_ptr_nothrow<IMFStreamDescriptor> ours;
    RETURN_IF_FAILED(presentationDescriptor_->GetStreamDescriptorByIndex(0, &wasSelected, &ours));

    if (selected) {
        wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
        wil::com_ptr_nothrow<IMFMediaType> mediaType;
        RETURN_IF_FAILED(requested->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED(handler->GetCurrentMediaType(&mediaType));

        RETURN_IF_FAILED(presentationDescriptor_->SelectStream(0));
        // MENewStream for a stream the client has not seen started yet, MEUpdatedStream otherwise.
        const MediaEventType streamEvent = (wasSelected && state_ == State::Started) ? MEUpdatedStream : MENewStream;
        RETURN_IF_FAILED(eventQueue_->QueueEventParamUnk(streamEvent, GUID_NULL, S_OK,
                                                         static_cast<IMFMediaStream*>(stream_.get())));
        RETURN_IF_FAILED(stream_->Start(mediaType.get()));
    } else if (wasSelected) {
        RETURN_IF_FAILED(presentationDescriptor_->DeselectStream(0));
        RETURN_IF_FAILED(stream_->Stop(true));
    }

    const PROPVARIANT startTime = SystemTimeVariant();
    RETURN_IF_FAILED(eventQueue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &startTime));
    state_ = State::Started;
    return S_OK;
}

STDMETHODIMP MediaSource::Stop() {
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    RETURN_HR_IF(MF_E_INVALID_STATE_TRANSITION, state_ != State::Started);

    RETURN_IF_FAILED(stream_->Stop(true));
    RETURN_IF_FAILED(presentationDescriptor_->DeselectStream(0));

    const PROPVARIANT stopTime = SystemTimeVariant();
    RETURN_IF_FAILED(eventQueue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &stopTime));
    state_ = State::Stopped;
    return S_OK;
}

// ---- IMFMediaSourceEx ----

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    *attributes = nullptr;
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    // Frame Server requires a reference to the live store, not a copy.
    return attributes_.copy_to(attributes);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    *attributes = nullptr;
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    RETURN_HR_IF(MF_E_NOT_FOUND, streamId != kStreamId);
    return stream_->GetAttributes(attributes);
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown* /*manager*/) {
    // v1 produces CPU samples only; Frame Server ignores the result of this call.
    return E_NOTIMPL;
}

// ---- IMFGetService ----

STDMETHODIMP MediaSource::GetService(REFGUID /*service*/, REFIID /*riid*/, LPVOID* object) {
    RETURN_HR_IF_NULL(E_POINTER, object);
    *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// ---- IKsControl ----
// ERROR_SET_NOT_FOUND is what the AVStream class driver returns when a miniport registers no
// handler for a KS request; clients treat it as "control not supported".

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID /*propertyData*/,
                                     ULONG /*dataLength*/, ULONG* bytesReturned) {
    RETURN_HR_IF(E_INVALIDARG, property == nullptr || propertyLength < sizeof(KSPROPERTY));
    if (bytesReturned != nullptr) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsMethod(PKSMETHOD /*method*/, ULONG /*methodLength*/, LPVOID /*methodData*/,
                                   ULONG /*dataLength*/, ULONG* bytesReturned) {
    if (bytesReturned != nullptr) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsEvent(PKSEVENT /*event*/, ULONG /*eventLength*/, LPVOID /*eventData*/,
                                  ULONG /*dataLength*/, ULONG* bytesReturned) {
    if (bytesReturned != nullptr) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ---- IMFSampleAllocatorControl ----

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) {
    RETURN_HR_IF_NULL(E_POINTER, allocator);
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    RETURN_HR_IF(MF_E_NOT_FOUND, outputStreamId != kStreamId);

    wil::com_ptr_nothrow<IMFVideoSampleAllocator> videoAllocator;
    RETURN_IF_FAILED(allocator->QueryInterface(IID_PPV_ARGS(&videoAllocator)));
    return stream_->SetAllocator(videoAllocator.get());
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) {
    RETURN_HR_IF(E_POINTER, inputStreamId == nullptr || usage == nullptr);
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    RETURN_HR_IF(MF_E_NOT_FOUND, outputStreamId != kStreamId);
    *inputStreamId = outputStreamId;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

}  // namespace mwb::vcam

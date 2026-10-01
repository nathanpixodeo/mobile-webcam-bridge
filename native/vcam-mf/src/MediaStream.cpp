#include "Framework.h"

#include "MediaStream.h"

#include <mwb/um/Tracing.h>

#include <array>

namespace mwb::vcam {

namespace {
// Samples the allocator keeps in its pool: enough for the Frame Server fan-out plus slack.
constexpr DWORD kSamplePoolSize = 10;
}  // namespace

MediaStream::~MediaStream() {
    // Normally Shutdown() already ran; this covers abandonment without it.
    if (delivery_) delivery_->Stop();
}

HRESULT MediaStream::RuntimeClassInitialize(IMFMediaSource* parent, DWORD streamId,
                                            const um::CameraSettings& settings) noexcept try {
    RETURN_HR_IF_NULL(E_INVALIDARG, parent);
    auto lock = lock_.lock_exclusive();

    parent_ = parent;
    id_ = streamId;
    settings_ = settings;

    std::array<wil::com_ptr_nothrow<IMFMediaType>, kMediaTypeCount> types;
    RETURN_IF_FAILED(CreateMediaTypes(settings_.mode, types));

    RETURN_IF_FAILED(MFCreateAttributes(&attributes_, 4));
    RETURN_IF_FAILED(SetStreamAttributes(attributes_.get()));
    RETURN_IF_FAILED(MFCreateEventQueue(&eventQueue_));

    IMFMediaType* rawTypes[kMediaTypeCount] = {types[0].get(), types[1].get()};
    RETURN_IF_FAILED(MFCreateStreamDescriptor(id_, static_cast<DWORD>(kMediaTypeCount), rawTypes, &descriptor_));

    wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
    RETURN_IF_FAILED(descriptor_->GetMediaTypeHandler(&handler));
    RETURN_IF_FAILED(handler->SetCurrentMediaType(types[0].get()));
    RETURN_IF_FAILED(SetStreamAttributes(descriptor_.get()));
    return S_OK;
}
CATCH_RETURN()

HRESULT MediaStream::SetStreamAttributes(IMFAttributes* attributes) const noexcept {
    RETURN_IF_FAILED(attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    RETURN_IF_FAILED(attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, id_));
    RETURN_IF_FAILED(attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    RETURN_IF_FAILED(attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
    return S_OK;
}

HRESULT MediaStream::CheckShutdownRequiresLock() const noexcept {
    if (shutdown_) return MF_E_SHUTDOWN;
    return eventQueue_ ? S_OK : E_UNEXPECTED;
}

// ---- IMFMediaEventGenerator ----

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    return eventQueue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    return eventQueue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    // GetEvent may block indefinitely: never hold the lock while waiting.
    wil::com_ptr_nothrow<IMFMediaEventQueue> queue;
    {
        auto lock = lock_.lock_exclusive();
        RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
        queue = eventQueue_;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) {
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    return eventQueue_->QueueEventParamVar(type, extendedType, status, value);
}

// ---- IMFMediaStream ----

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    RETURN_HR_IF_NULL(E_POINTER, source);
    *source = nullptr;
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    return parent_.copy_to(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    *descriptor = nullptr;
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    return descriptor_.copy_to(descriptor);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    if (state_ != MF_STREAM_STATE_RUNNING || !delivery_) return MF_E_INVALIDREQUEST;
    return delivery_->Enqueue(token);
}

// ---- IMFMediaStream2 ----

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());

    if (state == state_) return S_OK;  // note: comparison, not the assignment some samples ship
    switch (state) {
        case MF_STREAM_STATE_PAUSED:
            RETURN_HR_IF(MF_E_INVALID_STATE_TRANSITION, state_ != MF_STREAM_STATE_RUNNING);
            if (delivery_) delivery_->SetPaused(true);
            state_ = MF_STREAM_STATE_PAUSED;
            return S_OK;
        case MF_STREAM_STATE_RUNNING:
            return StartRequiresLock(nullptr, false);
        case MF_STREAM_STATE_STOPPED:
            StopRequiresLock();
            return S_OK;
        default:
            return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    RETURN_HR_IF_NULL(E_POINTER, state);
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    *state = state_;
    return S_OK;
}

// ---- Source-facing ----

HRESULT MediaStream::Start(IMFMediaType* mediaType) noexcept {
    RETURN_HR_IF_NULL(E_INVALIDARG, mediaType);
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED(CheckShutdownRequiresLock());
    return StartRequiresLock(mediaType, true);
}

HRESULT MediaStream::Stop(bool sendEvent) noexcept {
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    StopRequiresLock();
    if (sendEvent) RETURN_IF_FAILED(eventQueue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
    return S_OK;
}

void MediaStream::Shutdown() noexcept {
    auto lock = lock_.lock_exclusive();
    if (shutdown_) return;
    StopRequiresLock();
    shutdown_ = true;
    if (eventQueue_) LOG_IF_FAILED(eventQueue_->Shutdown());
    eventQueue_.reset();
    descriptor_.reset();
    attributes_.reset();
    allocator_.reset();
    mediaType_.reset();
    parent_.reset();
}

HRESULT MediaStream::SetAllocator(IMFVideoSampleAllocator* allocator) noexcept {
    RETURN_HR_IF_NULL(E_POINTER, allocator);
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED(CheckShutdownRequiresLock());
    RETURN_HR_IF(MF_E_INVALIDREQUEST, state_ == MF_STREAM_STATE_RUNNING);
    allocator_ = allocator;
    allocatorInitialized_ = false;
    return S_OK;
}

HRESULT MediaStream::GetAttributes(IMFAttributes** attributes) noexcept {
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    *attributes = nullptr;
    auto lock = lock_.lock_exclusive();
    RETURN_IF_FAILED_EXPECTED(CheckShutdownRequiresLock());
    return attributes_.copy_to(attributes);
}

// ---- Internals ----

HRESULT MediaStream::StartRequiresLock(IMFMediaType* newMediaType, bool sendEvent) noexcept try {
    if (newMediaType != nullptr) {
        OutputFormat format{};
        RETURN_IF_FAILED(ResolveOutputFormat(newMediaType, settings_.mode, &format));
        BOOL same = FALSE;
        if (!mediaType_ || FAILED(mediaType_->Compare(newMediaType, MF_ATTRIBUTES_MATCH_ALL_ITEMS, &same)) || !same) {
            StopRequiresLock();  // format change: tear the pipeline down and rebuild below
            mediaType_ = newMediaType;
            format_ = format;
        }
    }
    if (!mediaType_) {
        // Resumed without ever being started by the source: use the descriptor's current type.
        wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
        RETURN_IF_FAILED(descriptor_->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED(handler->GetCurrentMediaType(&mediaType_));
        RETURN_IF_FAILED(ResolveOutputFormat(mediaType_.get(), settings_.mode, &format_));
    }

    if (!delivery_) {
        if (!allocator_) {
            // Frame Server always provides one (IMFSampleAllocatorControl); other hosts may not.
            RETURN_IF_FAILED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_)));
        }
        if (!allocatorInitialized_) {
            RETURN_IF_FAILED(allocator_->InitializeSampleAllocator(kSamplePoolSize, mediaType_.get()));
            allocatorInitialized_ = true;
        }

        auto delivery = std::make_unique<FrameDelivery>(DeliveryConfig{
            .eventQueue = eventQueue_,
            .allocator = allocator_,
            .mode = settings_.mode,
            .format = format_,
            .pipeName = settings_.pipeName,
        });
        RETURN_IF_FAILED(delivery->Start());
        delivery_ = std::move(delivery);
        trace::Writef(trace::Level::Info, L"Stream started: {}x{}@{} {}", settings_.mode.width, settings_.mode.height,
                      settings_.mode.fpsNum, format_ == OutputFormat::Nv12 ? L"NV12" : L"YUY2");
    }
    delivery_->SetPaused(false);

    if (sendEvent) RETURN_IF_FAILED(eventQueue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
    state_ = MF_STREAM_STATE_RUNNING;
    return S_OK;
}
CATCH_RETURN()

void MediaStream::StopRequiresLock() noexcept {
    state_ = MF_STREAM_STATE_STOPPED;
    if (delivery_) {
        delivery_->Stop();  // joins the delivery thread; it never takes lock_
        delivery_.reset();
        trace::Write(trace::Level::Info, L"Stream stopped");
    }
    if (allocator_ && allocatorInitialized_) {
        LOG_IF_FAILED(allocator_->UninitializeSampleAllocator());
        allocatorInitialized_ = false;
    }
}

}  // namespace mwb::vcam

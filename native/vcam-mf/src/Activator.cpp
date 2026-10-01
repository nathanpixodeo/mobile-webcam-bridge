#include "Framework.h"

#include "Activator.h"

#include <mwb/um/Tracing.h>

namespace mwb::vcam {

HRESULT Activator::RuntimeClassInitialize() noexcept {
    RETURN_IF_FAILED(MFCreateAttributes(&store_, 4));
    // Both reference samples (Microsoft VirtualCamera, VCamSample) advertise this on the activate
    // object; Frame Server then passes associated camera sources (none for a synthetic camera).
    RETURN_IF_FAILED(store_->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1));
    return S_OK;
}

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** object) {
    RETURN_HR_IF_NULL(E_POINTER, object);
    *object = nullptr;

    auto lock = lock_.lock_exclusive();
    if (!source_ || source_->IsShutdown()) {
        // A source the pipeline shut down cannot be restarted: build a fresh one. It copies our
        // attributes (Frame Server's client context included) at creation.
        source_.reset();
        RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaSource>(source_.put(), store_.get()));
        um::trace::Write(um::trace::Level::Info, L"Media source created");
    }
    return source_->QueryInterface(riid, object);
}

STDMETHODIMP Activator::ShutdownObject() {
    wil::com_ptr_nothrow<MediaSource> source;
    {
        auto lock = lock_.lock_exclusive();
        source = std::move(source_);
    }
    if (source) {
        // MF_E_SHUTDOWN just means the pipeline already shut it down.
        const HRESULT hr = source->Shutdown();
        if (FAILED(hr) && hr != MF_E_SHUTDOWN) LOG_HR(hr);
    }
    return S_OK;
}

STDMETHODIMP Activator::DetachObject() {
    auto lock = lock_.lock_exclusive();
    source_.reset();
    return S_OK;
}

// ---- IMFAttributes delegation ----

STDMETHODIMP Activator::GetItem(REFGUID key, PROPVARIANT* itemValue) { return store_->GetItem(key, itemValue); }
STDMETHODIMP Activator::GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) { return store_->GetItemType(key, type); }
STDMETHODIMP Activator::CompareItem(REFGUID key, REFPROPVARIANT itemValue, BOOL* result) {
    return store_->CompareItem(key, itemValue, result);
}
STDMETHODIMP Activator::Compare(IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE matchType, BOOL* result) {
    return store_->Compare(theirs, matchType, result);
}
STDMETHODIMP Activator::GetUINT32(REFGUID key, UINT32* itemValue) { return store_->GetUINT32(key, itemValue); }
STDMETHODIMP Activator::GetUINT64(REFGUID key, UINT64* itemValue) { return store_->GetUINT64(key, itemValue); }
STDMETHODIMP Activator::GetDouble(REFGUID key, double* itemValue) { return store_->GetDouble(key, itemValue); }
STDMETHODIMP Activator::GetGUID(REFGUID key, GUID* itemValue) { return store_->GetGUID(key, itemValue); }
STDMETHODIMP Activator::GetStringLength(REFGUID key, UINT32* length) { return store_->GetStringLength(key, length); }
STDMETHODIMP Activator::GetString(REFGUID key, LPWSTR itemValue, UINT32 size, UINT32* length) {
    return store_->GetString(key, itemValue, size, length);
}
STDMETHODIMP Activator::GetAllocatedString(REFGUID key, LPWSTR* itemValue, UINT32* length) {
    return store_->GetAllocatedString(key, itemValue, length);
}
STDMETHODIMP Activator::GetBlobSize(REFGUID key, UINT32* size) { return store_->GetBlobSize(key, size); }
STDMETHODIMP Activator::GetBlob(REFGUID key, UINT8* buffer, UINT32 bufferSize, UINT32* blobSize) {
    return store_->GetBlob(key, buffer, bufferSize, blobSize);
}
STDMETHODIMP Activator::GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size) {
    return store_->GetAllocatedBlob(key, buffer, size);
}
STDMETHODIMP Activator::GetUnknown(REFGUID key, REFIID riid, LPVOID* object) { return store_->GetUnknown(key, riid, object); }
STDMETHODIMP Activator::SetItem(REFGUID key, REFPROPVARIANT itemValue) { return store_->SetItem(key, itemValue); }
STDMETHODIMP Activator::DeleteItem(REFGUID key) { return store_->DeleteItem(key); }
STDMETHODIMP Activator::DeleteAllItems() { return store_->DeleteAllItems(); }
STDMETHODIMP Activator::SetUINT32(REFGUID key, UINT32 itemValue) { return store_->SetUINT32(key, itemValue); }
STDMETHODIMP Activator::SetUINT64(REFGUID key, UINT64 itemValue) { return store_->SetUINT64(key, itemValue); }
STDMETHODIMP Activator::SetDouble(REFGUID key, double itemValue) { return store_->SetDouble(key, itemValue); }
STDMETHODIMP Activator::SetGUID(REFGUID key, REFGUID itemValue) { return store_->SetGUID(key, itemValue); }
STDMETHODIMP Activator::SetString(REFGUID key, LPCWSTR itemValue) { return store_->SetString(key, itemValue); }
STDMETHODIMP Activator::SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) { return store_->SetBlob(key, buffer, size); }
STDMETHODIMP Activator::SetUnknown(REFGUID key, IUnknown* unknown) { return store_->SetUnknown(key, unknown); }
STDMETHODIMP Activator::LockStore() { return store_->LockStore(); }
STDMETHODIMP Activator::UnlockStore() { return store_->UnlockStore(); }
STDMETHODIMP Activator::GetCount(UINT32* count) { return store_->GetCount(count); }
STDMETHODIMP Activator::GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* itemValue) {
    return store_->GetItemByIndex(index, key, itemValue);
}
STDMETHODIMP Activator::CopyAllItems(IMFAttributes* destination) { return store_->CopyAllItems(destination); }

}  // namespace mwb::vcam

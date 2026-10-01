// IMFActivate handed to Frame Server by our class factory. Frame Server (and Frame Server Monitor,
// and sometimes client processes) create it freely, so construction is cheap: the media source is
// only built in ActivateObject().
#pragma once

#include "Framework.h"
#include "MediaSource.h"
#include "ModuleLifetime.h"

namespace mwb::vcam {

class Activator final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::ChainInterfaces<IMFActivate, IMFAttributes>> {
public:
    Activator() = default;

    HRESULT RuntimeClassInitialize() noexcept;

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** object) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    // IMFAttributes — delegated to an internal attribute store.
    STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* itemValue) override;
    STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override;
    STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT itemValue, BOOL* result) override;
    STDMETHODIMP Compare(IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE matchType, BOOL* result) override;
    STDMETHODIMP GetUINT32(REFGUID key, UINT32* itemValue) override;
    STDMETHODIMP GetUINT64(REFGUID key, UINT64* itemValue) override;
    STDMETHODIMP GetDouble(REFGUID key, double* itemValue) override;
    STDMETHODIMP GetGUID(REFGUID key, GUID* itemValue) override;
    STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override;
    STDMETHODIMP GetString(REFGUID key, LPWSTR itemValue, UINT32 size, UINT32* length) override;
    STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* itemValue, UINT32* length) override;
    STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override;
    STDMETHODIMP GetBlob(REFGUID key, UINT8* buffer, UINT32 bufferSize, UINT32* blobSize) override;
    STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size) override;
    STDMETHODIMP GetUnknown(REFGUID key, REFIID riid, LPVOID* object) override;
    STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT itemValue) override;
    STDMETHODIMP DeleteItem(REFGUID key) override;
    STDMETHODIMP DeleteAllItems() override;
    STDMETHODIMP SetUINT32(REFGUID key, UINT32 itemValue) override;
    STDMETHODIMP SetUINT64(REFGUID key, UINT64 itemValue) override;
    STDMETHODIMP SetDouble(REFGUID key, double itemValue) override;
    STDMETHODIMP SetGUID(REFGUID key, REFGUID itemValue) override;
    STDMETHODIMP SetString(REFGUID key, LPCWSTR itemValue) override;
    STDMETHODIMP SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) override;
    STDMETHODIMP SetUnknown(REFGUID key, IUnknown* unknown) override;
    STDMETHODIMP LockStore() override;
    STDMETHODIMP UnlockStore() override;
    STDMETHODIMP GetCount(UINT32* count) override;
    STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* itemValue) override;
    STDMETHODIMP CopyAllItems(IMFAttributes* destination) override;

private:
    ModuleObjectToken moduleToken_;
    wil::srwlock lock_;  // guards source_
    wil::com_ptr_nothrow<IMFAttributes> store_;
    wil::com_ptr_nothrow<MediaSource> source_;
};

}  // namespace mwb::vcam

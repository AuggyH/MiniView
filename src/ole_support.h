#pragma once
#include <ole2.h>
#include <string>
#include <vector>

namespace mv {

// Simple IDataObject that holds one or more file paths as CF_HDROP.
class FileDataObject : public IDataObject {
public:
    explicit FileDataObject(const std::vector<std::wstring>& paths);
    ~FileDataObject();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IDataObject
    STDMETHODIMP GetData(FORMATETC* fe, STGMEDIUM* sm) override;
    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override;
    STDMETHODIMP QueryGetData(FORMATETC* fe) override;
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override;
    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override;
    STDMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC**) override;
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override;
    STDMETHODIMP DUnadvise(DWORD) override;
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override;

private:
    ULONG   m_ref;
    HGLOBAL m_data = nullptr;
    SIZE_T  m_data_size = 0;
};

// Minimal IDropSource
class SimpleDropSource : public IDropSource {
public:
    SimpleDropSource();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys) override;
    STDMETHODIMP GiveFeedback(DWORD) override;

private:
    ULONG m_ref;
};

} // namespace mv

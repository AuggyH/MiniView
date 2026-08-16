#include "ole_support.h"
#include <shlobj.h>

namespace mv {

FileDataObject::FileDataObject(const std::vector<std::wstring>& paths) : m_ref(1) {
    SIZE_T path_bytes = sizeof(wchar_t);
    for (const auto& path : paths) path_bytes += (path.size() + 1) * sizeof(wchar_t);
    m_data_size = sizeof(DROPFILES) + path_bytes;
    m_data = GlobalAlloc(GMEM_MOVEABLE, m_data_size);
    if (m_data) {
        auto* df = static_cast<DROPFILES*>(GlobalLock(m_data));
        if (!df) {
            GlobalFree(m_data);
            m_data = nullptr;
            m_data_size = 0;
            return;
        }
        ZeroMemory(df, sizeof(*df));
        df->pFiles = sizeof(DROPFILES);
        df->fWide  = TRUE;
        auto* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
        for (const auto& path : paths) {
            wmemcpy(dst, path.c_str(), path.size() + 1);
            dst += path.size() + 1;
        }
        *dst = L'\0';
        GlobalUnlock(m_data);
    }
}

FileDataObject::~FileDataObject() { if (m_data) GlobalFree(m_data); }

STDMETHODIMP FileDataObject::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
        *ppv = static_cast<IDataObject*>(this);
        AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) FileDataObject::AddRef() { return InterlockedIncrement(&m_ref); }

STDMETHODIMP_(ULONG) FileDataObject::Release() {
    ULONG c = InterlockedDecrement(&m_ref);
    if (c == 0) delete this;
    return c;
}

STDMETHODIMP FileDataObject::GetData(FORMATETC* fe, STGMEDIUM* sm) {
    if (!fe || !sm) return E_INVALIDARG;
    if (fe->cfFormat != CF_HDROP || !(fe->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;
    if (!m_data || m_data_size == 0) return E_OUTOFMEMORY;
    ZeroMemory(sm, sizeof(*sm));
    sm->tymed = TYMED_HGLOBAL;
    sm->hGlobal = GlobalAlloc(GMEM_MOVEABLE, m_data_size);
    if (!sm->hGlobal) return E_OUTOFMEMORY;
    void* src = GlobalLock(m_data);
    void* dst = GlobalLock(sm->hGlobal);
    if (!src || !dst) {
        if (dst) GlobalUnlock(sm->hGlobal);
        if (src) GlobalUnlock(m_data);
        GlobalFree(sm->hGlobal);
        ZeroMemory(sm, sizeof(*sm));
        return STG_E_READFAULT;
    }
    memcpy(dst, src, m_data_size);
    GlobalUnlock(sm->hGlobal);
    GlobalUnlock(m_data);
    sm->pUnkForRelease = nullptr;
    return S_OK;
}

STDMETHODIMP FileDataObject::GetDataHere(FORMATETC*, STGMEDIUM*) { return DV_E_FORMATETC; }

STDMETHODIMP FileDataObject::QueryGetData(FORMATETC* fe) {
    if (!fe) return E_INVALIDARG;
    return (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) ? S_OK : DV_E_FORMATETC;
}

STDMETHODIMP FileDataObject::GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) { return DV_E_FORMATETC; }
STDMETHODIMP FileDataObject::SetData(FORMATETC*, STGMEDIUM*, BOOL) { return E_NOTIMPL; }
STDMETHODIMP FileDataObject::EnumFormatEtc(DWORD, IEnumFORMATETC**) { return OLE_S_USEREG; }
STDMETHODIMP FileDataObject::DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) { return OLE_E_ADVISENOTSUPPORTED; }
STDMETHODIMP FileDataObject::DUnadvise(DWORD) { return OLE_E_ADVISENOTSUPPORTED; }
STDMETHODIMP FileDataObject::EnumDAdvise(IEnumSTATDATA**) { return OLE_E_ADVISENOTSUPPORTED; }

SimpleDropSource::SimpleDropSource() : m_ref(1) {}

STDMETHODIMP SimpleDropSource::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
        *ppv = static_cast<IDropSource*>(this);
        AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) SimpleDropSource::AddRef() { return InterlockedIncrement(&m_ref); }

STDMETHODIMP_(ULONG) SimpleDropSource::Release() {
    ULONG c = InterlockedDecrement(&m_ref);
    if (c == 0) delete this;
    return c;
}

STDMETHODIMP SimpleDropSource::QueryContinueDrag(BOOL escape, DWORD keys) {
    if (escape) return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
    return S_OK;
}

STDMETHODIMP SimpleDropSource::GiveFeedback(DWORD) { return DRAGDROP_S_USEDEFAULTCURSORS; }

} // namespace mv

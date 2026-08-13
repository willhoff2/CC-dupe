// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
//
// ATL's COM object plumbing, as much of it as FEBDispatch.h and WebBrowser.h use: the threading
// model tags, CComObjectRootEx/CComCoClass, CComObject, and the COM map macros. Only the embedded
// browser (cut scope, see EABrowserDispatch/BrowserDispatch.h) needs any of it.
//
// The COM map expands to a QueryInterface that always fails: an off-Windows build has no COM
// runtime to hand an interface pointer to, and an implementation that appeared to work would hide
// that. AddRef/Release are honest, since they are just a counter.
#pragma once

#include <atlbase.h>
#include <oaidl.h>

class CComSingleThreadModel
{
public:
	static long Increment(long* p) { return ++*p; }
	static long Decrement(long* p) { return --*p; }
};
typedef CComSingleThreadModel CComMultiThreadModel;
typedef CComSingleThreadModel CComObjectThreadModel;
typedef CComSingleThreadModel CComGlobalsThreadModel;

template <class ThreadModel>
class CComObjectRootEx
{
public:
	CComObjectRootEx() : m_dwRef(0) {}
	typedef ThreadModel _ThreadModel;
	ULONG InternalAddRef() { return (ULONG)ThreadModel::Increment(&m_dwRef); }
	ULONG InternalRelease() { return (ULONG)ThreadModel::Decrement(&m_dwRef); }
	HRESULT FinalConstruct() { return S_OK; }
	void FinalRelease() {}
	long m_dwRef;
};
typedef CComObjectRootEx<CComSingleThreadModel> CComObjectRoot;

template <class T>
class CComCoClass
{
public:
	static const CLSID& GetObjectCLSID();
};

// A concrete instantiation of an ATL COM class. CreateInstance is declared and not defined: the
// class it would instantiate is a COM server, and there is no server off Windows.
template <class Base>
class CComObject : public Base
{
public:
	static HRESULT CreateInstance(CComObject<Base>** pp);
};

#define BEGIN_COM_MAP(x) \
	public: \
		HRESULT _InternalQueryInterface(REFIID, void** ppv) { *ppv = 0; return E_NOINTERFACE; }
#define COM_INTERFACE_ENTRY(x)
#define COM_INTERFACE_ENTRY_IID(iid, x)
#define COM_INTERFACE_ENTRY_AGGREGATE(iid, punk)
#define END_COM_MAP() \
	public: \
		virtual ULONG STDMETHODCALLTYPE AddRef() = 0; \
		virtual ULONG STDMETHODCALLTYPE Release() = 0; \
		virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;

#define DECLARE_REGISTRY_RESOURCEID(x)
#define DECLARE_NOT_AGGREGATABLE(x)
#define DECLARE_PROTECT_FINAL_CONSTRUCT()
#define OBJECT_ENTRY(clsid, class)
#define BEGIN_OBJECT_MAP(x)
#define END_OBJECT_MAP()

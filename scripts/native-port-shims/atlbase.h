// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
#pragma once

// ATL is only ever used for CComPtr/CComBSTR-style RAII in this codebase.
#include <windows.h>

typedef wchar_t* BSTR;
typedef const wchar_t* LPCOLESTR;
typedef wchar_t OLECHAR;

template <class T> class CComPtr
{
public:
	CComPtr() : p(0) {}
	CComPtr(T* ptr) : p(ptr) { if (p) p->AddRef(); }
	~CComPtr() { if (p) p->Release(); }
	T** operator&() { return &p; }
	T* operator->() const { return p; }
	operator T*() const { return p; }
	T* p;
};

class CComBSTR
{
public:
	CComBSTR() : m_str(0) {}
	~CComBSTR() {}
	operator BSTR() const { return m_str; }
	BSTR* operator&() { return &m_str; }
	BSTR m_str;
};

class CComModule
{
public:
	// The real CComModule::Init takes the object map, the instance and an optional library id.
	HRESULT Init(void*, HINSTANCE, const void* = 0) { return S_OK; }
	void Term() {}
};

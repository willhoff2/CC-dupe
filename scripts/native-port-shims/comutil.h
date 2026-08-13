// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
//
// MSVC's C++ COM support library, reduced to the one type this codebase uses: _bstr_t, the RAII
// wrapper around a BSTR. FEBDispatch.h builds one from a char* to pass a module path to
// LoadTypeLib; dx8webbrowser.cpp (renderer, not in the default probe scope) builds several.
//
// The conversion functions are declared, not defined -- SysAllocString and friends are OLE, which
// does not exist here. MinGW builds use Utility/comsupp_compat.h instead, which does implement
// them over the real OLE allocator.
#pragma once

#include <oaidl.h>

namespace _com_util
{
BSTR ConvertStringToBSTR(const char* src);
char* ConvertBSTRToString(BSTR src);
}

class _bstr_t
{
public:
	_bstr_t() : m_str(0) {}
	_bstr_t(const char* src) : m_str(_com_util::ConvertStringToBSTR(src)) {}
	_bstr_t(const wchar_t* src) : m_str(SysAllocString(src)) {}
	_bstr_t(const _bstr_t& other) : m_str(SysAllocString(other.m_str)) {}
	~_bstr_t() { SysFreeString(m_str); }

	_bstr_t& operator=(const _bstr_t& other)
	{
		if (this != &other)
		{
			SysFreeString(m_str);
			m_str = SysAllocString(other.m_str);
		}
		return *this;
	}

	operator BSTR() const { return m_str; }
	operator const wchar_t*() const { return m_str; }
	unsigned int length() const { return SysStringLen(m_str); }

private:
	BSTR m_str;
};

class _variant_t : public VARIANT
{
public:
	_variant_t() { VariantInit(this); }
	~_variant_t() { VariantClear(this); }
};

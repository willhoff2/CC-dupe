// Declaration-only stand-in for scripts/native-port-probe.py. See README.md.
//
// OLE Automation: IDispatch, VARIANT and the type-library interfaces. Used by WWCOMUtil.cpp (the
// Dispatch_GetProperty/PutProperty/InvokeMethod helpers) and, through FEBDispatch.h, by the
// embedded-browser screens. All of it is Windows-only COM; nothing here is implemented, and the
// layouts are only approximately right -- VARIANT in particular is declared with the members this
// codebase touches rather than the full union.
#pragma once

#include <windows.h>

typedef LONG DISPID;
typedef DISPID MEMBERID;
typedef DWORD HREFTYPE;
typedef short VARTYPE;
typedef double DATE;
typedef LONG SCODE;

#ifndef _OLECHAR_DEFINED
#define _OLECHAR_DEFINED
typedef wchar_t OLECHAR;
typedef OLECHAR* BSTR;
typedef OLECHAR* LPOLESTR;
typedef const OLECHAR* LPCOLESTR;
#endif

#define VT_EMPTY      0
#define VT_NULL       1
#define VT_I2         2
#define VT_I4         3
#define VT_R4         4
#define VT_R8         5
#define VT_BSTR       8
#define VT_DISPATCH   9
#define VT_ERROR      10
#define VT_BOOL       11
#define VT_VARIANT    12
#define VT_UNKNOWN    13
#define VT_I1         16
#define VT_UI1        17
#define VT_UI2        18
#define VT_UI4        19
#define VT_INT        22
#define VT_UINT       23
#define VT_BYREF      0x4000

#define DISPATCH_METHOD         0x1
#define DISPATCH_PROPERTYGET    0x2
#define DISPATCH_PROPERTYPUT    0x4
#define DISPATCH_PROPERTYPUTREF 0x8

#define DISPID_VALUE     0
#define DISPID_UNKNOWN   (-1)
#define DISPID_PROPERTYPUT (-3)

#define IID_NULL         (*(const IID*)0)
#define GUID_NULL        (*(const GUID*)0)

struct IDispatch;
struct ITypeInfo;
struct ITypeLib;

typedef struct tagVARIANT {
	VARTYPE vt;
	WORD wReserved1, wReserved2, wReserved3;
	union {
		LONG lVal;
		SHORT iVal;
		FLOAT fltVal;
		DOUBLE dblVal;
		VARIANT_BOOL boolVal;
		SCODE scode;
		BSTR bstrVal;
		IUnknown* punkVal;
		IDispatch* pdispVal;
		BYTE bVal;
		CHAR cVal;
		void* byref;
		LONGLONG llVal;
	};
} VARIANT, VARIANTARG, *LPVARIANT, *LPVARIANTARG;

typedef struct tagDISPPARAMS {
	VARIANTARG* rgvarg;
	DISPID* rgdispidNamedArgs;
	UINT cArgs;
	UINT cNamedArgs;
} DISPPARAMS;

typedef struct tagEXCEPINFO {
	WORD wCode, wReserved;
	BSTR bstrSource, bstrDescription, bstrHelpFile;
	DWORD dwHelpContext;
	PVOID pvReserved;
	HRESULT (*pfnDeferredFillIn)(struct tagEXCEPINFO*);
	SCODE scode;
} EXCEPINFO, *LPEXCEPINFO;

struct IDispatch : public IUnknown {
	virtual HRESULT GetTypeInfoCount(UINT*) = 0;
	virtual HRESULT GetTypeInfo(UINT, LCID, ITypeInfo**) = 0;
	virtual HRESULT GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) = 0;
	virtual HRESULT Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*,
	                       UINT*) = 0;
};
typedef IDispatch* LPDISPATCH;

// Only the members FEBDispatch.h calls; the real interfaces are far wider.
struct ITypeInfo : public IUnknown {
	virtual HRESULT GetTypeAttr(void**) = 0;
	virtual HRESULT GetDocumentation(MEMBERID, BSTR*, BSTR*, DWORD*, BSTR*) = 0;
};

struct ITypeLib : public IUnknown {
	virtual HRESULT GetTypeInfoCount(UINT*) = 0;
	virtual HRESULT GetTypeInfoOfGuid(REFGUID, ITypeInfo**) = 0;
};

extern "C" {
HRESULT LoadTypeLib(LPCOLESTR, ITypeLib**);
HRESULT CreateStdDispatch(IUnknown*, void*, ITypeInfo*, IUnknown**);
void    VariantInit(VARIANTARG*);
HRESULT VariantClear(VARIANTARG*);
HRESULT VariantCopy(VARIANTARG*, const VARIANTARG*);
BSTR    SysAllocString(const OLECHAR*);
BSTR    SysAllocStringLen(const OLECHAR*, UINT);
void    SysFreeString(BSTR);
UINT    SysStringLen(BSTR);
}

#pragma once

#define _CRT_SECURE_NO_DEPRECATE 1
#ifdef _WIN64
#define _WIN32_WINNT _WIN32_WINNT_WIN10
#define WINVER _WIN32_WINNT_VISTA
#else
#define _WIN32_WINNT _WIN32_WINNT_WIN10
#define WINVER _WIN32_WINNT_WIN10
#endif
#define NTDDI_VERSION NTDDI_WIN10_RS3
#define WIN32_LEAN_AND_MEAN 1
#define UNICODE  1
#define _UNICODE 1

#define NOMINMAX
#include <Windows.h>
#include <Uxtheme.h>
#include <usp10.h>
#include <functional>
#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>
#include "renderer_raii.h"
#include "array.h"
#include <set>
#include "ownedcs.h"
#include "undocAPI.h"
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1_3.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <dwrite_2.h>
#include <dwrite_3.h>
#include <string>
#include <locale>
#include <codecvt>

#define for if(0);else for

#include <tchar.h>
#include <stddef.h>
#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>

#define _CRTDBG_MAP_ALLOC
#include <cstdlib>
#include <malloc.h>
#include <crtdbg.h>

#include <map>
#include <string>
using namespace std;

#define FONT_MAGIC_NUMBER 0xA8

#define ASSERT			_ASSERTE
#define Assert			_ASSERTE
#ifdef _DEBUG
#define new				new(_NORMAL_BLOCK, __FILE__, __LINE__)
#endif

#ifndef NOP_FUNCTION
#if (_MSC_VER >= 1210)
#define NOP_FUNCTION	__noop
#else
#define NOP_FUNCTION	static_cast<void>(0)
#endif	//_MSC_VER
#endif	//!NOP_FUNCTION
#ifndef C_ASSERT
#define C_ASSERT(e)		typedef char __C_ASSERT__[(e)?1:-1]
#endif	//!C_ASSERT
#ifndef FORCEINLINE
#if (_MSC_VER >= 1200)
#define FORCEINLINE		__forceinline
#else
#define FORCEINLINE		__inline
#endif	//_MSC_VER
#endif	//!FORCEINLINE


void Log(char* Msg);
void Log(wchar_t* Msg);


// convert string to wstring
std::wstring to_wide_string(const std::string & input);

// convert wstring to string
std::string to_byte_string(const std::wstring & input);

// convert a utf-16be string back to utf-16le string
std::wstring to_utf16le(const std::wstring& input);

wstring to_lower_case(wstring str);

FORCEINLINE HINSTANCE GetDLLInstance()
{
	extern HINSTANCE g_hinstDLL;
	return g_hinstDLL;
}

//排他制御
class CCriticalSectionLock
{
#define MAX_CRITICAL_COUNT 20
private:
	static renderer_raii::CriticalSection m_cs[MAX_CRITICAL_COUNT];
	friend class CCriticalSectionLockTry;
	int m_index;
public:
	enum {
		CS_LIBRARY,
		CS_CACHEDFONT,
		CS_FONTENG,
		CS_SETTING,
		CS_MAIN,
		CS_FONTCACHE,
		CS_MANAGER,
		CS_CREATEFONT,
		CS_FONTLINK,
		CS_FONTMAP,
		CS_OWNEDCS,
		CS_VIRTMEM,
		CS_DWRITE,
		CS_DCRELATION,
	};
	explicit CCriticalSectionLock(int index=CS_LIBRARY):
	  m_index(index)
	{
		::EnterCriticalSection(m_cs[index].get());
	}
	~CCriticalSectionLock()
	{
		::LeaveCriticalSection(m_cs[m_index].get());
	}
	static void Init()
	{
		for (int i=0;i<MAX_CRITICAL_COUNT;i++)
			m_cs[i].initialize();
	}
	static void Term()
	{
		for (int i=0;i<MAX_CRITICAL_COUNT;i++)
			m_cs[i].reset();
	}
};

#ifdef _DEBUG
#define TRACE	_Trace
#include <stdarg.h>	//va_list
#include <stdio.h>	//_vsnwprintf

static void _Trace(LPCTSTR pszFormat, ...)
{
	CCriticalSectionLock __lock;
	va_list argptr;
	va_start(argptr, pszFormat);
	//w(v)sprintfは1024文字以上返してこない
	TCHAR szBuffer[10240];
	wvsprintf(szBuffer, pszFormat, argptr);
	va_end(argptr);

	OutputDebugString(szBuffer);
}
#else	//!_DEBUG
#define TRACE	NOP_FUNCTION
#endif	//_DEBUG

//TRACEマクロ
//使用例: TRACE(_T("cx: %d\n"), cx);
#ifdef USE_TRACE
#define TRACE2	_Trace2
#define TRACE2_STR	_Trace2_Str
#define TRACE2_BIN	_Trace2_Bin
#include <stdarg.h>	//va_list
#include <stdio.h>	//_vsnwprintf

static void _Trace2(LPCTSTR pszFormat, ...)
{
	CCriticalSectionLock __lock;
	va_list argptr;
	va_start(argptr, pszFormat);
	//w(v)sprintfは1024文字以上返してこない
	TCHAR szBuffer[1024];
	wvsprintf(szBuffer, pszFormat, argptr);
	OutputDebugString(szBuffer);
}

static void _Trace2_Bin(LPWSTR func, int line, LPVOID lpString, UINT cbString)
{
	const BYTE* srcp = static_cast<const BYTE*>(lpString);
	WCHAR buf[0x1000];
	LPWSTR p = buf;
	for (UINT i = 0; i < 32 && i < cbString; ++i) {
		wsprintf(p, L"%02x ", srcp[i]);
		p += lstrlen(p);
	}
	*p = 0;
	TRACE(_T("%s %d: %d %08x %s\n"), func, line, cbString, lpString, buf);
}

static void _Trace2_Str(LPWSTR func, int line, LPCWSTR lpString, UINT cbString)
{
	WCHAR buf[0x1000];
	UINT len = Min(cbString, countof(buf) - 1);
	lstrcpyn(buf, lpString, len + 1);
	buf[countof(buf) - 1] = 0;
	TRACE(_T("%s %d: %d %s\n"), func, line, cbString, buf);
	LPWSTR p = buf;
	for (UINT i = 0; i < 32 && i < cbString; ++i) {
		wsprintf(p, L"%04x ", lpString[i]);
		p += lstrlen(p);
	}
	TRACE(_T("%s %d: %d %08x %s\n"), func, line, cbString, lpString, buf);
}

#else	//!USE_TRACE
#define TRACE2	NOP_FUNCTION
#define TRACE2_STR	NOP_FUNCTION
#define TRACE2_BIN	NOP_FUNCTION
#endif	//USE_TRACE


class OwnedCriticalSectionResource
{
public:
	OwnedCriticalSectionResource() noexcept : initialized_(false) {}
	~OwnedCriticalSectionResource() noexcept { reset(); }

	OwnedCriticalSectionResource(const OwnedCriticalSectionResource&) = delete;
	OwnedCriticalSectionResource& operator=(const OwnedCriticalSectionResource&) = delete;
	OwnedCriticalSectionResource(OwnedCriticalSectionResource&&) = delete;
	OwnedCriticalSectionResource& operator=(OwnedCriticalSectionResource&&) = delete;

	void initialize()
	{
		if (!initialized_) {
			InitializeOwnedCritialSection(&value_);
			initialized_ = true;
		}
	}

	void reset() noexcept
	{
		if (initialized_) {
			DeleteOwnedCritialSection(&value_);
			initialized_ = false;
		}
	}

	POWNED_CRITIAL_SECTION get() noexcept { return &value_; }

private:
	OWNED_CRITIAL_SECTION value_{};
	bool initialized_;
};

class COwnedCriticalSectionLock
{
private:
	static OwnedCriticalSectionResource m_cs[2];
	WORD FOwner;
	int m_index;
public:
	enum {
		OCS_FREETYPE,
		OCS_DC
	};
	COwnedCriticalSectionLock():FOwner(0), m_index(OCS_FREETYPE)
	{
		EnterOwnedCritialSection(m_cs[m_index].get(), FOwner);
	}
	explicit COwnedCriticalSectionLock(WORD Owner, int index=OCS_FREETYPE):FOwner(Owner), m_index(index)
	{
		EnterOwnedCritialSection(m_cs[m_index].get(), Owner);
	}
	~COwnedCriticalSectionLock()
	{
		LeaveOwnedCritialSection(m_cs[m_index].get(), FOwner);
	}
	static void Init()
	{
		for (int i=0;i<2;i++)
		{
			m_cs[i].initialize();
		}
	}
	static void Term()
	{
		for (int i=0;i<2;i++)
		{
			m_cs[i].reset();
		}
	}
};

class CThreadCounter
{
private:
	static LONG interlock;
public:
	CThreadCounter() noexcept
	{
		InterlockedIncrement(&interlock);
	}
	~CThreadCounter() noexcept
	{
		InterlockedDecrement(&interlock);
	}
	CThreadCounter(const CThreadCounter&) = delete;
	CThreadCounter& operator=(const CThreadCounter&) = delete;
	static void Init()
	{
		InterlockedExchange(&interlock, 0);
	}
	static LONG Count() noexcept
	{
		return InterlockedCompareExchange(&interlock, 0, 0);
	}
};

class CCriticalSectionLockTry
{
public:
	static BOOL TryEnter(int index=CCriticalSectionLock::CS_LIBRARY)
	{
		return ::TryEnterCriticalSection(CCriticalSectionLock::m_cs[index].get());
	}
	static int CritalCount(int index=CCriticalSectionLock::CS_LIBRARY)
	{
		return CCriticalSectionLock::m_cs[index].get()->RecursionCount;
	}
	static void Leave(int index=CCriticalSectionLock::CS_LIBRARY)
	{
		::LeaveCriticalSection(CCriticalSectionLock::m_cs[index].get());
	}
};

// 使用後はfreeで開放する事
LPWSTR _StrDupExAtoW(LPCSTR pszMB, int cchMB, LPWSTR pszStack, int cchStack, int* pcchWC, int nACP = CP_ACP);
static inline LPWSTR _StrDupAtoW(LPCSTR pszMB, int cchMB = -1, int* pcchWC = nullptr)
{
	return _StrDupExAtoW(pszMB, cchMB, nullptr, 0, pcchWC);
}

// useful macros
#define NOCOPY(T)					T(const T&); T& operator=(const T&)
#define countof(array)				(sizeof(array)/sizeof(array[0]))
#define sizeof_struct(s, m)			(static_cast<int>(offsetof(s, m)) + sizeof((static_cast<s*>(nullptr))->m))
#ifdef _DEBUG
#define Verify(expr)				_ASSERTE(expr)
#else
#define Verify(expr)				(expr)
#endif

template<typename T> FORCEINLINE T Min(T x, T y) { return (x < y) ? x : y; }
template<typename T> FORCEINLINE T Max(T x, T y) { return (y < x) ? x : y; }
template<typename T> FORCEINLINE T Bound(T x, T m, T M) { return (x < m) ? m : ((x > M) ? M : x); }
template<typename T> FORCEINLINE int Sgn(T x, T y) { return (x > y) ? 1 : ((x < y) ? -1 : 0); }


//型チェック機能つきDeleteXXX/SelectXXX
//SelectObject/DeleteObjectは使用できなくなる

#ifdef _DEBUG
#undef DeletePen
#undef DeleteBrush
#undef DeleteRgn
#undef DeleteFont
#undef DeleteBitmap
#undef SelectPen
#undef SelectBrush
#undef SelectRgn
#undef SelectFont
#undef SelectBitmap

#define _IsValidPen(hPen)		\
	(hPen == nullptr || ::GetObjectType(hPen) == OBJ_PEN || ::GetObjectType(hPen) == OBJ_EXTPEN)
#define _IsValidBrush(hBrush)	\
	(hBrush == nullptr || ::GetObjectType(hBrush) == OBJ_BRUSH)
#define _IsValidRgn(hRgn)		\
	(hRgn == nullptr || ::GetObjectType(hRgn) == OBJ_REGION)
#define _IsValidFont(hFont)		\
	(hFont == nullptr || ::GetObjectType(hFont) == OBJ_FONT)
#define _IsValidBitmap(hBitmap)	\
	(hBitmap == nullptr || ::GetObjectType(hBitmap) == OBJ_BITMAP)

#define DEFINE_DELETE_FUNCTION(type, name) \
	FORCEINLINE BOOL WINAPI Delete##name(type h##name) \
	{ \
		_ASSERTE(_IsValid##name(h##name)); \
		return ::DeleteObject(h##name); \
	}

#define DEFINE_SELECT_FUNCTION(type, name) \
	FORCEINLINE type WINAPI Select##name(HDC hDC, type h##name) \
	{ \
		_ASSERTE(hDC != nullptr); \
		if (!_IsValid##name(h##name)) { \
		TRACE(_T("Select object %x for DC %x"), static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(h##name)), static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(hDC))); \
			}; \
		return reinterpret_cast<type>(::SelectObject(hDC, h##name)); \
	}

DEFINE_DELETE_FUNCTION(HPEN,	Pen)
DEFINE_DELETE_FUNCTION(HBRUSH,	Brush)
DEFINE_DELETE_FUNCTION(HRGN,	Rgn)
DEFINE_DELETE_FUNCTION(HFONT,	Font)
DEFINE_DELETE_FUNCTION(HBITMAP,	Bitmap)

DEFINE_SELECT_FUNCTION(HPEN,	Pen)
DEFINE_SELECT_FUNCTION(HBRUSH,	Brush)
DEFINE_SELECT_FUNCTION(HRGN,	Rgn)
DEFINE_SELECT_FUNCTION(HFONT,	Font)
DEFINE_SELECT_FUNCTION(HBITMAP,	Bitmap)

#undef _IsValidPen
#undef _IsValidBrush
#undef _IsValidRgn
#undef _IsValidFont
#undef _IsValidBitmap
#undef DEFINE_DELETE_FUNCTION
#undef DEFINE_SELECT_FUNCTION

#if (_MSC_VER >= 1300)
#pragma deprecated(DeleteObject)
#pragma deprecated(SelectObject)
#else	//_MSC_VER < 1300
#undef DeleteObject
#define DeleteObject		DeleteObject_instead_use_DeleteXXX
#undef SelectObject
#define SelectObject		SelectObject_instead_use_DeleteXXX
#endif	//_MSC_VER

#else	//!_DEBUG
#ifndef _INC_WINDOWSX
#define DeletePen			::DeleteObject
#define DeleteBrush			::DeleteObject
#define DeleteRgn			::DeleteObject
#define DeleteFont			::DeleteObject
#define DeleteBitmap		::DeleteObject

#define SelectPen(d,o)		reinterpret_cast<HPEN>(::SelectObject(d,o))
#define SelectBrush(d,o)	reinterpret_cast<HBRUSH>(::SelectObject(d,o))
#define SelectRgn(d,o)		reinterpret_cast<HRGN>(::SelectObject(d,o))
#define SelectFont(d,o)		reinterpret_cast<HFONT>(::SelectObject(d,o))
#define SelectBitmap(d,o)	reinterpret_cast<HBITMAP>(::SelectObject(d,o))
#endif	//!_INC_WINDOWSX
#endif	//_DEBUG


//TRACEマクロ
//使用例: TRACE(_T("cx: %d\n"), cx);
#ifndef _WIN64
#ifdef _DEBUG
FORCEINLINE static __int64 GetClockCount()
{
	LARGE_INTEGER cycles;
	__asm {
		rdtsc
		mov cycles.LowPart,  eax
		mov cycles.HighPart, edx
	}
	return cycles.QuadPart;
}

//使用例
//{
//   CDebugElapsedCounter _cntr("hogehoge");
//     : (適当な処理)
//}
//出力例: "hogehoge: 10000 clocks"
class CDebugElapsedCounter
{
private:
	__int64 m_ilClk;
	LPCSTR m_pszName;
public:
	explicit CDebugElapsedCounter(LPCSTR psz)
		: m_ilClk(GetClockCount())
		, m_pszName(psz)
	{
	}
	~CDebugElapsedCounter()
	{
		TRACE(_T("%hs: %u clocks\n"), m_pszName, static_cast<DWORD>(GetClockCount() - m_ilClk));
	}
};
#else
class CDebugElapsedCounter
{
public:
	explicit CDebugElapsedCounter(LPCSTR psz)
	{
	}
};
#endif
#endif


void StartDirectWriteLifecycle();
bool HookD2D1();
void HookGdiplus();
void ChangeFileName(LPWSTR lpSrc, int nSize, LPCWSTR lpNewFileName);
std::wstring MakeUniqueFontName(const std::wstring& strFullName, const std::wstring& strFamilyName, const std::wstring& strStyleName);
std::string WstringToString(const std::wstring& str);

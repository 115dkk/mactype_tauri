// dll injection
#define _CRT_SECURE_NO_DEPRECATE 1
#define WINVER 0x500
#define _WIN32_WINNT 0x500
#define _WIN32_IE 0x601
#define WIN32_LEAN_AND_MEAN 1
#define UNICODE  1
#define _UNICODE 1
#include <Windows.h>
#include <ShellApi.h>
#include <ComDef.h>
#include <ShlObj.h>
#include <ShLwApi.h>
#include <tchar.h>
#include "renderer/array.h"
#include <strsafe.h>

// _vsnwprintf用
#include <wchar.h>		
#include <stdarg.h>

#define _CRTDBG_MAP_ALLOC
#include <cstdlib>
#include <malloc.h>
#include <crtdbg.h>

#define for if(0);else for
#ifndef _countof
#define _countof(array)		(sizeof(array) / sizeof((array)[0]))
#endif

#pragma comment(linker, "/subsystem:windows,5.0")
#pragma comment(lib, "Kernel32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "ShLwApi.lib")
#pragma comment(lib, "Ole32.lib")

#define IDS_USAGE		101
#define IDS_DLL			102
#define IDC_EXEC		103

static void showmsg(LPCSTR msg) {
	MessageBoxA(NULL, msg, "MacType ERROR", MB_OK | MB_ICONSTOP);
}

static void errmsg(UINT id, DWORD code)
{
	char  buffer[512];
	char  format[128];
	LoadStringA(GetModuleHandleA(NULL), id, format, 128);
	wnsprintfA(buffer, 512, format, code);
	showmsg(buffer);
}

inline HRESULT HresultFromLastError()
{
	DWORD dwErr = GetLastError();
	return HRESULT_FROM_WIN32(dwErr);
}


#include "renderer/detours.h"
#ifdef _M_IX86
#pragma comment (lib, "detours.lib")
const auto MacTypeDll = L"MacType.dll";
const auto MacTypeDllA = "MacType.dll";
#else
#pragma comment (lib, "detours64.lib")
const auto MacTypeDll = L"MacType64.dll";
const auto MacTypeDllA = "MacType64.dll";
#endif


HINSTANCE hinstDLL;

#include <stddef.h>
#define GetDLLInstance()	(hinstDLL)

#define _GDIPP_EXE
#define _GDIPP_RUN_CPP

//#define OLD_PSDK

#ifdef OLD_PSDK
extern "C" {
	HRESULT WINAPI _SHILCreateFromPath(LPCWSTR pszPath, LPITEMIDLIST* ppidl, DWORD* rgflnOut)
	{
		if (!pszPath || !ppidl) {
			return E_INVALIDARG;
		}

		LPSHELLFOLDER psf;
		HRESULT hr = ::SHGetDesktopFolder(&psf);
		if (hr != NOERROR) {
			return hr;
		}

		ULONG chEaten;
		LPOLESTR lpszDisplayName = ::StrDupW(pszPath);
		hr = psf->ParseDisplayName(NULL, NULL, lpszDisplayName, &chEaten, ppidl, rgflnOut);
		::LocalFree(lpszDisplayName);
		psf->Release();
		return hr;
	}

	void WINAPI _SHFree(void* pv)
	{
		if (!pv) {
			return;
		}

		LPMALLOC pMalloc = NULL;
		if (::SHGetMalloc(&pMalloc) == NOERROR) {
			pMalloc->Free(pv);
			pMalloc->Release();
		}
	}
}
#else
#define _SHILCreateFromPath	SHILCreateFromPath
#define _SHFree				SHFree
#endif


bool isX64PE(const TCHAR* file_path) {
	HANDLE hFile = CreateFile(file_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		showmsg("Error opening file");
		return false;
	}

	IMAGE_DOS_HEADER dosHeader;
	DWORD bytesRead;
	if (!ReadFile(hFile, &dosHeader, sizeof(IMAGE_DOS_HEADER), &bytesRead, NULL)) {
		showmsg("Error reading file");
		CloseHandle(hFile);
		return false;
	}

	// Check if it's a PE file
	if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
		showmsg("Not a PE file");
		CloseHandle(hFile);
		return false;
	}

	IMAGE_NT_HEADERS ntHeaders;
	// Seek to the PE header offset
	SetFilePointer(hFile, dosHeader.e_lfanew, NULL, FILE_BEGIN);
	if (!ReadFile(hFile, &ntHeaders, sizeof(IMAGE_NT_HEADERS), &bytesRead, NULL)) {
		showmsg("Error reading PE header");
		CloseHandle(hFile);
		return false;
	}

	if (ntHeaders.FileHeader.Machine == IMAGE_FILE_MACHINE_I386) {
		CloseHandle(hFile);
		return false;
	}
	else if (ntHeaders.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) {
		CloseHandle(hFile);
		return true;
	}
	else {
		CloseHandle(hFile);
		return false;
	}
}


// １つ目の引数だけファイルとして扱い、実行する。
//
// コマンドは こんな感じで連結されます。
//  exe linkpath linkarg cmdarg2 cmdarg3 cmdarg4 ...
//
static HRESULT HookAndExecute(int show)
{
	int     argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) {
		return HresultFromLastError();
	}
	if (argc <= 1) {
		char buffer[256];
		LoadStringA(GetModuleHandleA(NULL), IDS_USAGE, buffer, 256);
		MessageBoxA(NULL,
			buffer
			, "MacType", MB_OK | MB_ICONINFORMATION);
		LocalFree(argv);
		return S_OK;
	}


	int i;
	size_t length = 1;
	for (i = 1; i < argc; i++) {
		length += wcslen(argv[i]) + 3;
	}

	LPWSTR cmdline = (WCHAR*)calloc(sizeof(WCHAR), length);
	if (!cmdline) {
		LocalFree(argv);
		return E_OUTOFMEMORY;
	}

	LPWSTR p = cmdline;
	*p = L'\0';
	for (i = 1; i < argc; i++) {
		const bool dq = !!wcschr(argv[i], L' ');
		if (dq) {
			*p++ = '"';
			length--;
		}
		StringCchCopyExW(p, length, argv[i], &p, &length, STRSAFE_NO_TRUNCATION);
		if (dq) {
			*p++ = '"';
			length--;
		}
		*p++ = L' ';
		length--;
	}

	*CharPrevW(cmdline, p) = L'\0';

// now we got the full cmdline for external exetuble. let's check if we can hook into it
#ifdef _M_IX86
	if (isX64PE(argv[1])) {
		ShellExecute(NULL, NULL, L"macloader64.exe", cmdline, NULL, SW_SHOW);
		return S_OK;
	}
#else
	if (!isX64PE(argv[1])) {
		ShellExecute(NULL, NULL, L"macloader.exe", cmdline, NULL, SW_SHOW);
		return S_OK;
	}
#endif

	WCHAR file[MAX_PATH], dir[MAX_PATH];
	GetCurrentDirectoryW(_countof(dir), dir);
	StringCchCopyW(file, _countof(file), argv[1]);
	if (PathIsRelativeW(file)) {
		PathCombineW(file, dir, file);
	}
	else {
		WCHAR gdippDir[MAX_PATH];
		GetModuleFileNameW(NULL, gdippDir, _countof(gdippDir));
		PathRemoveFileSpec(gdippDir);

		// カレントディレクトリがgdi++.exeの置かれているディレクトリと同じだったら、
		// 起動しようとしているEXEのフルパスから抜き出したディレクトリ名をカレント
		// ディレクトリとして起動する。(カレントディレクトリがEXEと同じ場所である
		// 前提で作られているアプリ対策)
		if (wcscmp(dir, gdippDir) == 0) {
			StringCchCopyW(dir, _countof(dir), argv[1]);
			PathRemoveFileSpec(dir);
		}
	}

#ifdef _DEBUG
	if ((GetAsyncKeyState(VK_CONTROL) & 0x8000)
		&& MessageBoxW(NULL, cmdline, NULL, MB_YESNO) != IDYES) {
		free(cmdline);
		return NOERROR;
	}
#endif


	PROCESS_INFORMATION processInfo;
	STARTUPINFO startupInfo = { 0 };
	startupInfo.cb = sizeof(startupInfo);

	// get current directory and append mactype dll 
	char path[MAX_PATH] = { 0 };
	if (GetModuleFileNameA(NULL, path, _countof(path))) {
		PathRemoveFileSpecA(path);
		strcat(path, "\\");
	}
	strcat(path, MacTypeDllA);

	auto ret = DetourCreateProcessWithDllEx(NULL, cmdline, NULL, NULL, false, 0, NULL, dir, &startupInfo, &processInfo, path, NULL);

	free(cmdline);
	LocalFree(argv);
	argv = NULL;
	return ret ? S_OK : E_ACCESSDENIED;
}

int WINAPI wWinMain(HINSTANCE ins, HINSTANCE prev, LPWSTR cmd, int show)
{
	_CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_LEAK_CHECK_DF);
	OleInitialize(NULL);

	WCHAR path[MAX_PATH];
	if (GetModuleFileNameW(NULL, path, _countof(path))) {
		PathRemoveFileSpec(path);
		wcscat(path, L"\\");
		wcscat(path, MacTypeDll);
		//DONT_RESOLVE_DLL_REFERENCESを指定すると依存関係の解決や
		//DllMainの呼び出しが行われない
		hinstDLL = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
	}
	if (!hinstDLL) {
		errmsg(IDS_DLL, HresultFromLastError());
	}
	else {
		PathRemoveFileSpecW(path);
		SetCurrentDirectoryW(path);

		HRESULT hr = HookAndExecute(show);
		if (hr != S_OK) {
			errmsg(IDC_EXEC, hr);
		}
	}

	OleUninitialize();
	return 0;
}

//EOF

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
#include "renderer/renderer_raii.h"
#include <strsafe.h>

#include <string>

// _vsnwprintf用
#include <wchar.h>		
#include <stdarg.h>

#define _CRTDBG_MAP_ALLOC
#include <cstdlib>
#include <malloc.h>
#include <crtdbg.h>

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
	MessageBoxA(nullptr, msg, "MacType ERROR", MB_OK | MB_ICONSTOP);
}

static void errmsg(UINT id, DWORD code)
{
	char  buffer[512];
	char  format[128];
	LoadStringA(GetModuleHandleA(nullptr), id, format, 128);
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

#define _SHILCreateFromPath	SHILCreateFromPath
#define _SHFree				SHFree


bool isX64PE(const TCHAR* file_path) {
	auto hFile = renderer_raii::AdoptHandle(CreateFile(
		file_path, GENERIC_READ, 0, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!hFile) {
		showmsg("Error opening file");
		return false;
	}

	IMAGE_DOS_HEADER dosHeader;
	DWORD bytesRead;
	if (!ReadFile(hFile.get(), &dosHeader, sizeof(IMAGE_DOS_HEADER), &bytesRead, nullptr)) {
		showmsg("Error reading file");
		return false;
	}

	// Check if it's a PE file
	if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
		showmsg("Not a PE file");
		return false;
	}

	IMAGE_NT_HEADERS ntHeaders;
	// Seek to the PE header offset
	SetFilePointer(hFile.get(), dosHeader.e_lfanew, nullptr, FILE_BEGIN);
	if (!ReadFile(hFile.get(), &ntHeaders, sizeof(IMAGE_NT_HEADERS), &bytesRead, nullptr)) {
		showmsg("Error reading PE header");
		return false;
	}

	if (ntHeaders.FileHeader.Machine == IMAGE_FILE_MACHINE_I386) {
		return false;
	}
	else if (ntHeaders.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) {
		return true;
	}
	else {
		return false;
	}
}

static HRESULT BuildMacTypeDllPath(std::string& path) noexcept
{
	char modulePath[MAX_PATH] = {};
	SetLastError(ERROR_SUCCESS);
	DWORD const length = GetModuleFileNameA(
		nullptr, modulePath, static_cast<DWORD>(_countof(modulePath)));
	if (length == 0) {
		DWORD const error = GetLastError();
		return HRESULT_FROM_WIN32(
			error == ERROR_SUCCESS ? ERROR_PATH_NOT_FOUND : error);
	}
	if (length >= static_cast<DWORD>(_countof(modulePath)))
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	if (!PathRemoveFileSpecA(modulePath))
		return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);

	try {
		path.assign(modulePath);
		path.append("\\");
		path.append(MacTypeDllA);
	}
	catch (...) {
		path.clear();
		return E_OUTOFMEMORY;
	}
	return S_OK;
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
		LoadStringA(GetModuleHandleA(nullptr), IDS_USAGE, buffer, 256);
		MessageBoxA(nullptr,
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

	LPWSTR cmdline = static_cast<WCHAR*>(calloc(sizeof(WCHAR), length));
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
		ShellExecute(nullptr, nullptr, L"macloader64.exe", cmdline, nullptr, SW_SHOW);
		return S_OK;
	}
#else
	if (!isX64PE(argv[1])) {
		ShellExecute(nullptr, nullptr, L"macloader.exe", cmdline, nullptr, SW_SHOW);
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
		GetModuleFileNameW(nullptr, gdippDir, _countof(gdippDir));
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
		&& MessageBoxW(nullptr, cmdline, nullptr, MB_YESNO) != IDYES) {
		free(cmdline);
		return NOERROR;
	}
#endif


	PROCESS_INFORMATION processInfo;
	STARTUPINFO startupInfo = { 0 };
	startupInfo.cb = sizeof(startupInfo);

	// get current directory and append mactype dll
	std::string path;
	HRESULT const pathResult = BuildMacTypeDllPath(path);
	if (FAILED(pathResult)) {
		free(cmdline);
		LocalFree(argv);
		return pathResult;
	}

	auto ret = DetourCreateProcessWithDllEx(nullptr, cmdline, nullptr, nullptr, false, 0, nullptr, dir, &startupInfo, &processInfo, path.c_str(), nullptr);

	free(cmdline);
	LocalFree(argv);
	argv = nullptr;
	return ret ? S_OK : E_ACCESSDENIED;
}

int WINAPI wWinMain(HINSTANCE ins, HINSTANCE prev, LPWSTR cmd, int show)
{
	_CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_LEAK_CHECK_DF);
	OleInitialize(nullptr);

	WCHAR path[MAX_PATH];
	if (GetModuleFileNameW(nullptr, path, _countof(path))) {
		PathRemoveFileSpec(path);
		wcscat(path, L"\\");
		wcscat(path, MacTypeDll);
		//DONT_RESOLVE_DLL_REFERENCESを指定すると依存関係の解決や
		//DllMainの呼び出しが行われない
		hinstDLL = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
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

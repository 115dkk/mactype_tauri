#ifndef _GDIPP_EXE
#include "settings.h"
#include "override.h"
#include <tlhelp32.h>
#include <shlwapi.h>	//DLLVERSIONINFO
#include "undocAPI.h"
#include <windows.h>
#include <dwrite_1.h>
#include <dwrite_2.h>
#include <dwrite_3.h>
#include <locale>
#include "wow64ext.h"
#include <VersionHelpers.h>
#include "crc32.h"

// win2k以降
//#pragma comment(linker, "/subsystem:windows,5.0")
#ifndef _WIN64
#ifdef DEBUG
#pragma comment(lib, "wow64ext_dbg.lib")
#else
#pragma comment(lib, "wow64ext.lib")
#endif
#endif

EXTERN_C LRESULT CALLBACK GetMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
	//何もしない
	return CallNextHookEx(NULL, code, wParam, lParam);
}

EXTERN_C HRESULT WINAPI GdippDllGetVersion(DLLVERSIONINFO* pdvi)
{
	if (!pdvi || pdvi->cbSize < sizeof(DLLVERSIONINFO)) {
		return E_INVALIDARG;
	}

	const UINT cbSize = pdvi->cbSize;
	ZeroMemory(pdvi, cbSize);
	pdvi->cbSize = cbSize;

	HRSRC hRsrc = FindResource(GetDLLInstance(), MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
	if (!hRsrc) {
		return E_FAIL;
	}

	HGLOBAL hGlobal = LoadResource(GetDLLInstance(), hRsrc);
	if (!hGlobal) {
		return E_FAIL;
	}

	const WORD* lpwPtr = (const WORD*)LockResource(hGlobal);
	if (lpwPtr[1] != sizeof(VS_FIXEDFILEINFO)) {
		return E_FAIL;
	}

	const VS_FIXEDFILEINFO* pvffi = (const VS_FIXEDFILEINFO*)(lpwPtr + 20);
	if (pvffi->dwSignature != VS_FFI_SIGNATURE ||
			pvffi->dwStrucVersion != VS_FFI_STRUCVERSION) {
		return E_FAIL;
	}

	//8.0.2006.1027
	// -> Major: 8, Minor: 2006, Build: 1027
	pdvi->dwMajorVersion	= HIWORD(pvffi->dwFileVersionMS);
	pdvi->dwMinorVersion	= LOWORD(pvffi->dwFileVersionMS) * 10 + HIWORD(pvffi->dwFileVersionLS);
	pdvi->dwBuildNumber		= LOWORD(pvffi->dwFileVersionLS);
	pdvi->dwPlatformID		= DLLVER_PLATFORM_NT;

	if (pdvi->cbSize < sizeof(DLLVERSIONINFO2)) {
		return S_OK;
	}

	DLLVERSIONINFO2* pdvi2 = (DLLVERSIONINFO2*)pdvi;
	pdvi2->ullVersion		= MAKEDLLVERULL(pdvi->dwMajorVersion, pdvi->dwMinorVersion, pdvi->dwBuildNumber, 2);
	return S_OK;
}

#endif	//!_GDIPP_EXE

extern LONG interlock;
extern LONG g_bHookEnabled;
#include "gdiPlusFlat2.h"

#ifdef USE_DETOURS
//detours
#include "detours.h"
//
#define HOOK_MANUALLY(rettype, name, argtype, arglist) ;
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	DetourDetach(&(PVOID&)ORIG_##name, IMPL_##name);
LONG hook_term()
{
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());

#include "hooklist.h"

	LONG error = DetourTransactionCommit();

	if (error != NOERROR) {
		TRACE(_T("hook_term error: %#x\n"), error);
	}
	return error;
}
#undef HOOK_DEFINE
#undef HOOK_MANUALLY

#else
#include "easyhook.h"
#define HOOK_MANUALLY(rettype, name, argtype, arglist) ;
#define HOOK_DEFINE(rettype, name, argtype, arglist) \
	ORIG_##name = name;
#pragma optimize("s", on)
static LONG hook_term()
{
#include "hooklist.h"
	LhUninstallAllHooks();
	return LhWaitForPendingRemovals();
}
#pragma optimize("", on)
#undef HOOK_DEFINE
#undef HOOK_MANUALLY
#endif

HMODULE GetSelfModuleHandle()
{
	MEMORY_BASIC_INFORMATION mbi;

	return ((::VirtualQuery(GetSelfModuleHandle, &mbi, sizeof(mbi)) != 0) 
		? (HMODULE) mbi.AllocationBase : NULL);
}

EXTERN_C void WINAPI CreateControlCenter(IControlCenter** ret)
{
	*ret = (IControlCenter*)new CControlCenter;
}

EXTERN_C void WINAPI ReloadConfig()
{
	CControlCenter::ReloadConfig();
}

extern HINSTANCE g_dllInstance;
EXTERN_C void SafeUnload()
{
	static BOOL bInited = false;
	if (bInited)
		return;	//防重入
	bInited = true;
	while (CThreadCounter::Count())
		Sleep(0);
	CCriticalSectionLock * lock = new CCriticalSectionLock;
	BOOL last;
	if (last=InterlockedExchange(&g_bHookEnabled, FALSE)) {
		if (hook_term()!=NOERROR)
		{
			InterlockedExchange(&g_bHookEnabled, last);
			bInited = false;
			delete lock;
			ExitThread(ERROR_ACCESS_DENIED);
		}
	}
	delete lock;
	while (CThreadCounter::Count())
		Sleep(10);
	Sleep(0);
	do 
	{
		Sleep(10);
	} while (CThreadCounter::Count());	//double check for xp
		
	bInited = false; 
	FreeLibraryAndExitThread(g_dllInstance, 0);
}

BOOL ChangeFileName(LPWSTR lpSrc, size_t cchSrc, LPCWSTR lpNewFileName) {
	size_t pathLength = 0;
	if (!lpSrc || !lpNewFileName || !cchSrc ||
		FAILED(StringCchLengthW(lpSrc, cchSrc, &pathLength))) {
		return FALSE;
	}
	while (pathLength && lpSrc[pathLength - 1] != L'\\' && lpSrc[pathLength - 1] != L'/') {
		--pathLength;
	}
	if (!pathLength) {
		return FALSE;
	}
	return SUCCEEDED(StringCchCopyW(lpSrc + pathLength, cchSrc - pathLength, lpNewFileName));
}

std::string WstringToString(const std::wstring str)
{// wstringתstring
	setlocale(LC_CTYPE, "");
	size_t len = wcstombs(NULL, str.c_str(), 0);
	if (len == (size_t)-1) {
		return std::string();
	}
	char *p = (char*)malloc(len + 1);
	if (!p) {
		return std::string();
	}
	size_t converted = wcstombs(p, str.c_str(), len + 1);
	if (converted == (size_t)-1) {
		free(p);
		return std::string();
	}
	p[converted] = '\0';
	std::string str1(p, converted);
	free(p);
	return str1;
}

// make a unique name with fullname + crc32_of_fullname + familyname +stylename
std::wstring MakeUniqueFontName(const std::wstring strFullName, const std::wstring strFamilyName, const std::wstring strStyleName)
{
	return strFullName + to_wstring(crc32::getCrc32(0, strFullName.c_str(), strFullName.length() * sizeof(WCHAR))) + strFamilyName + strStyleName;
}

#ifndef Assert
#include <crtdbg.h>
#define Assert	_ASSERTE
#endif	//!Assert

#include "array.h"
#include <strsafe.h>
#include <shlwapi.h>
#include "dll.h"

//kernel32専用GetProcAddressモドキ
FARPROC K32GetProcAddress(LPCSTR lpProcName)
{
#ifndef _WIN64
	//序数渡しには対応しない
	//Assert(!IS_INTRESOURCE(lpProcName));

	//kernel32のベースアドレス取得
	LPBYTE pBase = (LPBYTE)GetModuleHandleA("kernel32.dll");

	//この辺は100%成功するはずなのでエラーチェックしない
	PIMAGE_DOS_HEADER pdosh = (PIMAGE_DOS_HEADER)pBase;
	//Assert(pdosh->e_magic == IMAGE_DOS_SIGNATURE);
	PIMAGE_NT_HEADERS pnth = (PIMAGE_NT_HEADERS)(pBase + pdosh->e_lfanew);
	//Assert(pnth->Signature == IMAGE_NT_SIGNATURE);

	const DWORD offs = pnth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	const DWORD size = pnth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
	if (offs == 0 || size == 0) {
		return NULL;
	}

	PIMAGE_EXPORT_DIRECTORY pdir = (PIMAGE_EXPORT_DIRECTORY)(pBase + offs);
	DWORD*	pFunc = (DWORD*)(pBase + pdir->AddressOfFunctions);
	WORD*	pOrd  = (WORD*)(pBase + pdir->AddressOfNameOrdinals);
	DWORD*	pName = (DWORD*)(pBase + pdir->AddressOfNames);

	for(DWORD i=0; i<pdir->NumberOfFunctions; i++) {
		for(DWORD j=0; j<pdir->NumberOfNames; j++) {
			if(pOrd[j] != i)
				continue;

			if(strcmp((LPCSTR)pBase + pName[j], lpProcName) != 0)
				continue;

			return (FARPROC)(pBase + pFunc[i]);
		}
	}
	return NULL;
#else
	//Assert(!IS_INTRESOURCE(lpProcName));

	//kernel32のベースアドレス取得
	WCHAR sysdir[MAX_PATH];
	UINT sysdirLength = GetWindowsDirectory(sysdir, countof(sysdir));
	if (!sysdirLength || sysdirLength >= countof(sysdir) ||
		FAILED(StringCchCatW(sysdir, countof(sysdir), L"\\SysWow64\\kernel32.dll"))) {
		return NULL;
	}
	HANDLE hFile = CreateFile(sysdir, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, NULL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return NULL;
	DWORD dwSize = GetFileSize(hFile, NULL);
	BYTE* pMem = new BYTE[dwSize];	//分配内存
	ReadFile(hFile, pMem, dwSize, &dwSize, NULL);//读取文件
	CloseHandle(hFile);

	CMemLoadDll MemDll;
	MemDll.MemLoadLibrary(pMem, dwSize, false, false);
	delete[] pMem;
	return FARPROC((DWORD_PTR)MemDll.MemGetProcAddress(lpProcName)-MemDll.GetImageBase());	//返回偏移值

#endif
}

typedef struct _UNICODE_STRING64 {
	USHORT Length;
	USHORT MaximumLength;
	DWORD64  Buffer;
} UNICODE_STRING64, *PUNICODE_STRING64;

#include <pshpack1.h>
class opcode_data {
private:
	BYTE	code[0x100];

	//注: dllpathをWORD境界にしないと場合によっては正常に動作しない
	WCHAR	dllpath[MAX_PATH];
	UNICODE_STRING64 uniDllPath;
	DWORD64 hDumyDllHandle;

public:
	opcode_data()
	{
		//int 03hで埋める
		FillMemory(this, sizeof(*this), 0xcc);
	}
	bool initWow64(LPDWORD remoteaddr, LONG orgEIP)	//Wow64初始化
	{
		//WORD嫬奅僠僃僢僋
		C_ASSERT((offsetof(opcode_data, dllpath) & 1) == 0);

		register BYTE* p = code;

#define emit_(t,x)	*(t* UNALIGNED)p = (t)(x); p += sizeof(t)
#define emit_db(b)	emit_(BYTE, b)
#define emit_dw(w)	emit_(WORD, w)
#define emit_dd(d)	emit_(DWORD, d)

		//側偤偐GetProcAddress偱LoadLibraryW偺傾僪儗僗偑惓偟偔庢傟側偄偙偲偑偁傞偺偱
		//kernel32偺僿僢僟偐傜帺慜偱庢摼偡傞
		static FARPROC pfn = K32GetProcAddress("LoadLibraryExW");
		if (!pfn)
			return false;

		emit_db(0x60);		//pushad

		/*
		* obsolete.
			mov eax,fs:[0x30]
			mov eax,[eax+0x0c]
			mov esi,[eax+0x1c]
			lodsd
			move ax,[eax+$08]//这个时候eax中保存的就是k32的基址了
			在win7获得的是KernelBase.dll的地址
		
		emit_db(0x64);
		emit_db(0xA1);
		emit_db(0x30);
		emit_db(00);
		emit_db(00);
		emit_db(00);
		emit_db(0x8B);
		emit_db(0x40);
		emit_db(0x0C);
		emit_db(0x8B);
		emit_db(0x70);
		emit_db(0x1C);
		emit_db(0xAD);
		emit_db(0x8B);
		emit_db(0x40);
		emit_db(0x08);		//use assemble to fetch kernel base
*/
/* faster way of simple comparison of 3 key letters of kernel32.dll. insecure but fast.
001D0001 | 64:8B1D 30000000         | mov ebx,dword ptr fs:[30]                         |
001D0008 | 8B5B 0C                  | mov ebx,dword ptr ds:[ebx+C]                      |
001D000B | 8B73 0C                  | mov esi,dword ptr ds:[ebx+C]                      |
001D000E | 8BD6                     | mov edx,esi                                       |
001D0010 | 8B5A 18                  | mov ebx,dword ptr ds:[edx+18]                     | loop start
001D0013 | 8B7A 30                  | mov edi,dword ptr ds:[edx+30]                     |
001D0016 | 0FB74A 2C                | movzx ecx,word ptr ds:[edx+2C]                    |
001D001A | 66:83F9 18               | cmp cx,18                                         |
001D001E | 75 27                    | jne 1D0047                                        | length not match
001D0020 | 85FF                     | test edi,edi                                      |
001D0022 | 74 23                    | je 1D0047                                         |
001D0024 | 8A07                     | mov al,byte ptr ds:[edi]                          |
001D0026 | 3C 6B                    | cmp al,6B                                         | 6B:'k'
001D0028 | 74 04                    | je 1D002E                                         |
001D002A | 3C 4B                    | cmp al,4B                                         | 4B:'K'
001D002C | 75 19                    | jne 1D0047                                        | not K or k
001D002E | 66:837F 0C 33            | cmp word ptr ds:[edi+C],33                        | 33:'3'
001D0033 | 75 12                    | jne 1D0047                                        |
001D0035 | 66:837F 10 2E            | cmp word ptr ds:[edi+10],2E                       | 2E:'.'
001D003A | 75 0B                    | jne 1D0047                                        |
001D003C | 8A47 16                  | mov al,byte ptr ds:[edi+16]                       |
001D003F | 3C 6C                    | cmp al,6C                                         | 6C:'l'
001D0041 | 74 0C                    | je 1D004F                                         |
001D0043 | 3C 4C                    | cmp al,4C                                         | 4C:'L'
001D0045 | 74 08                    | je 1D004F                                         |
001D0047 | 8B12                     | mov edx,dword ptr ds:[edx]                        | next entry
001D0049 | 3BD6                     | cmp edx,esi                                       |
001D004B | 75 C3                    | jne 1D0010                                        | loop back
001D004D | EB 12                    | jmp 1D0061                                        | not found
001D004F | 8BC3                     | mov eax,ebx                                       | eax=imagebase of kernel32.dll

		emit_db(0x64);
		emit_db(0x8B);
		emit_db(0x1D);
		emit_db(0x30);
		emit_db(0x00);
		emit_db(0x00);
		emit_db(0x00);
		emit_db(0x8B);
		emit_db(0x5B);
		emit_db(0x0C);
		emit_db(0x8B);
		emit_db(0x73);
		emit_db(0x0C);
		emit_db(0x8B);
		emit_db(0xD6);
		emit_db(0x8B);
		emit_db(0x5A);
		emit_db(0x18);
		emit_db(0x8B);
		emit_db(0x7A);
		emit_db(0x30);
		emit_db(0x0F);
		emit_db(0xB7);
		emit_db(0x4A);
		emit_db(0x2C);
		emit_db(0x66);
		emit_db(0x83);
		emit_db(0xF9);
		emit_db(0x18);
		emit_db(0x75);
		emit_db(0x27);
		emit_db(0x85);
		emit_db(0xFF);
		emit_db(0x74);
		emit_db(0x23);
		emit_db(0x8A);
		emit_db(0x07);
		emit_db(0x3C);
		emit_db(0x6B);
		emit_db(0x74);
		emit_db(0x04);
		emit_db(0x3C);
		emit_db(0x4B);
		emit_db(0x75);
		emit_db(0x19);
		emit_db(0x66);
		emit_db(0x83);
		emit_db(0x7F);
		emit_db(0x0C);
		emit_db(0x33);
		emit_db(0x75);
		emit_db(0x12);
		emit_db(0x66);
		emit_db(0x83);
		emit_db(0x7F);
		emit_db(0x10);
		emit_db(0x2E);
		emit_db(0x75);
		emit_db(0x0B);
		emit_db(0x8A);
		emit_db(0x47);
		emit_db(0x16);
		emit_db(0x3C);
		emit_db(0x6C);
		emit_db(0x74);
		emit_db(0x0C);
		emit_db(0x3C);
		emit_db(0x4C);
		emit_db(0x74);
		emit_db(0x08);
		emit_db(0x8B);
		emit_db(0x12);
		emit_db(0x3B);
		emit_db(0xD6);
		emit_db(0x75);
		emit_db(0xC3);
		emit_db(0xEB);
		emit_db(0x12);
		emit_db(0x8B);
		emit_db(0xC3);
*/

/*
001D0001 | BE 3FD6EC8F   | mov esi,8FECD63F                                  | target hash value for kernel32.dll
001D0006 | 64:8B0D 30000 | mov ecx,dword ptr fs:[30]                         |
001D000D | 8B49 0C       | mov ecx,dword ptr ds:[ecx+C]                      |
001D0010 | 8B69 0C       | mov ebp,dword ptr ds:[ecx+C]                      |
001D0013 | 8BD5          | mov edx,ebp                                       |
001D0015 | 8B5A 18       | mov ebx,dword ptr ds:[edx+18]                     | loop start
001D0018 | 8B7A 30       | mov edi,dword ptr ds:[edx+30]                     |
001D001B | 0FB74A 2C     | movzx ecx,word ptr ds:[edx+2C]                    |
001D001F | 85FF          | test edi,edi                                      |
001D0021 | 74 2E         | je 1D0051                                         | skip for empty name dlls
001D0023 | 66:83F9 18    | cmp cx,18                                         | check length
001D0027 | 75 28         | jne 1D0051                                        |
001D0029 | D1E9          | shr ecx,1                                         | ecx = length of unicode name of the dll
001D002B | E3 24         | jecxz 1D0051                                      |
001D002D | 52            | push edx                                          |
001D002E | 33D2          | xor edx,edx                                       | use edx to hash dll names
001D0030 | 0FB607        | movzx eax,byte ptr ds:[edi]                       | letter->al with zero expanding
001D0033 | 8A67 01       | mov ah,byte ptr ds:[edi+1]                        |
001D0036 | 84C0          | test al,al                                        |
001D0038 | 3C 41         | cmp al,41                                         | 41:'A'
001D003A | 7C 06         | jl 1D0042                                         |
001D003C | 3C 5A         | cmp al,5A                                         | 5A:'Z'
001D003E | 7F 02         | jg 1D0042                                         |
001D0040 | 04 20         | add al,20                                         | convert uppercased letters to lowercased
001D0042 | C1CA 0D       | ror edx,D                                         |
001D0045 | 03D0          | add edx,eax                                       | 
001D0047 | 83C7 02       | add edi,2                                         | next letter
001D004A | E2 E4         | loop 1D0030                                       |
001D004C | 3BD6          | cmp edx,esi                                       | 
001D004E | 5A            | pop edx                                           |
001D004F | 74 08         | je 1D0059                                         | match found
001D0051 | 8B12          | mov edx,dword ptr ds:[edx]                        |
001D0053 | 3BD5          | cmp edx,ebp                                       | check if we reached the end of the link table
001D0055 | 75 BE         | jne 1D0015                                        |
001D0057 | EB 12         | jmp 1D006B                                        |
001D0059 | 8BC3          | mov eax,ebx                                       | ebx->eax = image base of kernel32.dll
*/
DWORD hash = 0x8FECD63F; // hash of kernel32.dll
emit_db(0xBE);
emit_dd(hash);

emit_db(0x64);
emit_db(0x8B);
emit_db(0x0D);
emit_db(0x30);
emit_db(0x00);
emit_db(0x00);
emit_db(0x00);
emit_db(0x8B);
emit_db(0x49);
emit_db(0x0C);
emit_db(0x8B);
emit_db(0x69);
emit_db(0x0C);
emit_db(0x8B);
emit_db(0xD5);
emit_db(0x8B);
emit_db(0x5A);
emit_db(0x18);
emit_db(0x8B);
emit_db(0x7A);
emit_db(0x30);
emit_db(0x0F);
emit_db(0xB7);
emit_db(0x4A);
emit_db(0x2C);
emit_db(0x85);
emit_db(0xFF);
emit_db(0x74);
emit_db(0x2E);
emit_db(0x66);
emit_db(0x83);
emit_db(0xF9);
emit_db(0x18);
emit_db(0x75);
emit_db(0x28);
emit_db(0xD1);
emit_db(0xE9);
emit_db(0xE3);
emit_db(0x24);
emit_db(0x52);
emit_db(0x33);
emit_db(0xD2);
emit_db(0x0F);
emit_db(0xB6);
emit_db(0x07);
emit_db(0x8A);
emit_db(0x67);
emit_db(0x01);
emit_db(0x84);
emit_db(0xC0);
emit_db(0x3C);
emit_db(0x41);
emit_db(0x7C);
emit_db(0x06);
emit_db(0x3C);
emit_db(0x5A);
emit_db(0x7F);
emit_db(0x02);
emit_db(0x04);
emit_db(0x20);
emit_db(0xC1);
emit_db(0xCA);
emit_db(0x0D);
emit_db(0x03);
emit_db(0xD0);
emit_db(0x83);
emit_db(0xC7);
emit_db(0x02);
emit_db(0xE2);
emit_db(0xE4);
emit_db(0x3B);
emit_db(0xD6);
emit_db(0x5A);
emit_db(0x74);
emit_db(0x08);
emit_db(0x8B);
emit_db(0x12);
emit_db(0x3B);
emit_db(0xD5);
emit_db(0x75);
emit_db(0xBE);
emit_db(0xEB);
emit_db(0x12);
emit_db(0x8B);
emit_db(0xC3);


		emit_dw(0x006A);	//push 0
		emit_dw(0x006A);	//push 0
		emit_db(0x68);		//push dllpath
		emit_dd((LONG)remoteaddr + offsetof(opcode_data, dllpath));
		emit_db(0x05);		//add eax, LoadLibraryExW offset
		emit_dd(pfn);
		emit_dw(0xD0FF);	//call eax

		emit_db(0x61);		//popad
		emit_db(0xE9);		//jmp original_EIP
		emit_dd(orgEIP - (LONG)remoteaddr - (p - code) - sizeof(LONG));

		// gdi++.dllのパス
		int nSize = GetModuleFileNameW(GetDLLInstance(), dllpath, countof(dllpath));
		return nSize > 0 && nSize < countof(dllpath) &&
			ChangeFileName(dllpath, countof(dllpath), L"MTBootStrap.dll");
	}
	bool init32(LPDWORD remoteaddr, LONG orgEIP)	//32位程序初始化
	{
		//WORD境界チェック
		C_ASSERT((offsetof(opcode_data, dllpath) & 1) == 0);

		register BYTE* p = code;

#define emit_(t,x)	*(t* UNALIGNED)p = (t)(x); p += sizeof(t)
#define emit_db(b)	emit_(BYTE, b)
#define emit_dw(w)	emit_(WORD, w)
#define emit_dd(d)	emit_(DWORD, d)

		//なぜかGetProcAddressでLoadLibraryWのアドレスが正しく取れないことがあるので
		//kernel32のヘッダから自前で取得する
		static FARPROC pfn = K32GetProcAddress("LoadLibraryW");
		if(!pfn)
			return false;

		emit_db(0x60);		//pushad
#if _DEBUG
emit_dw(0xC033);	//xor eax, eax
emit_db(0x50);		//push eax
emit_db(0x50);		//push eax
emit_db(0x68);		//push dllpath
emit_dd((LONG)remoteaddr + offsetof(opcode_data, dllpath));
emit_db(0x50);		//push eax
emit_db(0xB8);		//mov eax, MessageBoxW
emit_dd((LONG)MessageBoxW);
emit_dw(0xD0FF);	//call eax
#endif

		emit_db(0x68);		//push dllpath
		emit_dd((LONG)remoteaddr + offsetof(opcode_data, dllpath));
		emit_db(0xB8);		//mov eax, LoadLibraryW
		emit_dd(pfn);
		emit_dw(0xD0FF);	//call eax

		emit_db(0x61);		//popad
		emit_db(0xE9);		//jmp original_EIP
		emit_dd(orgEIP - (LONG)remoteaddr - (p - code) - sizeof(LONG));

		// gdi++.dllのパス
		int nSize = GetModuleFileNameW(GetDLLInstance(), dllpath, countof(dllpath));
		return nSize > 0 && nSize < countof(dllpath) &&
			ChangeFileName(dllpath, countof(dllpath), L"MTBootStrap.dll");
	}
	bool init64From32(DWORD64 remoteaddr, DWORD64 orgEIP)
	{
		C_ASSERT((offsetof(opcode_data, dllpath) & 1) == 0);

		register BYTE* p = code;

#define emit_(t,x)	*(t* UNALIGNED)p = (t)(x); p += sizeof(t)
#define emit_db(b)	emit_(BYTE, b)
#define emit_dw(w)	emit_(WORD, w)
#define emit_dd(d)	emit_(DWORD, d)
#define emit_ddp(dp) emit_(DWORD64, dp)

		//なぜかGetProcAddressでLoadLibraryWのアドレスが正しく取れないことがあるので
		//kernel32のヘッダから自前で取得する
		WCHAR x64Addr[30] = { 0 };
		if (!GetEnvironmentVariable(L"MACTYPE_X64ADDR", x64Addr, 29)) return false;
		DWORD64 pfn = wcstoull(x64Addr, NULL, 10);
		//DWORD64 pfn = getenv("MACTYPE_X64ADDR"); //GetProcAddress64(GetModuleHandle64(L"kernelbase.dll"), "LoadLibraryW");
		if (!pfn)
			return false;

		emit_db(0x50);		//push rax
		emit_db(0x51);		//push rcx
		emit_db(0x52);		//push rdx
		emit_db(0x53);		//push rbx
		emit_dd(0x28ec8348);	//sub rsp,28h
		emit_db(0x48);		//mov rcx, dllpath
		emit_db(0xB9);
		emit_ddp((DWORD64)remoteaddr + offsetof(opcode_data, dllpath));
		emit_db(0x48);		//mov rsi, LoadLibraryW
		emit_db(0xBE);
		emit_ddp(pfn);
		//emit_db(0x48);
		emit_db(0xFF);	//call rdi
		emit_db(0xD6);

		emit_dd(0x28c48348);	//add rsp,28h
		emit_db(0x5B);
		emit_db(0x5A);
		emit_db(0x59);
		emit_db(0x58);		//popad		

		emit_db(0x48);		//mov rdi, orgRip
		emit_db(0xBE);
		emit_ddp(orgEIP);
		emit_db(0xFF);		//jmp rdi
		emit_db(0xE6);

		// gdi++.dllのパス

		int nSize = GetModuleFileNameW(GetDLLInstance(), dllpath, countof(dllpath));
		return nSize > 0 && nSize < countof(dllpath) &&
			ChangeFileName(dllpath, countof(dllpath), L"MTBootStrap64.dll");
	}

	bool init64From32(DWORD64 remoteaddr, DWORD64 orgEIP, DWORD dwLoaderOffset)
	{
		C_ASSERT((offsetof(opcode_data, dllpath) & 1) == 0);

		int nSize = GetModuleFileNameW(GetDLLInstance(), dllpath, countof(dllpath));
		if (!nSize || nSize >= countof(dllpath) ||
			!ChangeFileName(dllpath, countof(dllpath), L"MTBootStrap64.dll"))
			return false;
		uniDllPath.Length = wcslen(dllpath)*sizeof(WCHAR);
		uniDllPath.MaximumLength = uniDllPath.Length+2;
		uniDllPath.Buffer = remoteaddr + (DWORD64)offsetof(opcode_data, dllpath);	//prepare PUNICODE_STRING for remote process
		register BYTE* p = code;

#define emit_(t,x)	*(t* UNALIGNED)p = (t)(x); p += sizeof(t)
#define emit_db(b)	emit_(BYTE, b)
#define emit_dw(w)	emit_(WORD, w)
#define emit_dd(d)	emit_(DWORD, d)
#define emit_ddp(dp) emit_(DWORD64, dp)

//get ntdll.dll imagebase
//credit to http://www.52pojie.cn/thread-162625-1-1.html
/*asm:
	mov rsi, [gs:60h]   ;     peb from teb
	mov rsi, [rsi+18h]    ;_peb_ldr_data from peb
	mov rsi, [rsi+30h]   ;InInitializationOrderModuleList.Flink, ntdll.dll
	;mov rsi, [rsi]  ;kernelbase.dll
	;mov rsi, [rsi]      ;kernel32.dll (not used for win7+)
	mov rsi, [rsi+10h]
*/

// emit_db(0xEB);
// emit_db(0xFE);	// make a dead loop
		emit_db(0x65);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x34);
		emit_db(0x25);
		emit_db(0x60);
		emit_db(0x00);
		emit_db(0x00);
		emit_db(0x00);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x76);
		emit_db(0x18);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x76);
		emit_db(0x30);
// 		emit_db(0x48);
// 		emit_db(0x8B);
// 		emit_db(0x36);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x76);
		emit_db(0x10);
//rsi = ntdll.dll baseaddress

		emit_db(0x50);		//push rax
		emit_db(0x51);		//push rcx
		emit_db(0x52);		//push rdx
		emit_db(0x53);		//push rbx
		emit_dd(0x28ec8348);	//sub rsp,28h
		emit_db(0x48);
		emit_db(0x31);
		emit_db(0xc9);	//xor rcx, rcx
		emit_db(0x48);
		emit_db(0x31);
		emit_db(0xd2);	//xor rdx, rdx
		emit_db(0x49);		
		emit_db(0xB8);
		emit_ddp((DWORD64)remoteaddr + offsetof(opcode_data, uniDllPath));//mov r8, uniDllPath
		emit_db(0x49);
		emit_db(0xB9);
		emit_ddp((DWORD64)remoteaddr + offsetof(opcode_data, hDumyDllHandle));//mov r9, hDumyDllHandle
		//emit_db(0x48);		//mov rsi, LdrLoadDll
		//emit_db(0xBE);
		emit_db(0x48);
		emit_db(0x81);
		emit_db(0xC6);	//add rsi, offset LdrLoadDll
		emit_dd(dwLoaderOffset);
		//emit_db(0x48);
		emit_db(0xFF);	//call rsi
		emit_db(0xD6);

		emit_dd(0x28c48348);	//add rsp,28h
		emit_db(0x5B);
		emit_db(0x5A);
		emit_db(0x59);
		emit_db(0x58);		//popad		

		emit_db(0x48);		//mov rdi, orgRip
		emit_db(0xBE);
		emit_ddp(orgEIP);
		emit_db(0xFF);		//jmp rdi
		emit_db(0xE6);

		// gdi++.dllのパス

		return !!nSize;
	}

	bool init(DWORD_PTR* remoteaddr, DWORD_PTR orgEIP)
	{
		//WORD境界チェック
		C_ASSERT((offsetof(opcode_data, dllpath) & 1) == 0);

		register BYTE* p = code;
#undef emit_ddp

#define emit_(t,x)	*(t* UNALIGNED)p = (t)(x); p += sizeof(t)
#define emit_db(b)	emit_(BYTE, b)
#define emit_dw(w)	emit_(WORD, w)
#define emit_dd(d)	emit_(DWORD, d)
#define emit_ddp(dp) emit_(DWORD_PTR, dp)

		//なぜかGetProcAddressでLoadLibraryWのアドレスが正しく取れないことがあるので
		//kernel32のヘッダから自前で取得する
		static FARPROC pfn = (FARPROC)((INT_PTR)CDllHelper::MyGetProcAddress(GetModuleHandle(L"kernel32.dll"), L"LoadLibraryW") - (INT_PTR)GetModuleHandle(L"kernel32.dll"));
		/*WCHAR msg[500] = { 0 };
		wsprintf(msg, L"API paddr: 0x%I64x\r\nOffset: %x\r\nAPI addr: 0x%I64x\r\nKernel32.dll: 0x%I64x\r\nKernelBase: 0x%I64x", (DWORD_PTR)pfn, *(PDWORD)pfn, *(PDWORD)pfn + (DWORD_PTR)GetModuleHandle(L"kernel32.dll"),
			(DWORD_PTR)GetModuleHandle(L"kernel32.dll"), (DWORD_PTR)GetModuleHandle(L"kernelbase.dll"));
		MessageBoxW(NULL, msg, NULL, MB_OK);*/
		//if(!pfn)
		//	return false;
		//emit_db(0xEB);
		//emit_db(0xFE);	// make a dead loop

		emit_db(0x50);		//push rax
		emit_db(0x51);		//push rcx
		emit_db(0x52);		//push rdx
		emit_db(0x53);		//push rbx
		/*
#ifdef DEBUG
		emit_dd(0x28ec8348);
		emit_db(0x48);
		emit_db(0x31);
		emit_db(0xD0);	//xor rax,rax
		emit_db(0x48);
		emit_db(0x31);
		emit_db(0xC9);	//xor rcx,rcx
		emit_db(0x48);
		emit_db(0x31);
		emit_db(0xD2);	//xor rdx,rdx
		emit_db(0x45);
		emit_db(0x31);
		emit_db(0xC0);	//xor r8d,r8d
		emit_db(0x45);
		emit_db(0x31);
		emit_db(0xC9);	//xor r9d,r9d

		emit_db(0x48);		//mov rsi, MessageBoxW
		emit_db(0xBE);
		emit_ddp((DWORD_PTR)MessageBoxW);
		emit_db(0xFF);
		emit_db(0xD6);
		emit_dd(0x28c48348);
#endif*/
/*	
//Debug function2, Sleep for 10sec.
		emit_dd(0x28ec8348);
		emit_db(0x48);		//mov rsi, MessageBoxW
		emit_db(0xBE);
		emit_ddp((DWORD_PTR)Sleep);
		emit_db(0x48); emit_db(0xc7); emit_db(0xc1); emit_db(0x10); emit_db(0x27); emit_db(0x00); emit_db(0x00);
		emit_db(0xFF);
		emit_db(0xD6);
		emit_dd(0x28c48348);
*/

//shellcode to find imagebase of kernel32.dll (under x64)
//rax will store the imagebase of kernel32.dll, fast but not reliable. does not work in some scenarios
/*
| 65 48 8B  | mov rax,qword ptr gs:[60]                                                  |
| 48 8B 40  | mov rax,qword ptr ds:[rax+18]                                              |
| 48 8B 40  | mov rax,qword ptr ds:[rax+30]                                              |
| 48 8B 00  | mov rax,qword ptr ds:[rax]                                                 |
| 48 8B 00  | mov rax,qword ptr ds:[rax]                                                 |
| 48 8B 40  | mov rax,qword ptr ds:[rax+10]                                              |

		emit_db(0x65);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x04);
		emit_db(0x25);
		emit_db(0x60);
		emit_db(0x00);
		emit_db(0x00);
		emit_db(0x00);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x40);
		emit_db(0x18);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x40);
		emit_db(0x30);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x00);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x00);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x40);
		emit_db(0x10);
*/
/* plan B, accurate search through PEB
00000233B6160004  | 6548:8B0425 60000000     | mov rax,qword ptr gs:[60]                                       |
00000233B616000D  | 48:8B40 18               | mov rax,qword ptr ds:[rax+18]                                   |
00000233B6160011  | 48:8B50 10               | mov rdx,qword ptr ds:[rax+10]                                   |
00000233B6160015  | 4C:8B4A 30               | mov r9,qword ptr ds:[rdx+30]                                    |
00000233B6160019  | 4C:8B42 60               | mov r8,qword ptr ds:[rdx+60]                                    |
00000233B616001D  | 0FB74A 58                | movzx ecx,word ptr ds:[rdx+58]                                  |
00000233B6160021  | 66:83F9 18               | cmp cx,18                                                       |
00000233B6160025  | 75 2C                    | jne 233B6160053                                                 |
00000233B6160027  | 4D:85C0                  | test r8,r8                                                      |
00000233B616002A  | 74 27                    | je 233B6160053                                                  |
00000233B616002C  | 41:8A00                  | mov al,byte ptr ds:[r8]                                         |
00000233B616002F  | 3C 6B                    | cmp al,6B                                                       | 6B:'k'
00000233B6160031  | 74 04                    | je 233B6160037                                                  |
00000233B6160033  | 3C 4B                    | cmp al,4B                                                       | 4B:'K'
00000233B6160035  | 75 1C                    | jne 233B6160053                                                 |
00000233B6160037  | 6641:8378 0C 33          | cmp word ptr ds:[r8+C],33                                       | 33:'3'
00000233B616003D  | 75 14                    | jne 233B6160053                                                 |
00000233B616003F  | 6641:8378 10 2E          | cmp word ptr ds:[r8+10],2E                                      | 2E:'.'
00000233B6160045  | 75 0C                    | jne 233B6160053                                                 |
00000233B6160047  | 41:8A40 16               | mov al,byte ptr ds:[r8+16]                                      |
00000233B616004B  | 3C 6C                    | cmp al,6C                                                       | 6C:'l'
00000233B616004D  | 74 0F                    | je 233B616005E                                                  | found
00000233B616004F  | 3C 4C                    | cmp al,4C                                                       | 4C:'L'
00000233B6160051  | 74 0B                    | je 233B616005E                                                  | found
00000233B6160053  | 48:8B12                  | mov rdx,qword ptr ds:[rdx]                                      |
00000233B6160056  | 48:3B50 10               | cmp rdx,qword ptr ds:[rax+10]                                   |
00000233B616005A  | 75 B9                    | jne 233B6160015                                                 | loop back
00000233B616005C  | EB 25                    | jmp 233B6160083                                                 | not found
00000233B616005E  | 49:8BC1                  | mov rax,r9                                                      | :found, r9->imagebase
		
		emit_db(0x65);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x04);
		emit_db(0x25);
		emit_db(0x60);
		emit_db(0x00);
		emit_db(0x00);
		emit_db(0x00);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x40);
		emit_db(0x18);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x50);
		emit_db(0x10);
		emit_db(0x4C);
		emit_db(0x8B);
		emit_db(0x4A);
		emit_db(0x30);
		emit_db(0x4C);
		emit_db(0x8B);
		emit_db(0x42);
		emit_db(0x60);
		emit_db(0x0F);
		emit_db(0xB7);
		emit_db(0x4A);
		emit_db(0x58);
		emit_db(0x66);
		emit_db(0x83);
		emit_db(0xF9);
		emit_db(0x18);
		emit_db(0x75);
		emit_db(0x2C);
		emit_db(0x4D);
		emit_db(0x85);
		emit_db(0xC0);
		emit_db(0x74);
		emit_db(0x27);
		emit_db(0x41);
		emit_db(0x8A);
		emit_db(0x00);
		emit_db(0x3C);
		emit_db(0x6B);
		emit_db(0x74);
		emit_db(0x04);
		emit_db(0x3C);
		emit_db(0x4B);
		emit_db(0x75);
		emit_db(0x1C);
		emit_db(0x66);
		emit_db(0x41);
		emit_db(0x83);
		emit_db(0x78);
		emit_db(0x0C);
		emit_db(0x33);
		emit_db(0x75);
		emit_db(0x14);
		emit_db(0x66);
		emit_db(0x41);
		emit_db(0x83);
		emit_db(0x78);
		emit_db(0x10);
		emit_db(0x2E);
		emit_db(0x75);
		emit_db(0x0C);
		emit_db(0x41);
		emit_db(0x8A);
		emit_db(0x40);
		emit_db(0x16);
		emit_db(0x3C);
		emit_db(0x6C);
		emit_db(0x74);
		emit_db(0x0F);
		emit_db(0x3C);
		emit_db(0x4C);
		emit_db(0x74);
		emit_db(0x0B);
		emit_db(0x48);
		emit_db(0x8B);
		emit_db(0x12);
		emit_db(0x48);
		emit_db(0x3B);
		emit_db(0x50);
		emit_db(0x10);
		emit_db(0x75);
		emit_db(0xB9);
		emit_db(0xEB);
		emit_db(0x25);
		emit_db(0x49);
		emit_db(0x8B);
		emit_db(0xC1);
		*/
/*
0000024429180004  | 41:BC 72785CE1           | mov r12d,8FECD63F                                               |
000002442918000A  | 6548:8B0425 60000000     | mov rax,qword ptr gs:[60]                                       |
0000024429180013  | 48:8B40 18               | mov rax,qword ptr ds:[rax+18]                                   |
0000024429180017  | 48:8B50 10               | mov rdx,qword ptr ds:[rax+10]                                   |
000002442918001B  | 4C:8BFA                  | mov r15,rdx                                                     | r15=link start
000002442918001E  | 4C:8B4A 30               | mov r9,qword ptr ds:[rdx+30]                                    | loop start, r9=imagebase
0000024429180022  | 4C:8B42 60               | mov r8,qword ptr ds:[rdx+60]                                    |
0000024429180026  | 0FB74A 58                | movzx ecx,word ptr ds:[rdx+58]                                  |
000002442918002A  | 83F9 18                  | cmp ecx,18                                                      | quick length check
000002442918002D  | 75 2B                    | jne 2442918005A                                                 |
000001EE63DE002F  | D1E9                     | shr ecx,1                                                       | ecx >> 1 = length
000002442918002F  | 4D:33DB                  | xor r11,r11                                                     | r11=hash
0000024429180032  | 41:0FB700                | movzx eax,word ptr ds:[r8]                                      |
0000024429180036  | 0FB6D8                   | movzx ebx,al                                                    |
0000024429180039  | 80FB 41                  | cmp bl,41                                                       | 41:'A'
000002442918003C  | 7C 08                    | jl 24429180046                                                  |
000002442918003E  | 80FB 5A                  | cmp bl,5A                                                       | 5A:'Z'
0000024429180041  | 7F 03                    | jg 24429180046                                                  |
0000024429180043  | 80C3 20                  | add bl,20                                                       | change uppercased letters to lowercased
0000024429180046  | 41:C1CB 0D               | ror r11d,D                                                      |
000002442918004A  | 44:03DB                  | add r11d,ebx                                                    |
000002442918004D  | 49:83C0 02               | add r8,2                                                        |
0000024429180051  | FFC9                     | dec ecx                                                         |
0000024429180053  | 75 DD                    | jne 24429180032                                                 |
0000024429180055  | 45:3BDC                  | cmp r11d,r12d                                                   | hash check
0000024429180058  | 74 0A                    | je 24429180064                                                  |
000002442918005A  | 48:8B12                  | mov rdx,qword ptr ds:[rdx]                                      | not found, next
000002442918005D  | 49:3BD7                  | cmp rdx,r15                                                     |
0000024429180060  | 75 BC                    | jne 2442918001E                                                 |
0000024429180062  | EB 25                    | jmp 24429180089                                                 | not found, skip loading
0000024429180064  | 49:8BC1                  | mov rax,r9                                                      | found, rax=imagebase
*/

DWORD hash = 0x8FECD63F;
emit_db(0x41);
emit_db(0xBC);
emit_dd(hash);

emit_db(0x65);
emit_db(0x48);
emit_db(0x8B);
emit_db(0x04);
emit_db(0x25);
emit_db(0x60);
emit_db(0x00);
emit_db(0x00);
emit_db(0x00);
emit_db(0x48);
emit_db(0x8B);
emit_db(0x40);
emit_db(0x18);
emit_db(0x48);
emit_db(0x8B);
emit_db(0x50);
emit_db(0x10);
emit_db(0x4C);
emit_db(0x8B);
emit_db(0xFA);
emit_db(0x4C);
emit_db(0x8B);
emit_db(0x4A);
emit_db(0x30);
emit_db(0x4C);
emit_db(0x8B);
emit_db(0x42);
emit_db(0x60);
emit_db(0x0F);
emit_db(0xB7);
emit_db(0x4A);
emit_db(0x58);
emit_db(0x83);
emit_db(0xF9);
emit_db(0x18);
emit_db(0x75);
emit_db(0x2D);
emit_db(0xD1);
emit_db(0xE9);
emit_db(0x4D);
emit_db(0x33);
emit_db(0xDB);
emit_db(0x41);
emit_db(0x0F);
emit_db(0xB7);
emit_db(0x00);
emit_db(0x0F);
emit_db(0xB6);
emit_db(0xD8);
emit_db(0x80);
emit_db(0xFB);
emit_db(0x41);
emit_db(0x7C);
emit_db(0x08);
emit_db(0x80);
emit_db(0xFB);
emit_db(0x5A);
emit_db(0x7F);
emit_db(0x03);
emit_db(0x80);
emit_db(0xC3);
emit_db(0x20);
emit_db(0x41);
emit_db(0xC1);
emit_db(0xCB);
emit_db(0x0D);
emit_db(0x44);
emit_db(0x03);
emit_db(0xDB);
emit_db(0x49);
emit_db(0x83);
emit_db(0xC0);
emit_db(0x02);
emit_db(0xFF);
emit_db(0xC9);
emit_db(0x75);
emit_db(0xDD);
emit_db(0x45);
emit_db(0x3B);
emit_db(0xDC);
emit_db(0x74);
emit_db(0x0A);
emit_db(0x48);
emit_db(0x8B);
emit_db(0x12);
emit_db(0x49);
emit_db(0x3B);
emit_db(0xD7);
emit_db(0x75);
emit_db(0xBA);
emit_db(0xEB);
emit_db(0x25);
emit_db(0x49);
emit_db(0x8B);
emit_db(0xC1);


		// === end of shellcode ===

		emit_dd(0x28ec8348);	//sub rsp,28h
		emit_db(0x48);		//mov rcx, dllpath
		emit_db(0xB9);
		emit_ddp((DWORD_PTR)remoteaddr + offsetof(opcode_data, dllpath));

		emit_db(0x48);	// mov rdx, rax
		emit_db(0x89);
		emit_db(0xC2);

		emit_db(0x48);	// add rax, offset of LoadLibrary IAT 
		emit_db(0x05);
		emit_dd(pfn);

		/*  __asm:
				mov eax,dword ptr ds:[rax]
				add rdx,rax
				call rdx
		*/
		emit_db(0x8B);
		emit_db(0x00);
		emit_db(0x48);
		emit_db(0x01);
		emit_db(0xC2);
		emit_db(0xFF);
		emit_db(0xD2);

		emit_dd(0x28c48348);	//add rsp,28h
		emit_db(0x5B);
		emit_db(0x5A);
		emit_db(0x59);
		emit_db(0x58);		//popad		

		emit_db(0x48);		//mov rdi, orgRip
		emit_db(0xBE);
		emit_ddp(orgEIP);
		emit_db(0xFF);		//jmp rdi
		emit_db(0xE6);

		// gdi++.dllのパス
		int nSize = GetModuleFileNameW(GetDLLInstance(), dllpath, countof(dllpath));
		return nSize > 0 && nSize < countof(dllpath) &&
			ChangeFileName(dllpath, countof(dllpath), L"MTBootStrap64.dll");
	}

};
#include <poppack.h>

// 安全的取得真实系统信息
VOID SafeGetNativeSystemInfo(__out LPSYSTEM_INFO lpSystemInfo)
{
	if (NULL == lpSystemInfo)    return;
	typedef VOID(WINAPI *LPFN_GetNativeSystemInfo)(LPSYSTEM_INFO lpSystemInfo);
	LPFN_GetNativeSystemInfo fnGetNativeSystemInfo = (LPFN_GetNativeSystemInfo)GetProcAddress(GetModuleHandle(_T("kernel32")), "GetNativeSystemInfo");;
	if (NULL != fnGetNativeSystemInfo)
	{
		fnGetNativeSystemInfo(lpSystemInfo);
	}
	else
	{
		GetSystemInfo(lpSystemInfo);
	}
}

// 获取操作系统位数
int GetSystemBits()
{
	SYSTEM_INFO si;
	SafeGetNativeSystemInfo(&si);
	if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
		si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64)
	{
		return 64;
	}
	return 32;
}

static bool bIsOS64 = GetSystemBits() == 64;	// check if running in a x64 system.

#ifdef _M_IX86
// 止めているプロセスにLoadLibraryするコードを注入
EXTERN_C BOOL WINAPI GdippInjectDLL(const PROCESS_INFORMATION* ppi)
{
	BOOL bIsX64Proc = false;
	if (bIsOS64 && IsWow64Process(ppi->hProcess, &bIsX64Proc) && !bIsX64Proc)
	{
		//x86 process launches a x64 process
		_CONTEXT64 ctx = { 0 };
		ctx.ContextFlags = CONTEXT_CONTROL;
		if (!GetThreadContext64(ppi->hThread, &ctx))
			return false;
		static bool bTryLoadDll64 = false;
		static DWORD dwLoaderOffset = 0;
		if (!bTryLoadDll64) {
			bTryLoadDll64 = true;
			GetEnvironmentVariable(L"MACTYPE_X64ADDR", NULL, 0);
			if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
				DWORD64 hNtdll = 0;
				hNtdll = GetModuleHandle64(L"ntdll.dll");
				if (hNtdll) {
					DWORD64 pfnLdrAddr = GetProcAddress64(hNtdll, "LdrLoadDll");
					if (pfnLdrAddr) {
						dwLoaderOffset = (DWORD)(pfnLdrAddr - hNtdll);
					}
				}
			}
		}

		opcode_data local;
		DWORD64 remote = VirtualAllocEx64(ppi->hProcess, NULL, sizeof(opcode_data), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (!remote)
			return false;
		bool basmIniter = dwLoaderOffset ? local.init64From32(remote, ctx.Rip, dwLoaderOffset) : local.init64From32(remote, ctx.Rip);
		if (!basmIniter	|| !WriteProcessMemory64(ppi->hProcess, remote, &local, sizeof(opcode_data), NULL)) {
			VirtualFreeEx64(ppi->hProcess, remote, 0, MEM_RELEASE);
			return false;
		}

		//FlushInstructionCache64(ppi->hProcess, remote, sizeof(opcode_data));
		//FARPROC a=(FARPROC)remote;
		//a();
		ctx.Rip = (DWORD64)remote;
		return !!SetThreadContext64(ppi->hThread, &ctx);
	}
	else {
		CONTEXT ctx = { 0 };
		ctx.ContextFlags = CONTEXT_CONTROL;
		if (!GetThreadContext(ppi->hThread, &ctx))
			return false;

		opcode_data local;
		opcode_data* remote = (opcode_data*)VirtualAllocEx(ppi->hProcess, NULL, sizeof(opcode_data), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (!remote)
			return false;

		if (!local.init32((LPDWORD)remote, ctx.Eip)
			|| !WriteProcessMemory(ppi->hProcess, remote, &local, sizeof(opcode_data), NULL)) {
			VirtualFreeEx(ppi->hProcess, remote, 0, MEM_RELEASE);
			return false;
		}

		FlushInstructionCache(ppi->hProcess, remote, sizeof(opcode_data));
		ctx.Eip = (DWORD)remote;
		return !!SetThreadContext(ppi->hThread, &ctx);
	}
}
#else
EXTERN_C BOOL WINAPI GdippInjectDLL(const PROCESS_INFORMATION* ppi)
{
	BOOL bWow64 = false;
	IsWow64Process(ppi->hProcess, &bWow64);
	if (bWow64)
	{
		WOW64_CONTEXT ctx = { 0 };
		ctx.ContextFlags = CONTEXT_CONTROL;
		//CREATE_SUSPENDEDなので基本的に成功するはず
		if(!Wow64GetThreadContext(ppi->hThread, &ctx))
			return false;

		opcode_data local;
		LPVOID remote = VirtualAllocEx(ppi->hProcess, NULL, sizeof(opcode_data), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if(!remote)
			return false;

		if(!local.initWow64((LPDWORD)remote, ctx.Eip)
			|| !WriteProcessMemory(ppi->hProcess, remote, &local, sizeof(opcode_data), NULL)) {
				VirtualFreeEx(ppi->hProcess, remote, 0, MEM_RELEASE);
				return false;
		}

		FlushInstructionCache(ppi->hProcess, remote, sizeof(opcode_data));
		//FARPROC a=(FARPROC)remote;
		//a();
		ctx.Eip = (DWORD)remote;
		return !!Wow64SetThreadContext(ppi->hThread, &ctx);
	}
	else
	{
		CONTEXT ctx = { 0 };
		ctx.ContextFlags = CONTEXT_CONTROL;
		//CREATE_SUSPENDEDなので基本的に成功するはず
		if(!GetThreadContext(ppi->hThread, &ctx))
			return false;

		opcode_data local;
		LPVOID remote = VirtualAllocEx(ppi->hProcess, NULL, sizeof(opcode_data), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if(!remote)
			return false;

		if(!local.init((DWORD_PTR*)remote, ctx.Rip)
			|| !WriteProcessMemory(ppi->hProcess, remote, &local, sizeof(opcode_data), NULL)) {
				VirtualFreeEx(ppi->hProcess, remote, 0, MEM_RELEASE);
				return false;
		}

		FlushInstructionCache(ppi->hProcess, remote, sizeof(opcode_data));
		//FARPROC a=(FARPROC)remote;
		//a();
		ctx.Rip = (DWORD_PTR)remote;
		return !!SetThreadContext(ppi->hThread, &ctx);
	}
}

#endif

template <typename _TCHAR>
int strlendb(const _TCHAR* psz)
{
	const _TCHAR* p = psz;
	while (*p) {
		for (; *p; p++);
		p++;
	}
	return p - psz + 1;
}

template <typename _TCHAR>
_TCHAR* strdupdb(const _TCHAR* psz, int pad)
{
	int len = strlendb(psz);
	_TCHAR* p = (_TCHAR*)calloc(sizeof(_TCHAR), len + pad);
	if(p) {
		memcpy(p, psz, sizeof(_TCHAR) * len);
	}
	return p;
}



bool MultiSzToArray(LPWSTR p, CArray<LPWSTR>& arr)
{
	for (; *p; ) {
		LPWSTR cp = _wcsdup(p);
		if(!cp || !arr.Add(cp)) {
			free(cp);
			return false;
		}
		for (; *p; p++);
		p++;
	}
	return true;
}

LPWSTR ArrayToMultiSz(CArray<LPWSTR>& arr)
{
	size_t cch = 1;
	for (int i=0; i<arr.GetSize(); i++) {
		cch += wcslen(arr[i]) + 1;
	}

	LPWSTR pmsz = (LPWSTR)calloc(sizeof(WCHAR), cch);
	if (!pmsz)
		return NULL;

	LPWSTR p = pmsz;
	for (int i=0; i<arr.GetSize(); i++) {
		StringCchCopyExW(p, cch, arr[i], &p, &cch, STRSAFE_NO_TRUNCATION);
		p++;
	}
	*p = 0;
	return pmsz;
}

bool AddPathEnv(CArray<LPWSTR>& arr, LPWSTR dir, int dirlen)
{
	for (int i=0; i<arr.GetSize(); i++) {
		LPWSTR env = arr[i];
		if (_wcsnicmp(env, L"PATH=", 5)) {
			continue;
		}

		LPWSTR p = env + 5;
		LPWSTR pp = p;
		for (; ;) {
			for (; *p && *p != L';'; p++);
			int len = p - pp;
			if (len == dirlen && !_wcsnicmp(pp, dir, dirlen)) {
				return false;
			}
			if (!*p)
				break;
			pp = p + 1;
			p++;
		}

		size_t cch = wcslen(env) + MAX_PATH + 4;
		env = (LPWSTR)realloc(env, sizeof(WCHAR) * cch);
		if(env) {
			StringCchCatW(env, cch, L";");
			StringCchCatW(env, cch, dir);
			arr[i] = env;
			return true;
		}
		return false;
	}

	size_t cch = dirlen + sizeof("PATH=") + 1;
	LPWSTR p = (LPWSTR)calloc(sizeof(WCHAR), cch);
	if(p) {
		StringCchCopyW(p, cch, L"PATH=");
		StringCchCatW(p, cch, dir);
		if (arr.Add(p)) {
			return true;
		}
		free(p);
	}
	return false;
}

bool AddX64Env(CArray<LPWSTR>& arr)
{
	FARPROC k32 = GetProcAddress(GetModuleHandle(L"kernel32.dll"), "LoadLibraryW");
	WCHAR szAddr[20] = { 0 };
	_ui64tow((DWORD64)k32, szAddr, 10);
	//wsprintf(szAddr, L"%Ld", (DWORD_PTR)k32);
	size_t cch = wcslen(szAddr) + sizeof("MACTYPE_X64ADDR=") + 1;
	LPWSTR p = (LPWSTR)calloc(sizeof(WCHAR), cch);
	if (p) {
		StringCchCopyW(p, cch, L"MACTYPE_X64ADDR=");
		StringCchCatW(p, cch, szAddr);
		if (arr.Add(p)) {
			return true;
		}
		free(p);
	}
	return false;
}

EXTERN_C LPWSTR WINAPI GdippEnvironment(DWORD& dwCreationFlags, LPVOID lpEnvironment)
{
#ifndef _WIN64
	return NULL;
#endif

	TCHAR dir[MAX_PATH];
	int dirlen = GetModuleFileName(GetDLLInstance(), dir, MAX_PATH);
	LPTSTR lpfilename=dir+dirlen;
	while (lpfilename>dir && *lpfilename!=_T('\\') && *lpfilename!=_T('/')) --lpfilename;
	*lpfilename = 0;
	dirlen = wcslen(dir);

	LPWSTR pEnvW = NULL;
	if (lpEnvironment) {
		if (dwCreationFlags & CREATE_UNICODE_ENVIRONMENT) {
			pEnvW = strdupdb((LPCWSTR)lpEnvironment, MAX_PATH + 1);
		} else {
			int alen = strlendb((LPCSTR)lpEnvironment);
			int wlen = MultiByteToWideChar(CP_ACP, 0, (LPCSTR)lpEnvironment, alen, NULL, 0) + 1;
			pEnvW = (LPWSTR)calloc(sizeof(WCHAR), wlen + MAX_PATH + 1);
			if (pEnvW) {
				MultiByteToWideChar(CP_ACP, 0, (LPCSTR)lpEnvironment, alen, pEnvW, wlen);
			}
		}
	} else {
		LPWSTR block = (LPWSTR)GetEnvironmentStringsW();
		if (block) {
			pEnvW = strdupdb(block, MAX_PATH + 1);
			FreeEnvironmentStrings(block);
		}
	}

	if (!pEnvW) {
		return NULL;
	}

	CArray<LPWSTR> envs;
	bool ret = MultiSzToArray(pEnvW, envs);
	free(pEnvW);
	pEnvW = NULL;
	
	/*if (ret) {
		ret = AddPathEnv(envs, dir, dirlen);
	}*/
#ifdef _WIN64
	{
		GetEnvironmentVariableW(L"MACTYPE_X64ADDR", NULL, 0);
		if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
			ret = AddX64Env(envs);
		}
	}
#endif
	if (ret) {
		pEnvW = ArrayToMultiSz(envs);
	}

	for (int i=0; i<envs.GetSize(); free(envs[i++]));

	if (!pEnvW) {
		return NULL;
	}

#ifdef _DEBUG
	{
		LPWSTR tmp = strdupdb(pEnvW, 0);
		LPWSTR tmpe = tmp + strlendb(tmp);
		PathRemoveFileSpec(dir);
		for (LPWSTR z=tmp; z<tmpe; z++)if(!*z)*z=L'\n';
			StringCchCatW(dir,MAX_PATH,L"\\");
			StringCchCatW(dir,MAX_PATH,L"gdienv.txt");
			HANDLE hf = CreateFileW(dir,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
			if(hf) {
			DWORD cb;
			WORD w = 0xfeff;
			WriteFile(hf,&w, sizeof(WORD), &cb, 0);
			WriteFile(hf,tmp, sizeof(WCHAR) * (tmpe - tmp), &cb, 0);
			SetEndOfFile(hf);
			CloseHandle(hf);
			free(tmp);
		}
	}
#endif

	dwCreationFlags |= CREATE_UNICODE_ENVIRONMENT;
	return pEnvW;
}

void DebugOut(const WCHAR* szFormat, ...) {
#ifdef TRACE
	va_list args;
	va_start(args, szFormat);
	WCHAR buffer[1024] = { 0 };
	vswprintf(buffer, szFormat, args);
	std::wstring fullmsg = L"[MTCore] " + std::wstring(buffer);
	OutputDebugString(fullmsg.c_str());
#endif
}

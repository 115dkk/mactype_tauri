#pragma once

#include <mmsystem.h>	//mmioFOURCC
#include "renderer_raii.h"
#define FOURCC_GDIPP	mmioFOURCC('G', 'D', 'I', 'P')

typedef struct {
	int dummy;
	FOURCC magic;
//	BYTE reserved[256];
} GDIPP_CREATE_MAGIC;

//参照
//http://www.catch22.net/tuts/undoc01.asp

#ifdef _GDIPP_EXE
template <typename _STARTUPINFO>
void FillGdiPPStartupInfo(_STARTUPINFO& si, GDIPP_CREATE_MAGIC& gppcm)
{
	ZeroMemory(&gppcm, sizeof(GDIPP_CREATE_MAGIC));
	gppcm.magic = FOURCC_GDIPP;
	si.cbReserved2 = sizeof(GDIPP_CREATE_MAGIC);
	si.lpReserved2 = reinterpret_cast<LPBYTE>(&gppcm);
}
#endif

#ifdef _GDIPP_DLL
template <typename _STARTUPINFO>
bool IsGdiPPStartupInfo(const _STARTUPINFO& si)
{
	if(si.cbReserved2 >= sizeof(int) + sizeof(FOURCC)) {
		GDIPP_CREATE_MAGIC* pMagic = reinterpret_cast<GDIPP_CREATE_MAGIC*>(si.lpReserved2);
		if (pMagic->dummy == 0 && pMagic->magic == FOURCC_GDIPP) {
			return true;
		}
	}
	return false;
}
#endif

EXTERN_C BOOL WINAPI GdippInjectDLL(const PROCESS_INFORMATION* ppi);
EXTERN_C LPWSTR WINAPI GdippEnvironment(DWORD& dwCreationFlags, LPVOID lpEnvironment);



//子プロセスにも自動でgdi++適用
template <typename _TCHAR, typename _STARTUPINFO, class _Function>
BOOL _CreateProcessAorW(const _TCHAR* lpApp, _TCHAR* lpCmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL bInherit, DWORD dwFlags, LPVOID lpEnv, const _TCHAR* lpDir, _STARTUPINFO* psi, LPPROCESS_INFORMATION ppi, _Function fn)
{
#ifdef _GDIPP_RUN_CPP
	const bool hookCP = true;
	const bool runGdi = true;
#else
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	const bool hookCP = pSettings->HookChildProcesses();
	const bool runGdi = pSettings->RunFromGdiExe();
#endif

	if (!hookCP || (!lpApp && !lpCmd) || (dwFlags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS | DETACHED_PROCESS))/* || !psi || psi->cb < sizeof(_STARTUPINFO)*/) {
		return fn(lpApp, lpCmd, pa, ta, bInherit, dwFlags, lpEnv, lpDir, psi, ppi);
	}

	PROCESS_INFORMATION _pi = { 0 };
	if (!ppi) {
		ppi = &_pi;
	}

	int szpsi = psi ? (psi->cb ? psi->cb: sizeof(_STARTUPINFO)) : sizeof(_STARTUPINFO);
	_STARTUPINFO& si = *static_cast<_STARTUPINFO*>(_alloca(szpsi));
	memset(&si, 0, sizeof(si));
	si.cb=szpsi;
	if (psi && psi->cb)
		memcpy(&si, psi, psi->cb);

	GDIPP_CREATE_MAGIC gppcm;
	if (runGdi && !si.cbReserved2) {
		FillGdiPPStartupInfo(si, gppcm);
	}

	renderer_raii::UniqueMallocMemory<WCHAR> environment(GdippEnvironment(dwFlags, lpEnv));
	if (environment) {
		lpEnv = environment.get();
	}

	if (!fn(lpApp, lpCmd, pa, ta, bInherit, dwFlags | CREATE_SUSPENDED, lpEnv, lpDir, &si, ppi)) {
		ZeroMemory(ppi, sizeof(*ppi));
		return FALSE;
	}

	GdippInjectDLL(ppi);
	if (!(dwFlags & CREATE_SUSPENDED)) {
		ResumeThread(ppi->hThread);
	}
	return TRUE;
}

template <typename _TCHAR, typename _STARTUPINFO, class _Function>
BOOL _CreateProcessAsUserAorW(HANDLE hToken, const _TCHAR* lpApp, _TCHAR* lpCmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL bInherit, DWORD dwFlags, LPVOID lpEnv, const _TCHAR* lpDir, _STARTUPINFO* psi, LPPROCESS_INFORMATION ppi, _Function fn)
{
#ifdef _GDIPP_RUN_CPP
	const bool hookCP = true;
	const bool runGdi = true;
#else
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	const bool hookCP = pSettings->HookChildProcesses();
	const bool runGdi = pSettings->RunFromGdiExe();
#endif

	if (!hookCP || (!lpApp && !lpCmd) || (dwFlags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS))/* || !psi || psi->cb < sizeof(_STARTUPINFO)*/) {
		return fn(hToken, lpApp, lpCmd, pa, ta, bInherit, dwFlags, lpEnv, lpDir, psi, ppi);
	}

	PROCESS_INFORMATION _pi = { 0 };
	if (!ppi) {
		ppi = &_pi;
	}
	int szpsi = psi ? (psi->cb ? psi->cb: sizeof(_STARTUPINFO)) : sizeof(_STARTUPINFO);
	_STARTUPINFO& si = *static_cast<_STARTUPINFO*>(_alloca(szpsi));
	memset(&si, 0, sizeof(si));
	si.cb=szpsi;
	if (psi && psi->cb)
		memcpy(&si, psi, psi->cb);

	GDIPP_CREATE_MAGIC gppcm;
	if (runGdi && !si.cbReserved2) {
		FillGdiPPStartupInfo(si, gppcm);
	}

	renderer_raii::UniqueMallocMemory<WCHAR> environment(GdippEnvironment(dwFlags, lpEnv));
	if (environment) {
		lpEnv = environment.get();
	}

	if (!fn(hToken, lpApp, lpCmd, pa, ta, bInherit, dwFlags | CREATE_SUSPENDED, lpEnv, lpDir, &si, ppi)) {
		ZeroMemory(ppi, sizeof(*ppi));
		return FALSE;
	}

	GdippInjectDLL(ppi);
	if (!(dwFlags & CREATE_SUSPENDED)) {
		ResumeThread(ppi->hThread);
	}
	return TRUE;
}

static wstring GetExeName(LPCTSTR lpApp, LPTSTR lpCmd)
{
	wstring ret;
	LPTSTR vlpApp = const_cast<LPTSTR>(lpApp);	//变成可以操作的参数
	if (lpApp)
	{
		do
		{
			vlpApp = _tcsstr(vlpApp+1, _T(" "));	//获得第一个空格所在的位置
			ret.assign(lpApp);
			if (vlpApp)
				ret.resize(vlpApp-lpApp);
			DWORD fa = GetFileAttributes(ret.c_str());
			if (fa!=INVALID_FILE_ATTRIBUTES && fa!=FILE_ATTRIBUTE_DIRECTORY)	//文件是否存在
			{
				int p = ret.find_last_of(_T("\\"));
				if (p!=-1)
					ret.erase(0, p+1);	//如果有路径就删掉路径
				return ret;
			}
			else
			{
				ret+=_T(".exe");	//加上.exe扩展名再试
				DWORD fa = GetFileAttributes(ret.c_str());
				if (fa!=INVALID_FILE_ATTRIBUTES && fa!=FILE_ATTRIBUTE_DIRECTORY)
				{
					int p = ret.find_last_of(_T("\\"));
					if (p!=-1)
						ret.erase(0, p+1);	//如果有路径就删掉路径
					return ret;
				}
			}
		} while (vlpApp);
	}

	if (lpCmd)
	{
		ret.assign(lpCmd);
		int p=0;
		if ((*lpCmd)==_T('\"'))
		{
			ret.erase(0,1);	//删除第一个引号
			p=ret.find_first_of(_T("\""));	//查找下一个引号
		}
		else
			p=ret.find_first_of(_T(" "));
		if (p>0)
			ret.resize(p);	//获得Cmd里面的文件名
		p = ret.find_last_of(_T("\\"));
		if (p>0)
			ret.erase(0, p+1);	//如果有路径就删掉路径
		return ret;
	}
	return ret;
}

template <class _Function>
BOOL _CreateProcessInternalW(HANDLE hToken, LPCTSTR lpApp, LPTSTR lpCmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL bInherit, \
							 DWORD dwFlags, LPVOID lpEnv, LPCTSTR lpDir, LPSTARTUPINFO psi, LPPROCESS_INFORMATION ppi , PHANDLE hNewToken, _Function fn)
{
#ifdef _GDIPP_RUN_CPP
	const bool hookCP = true;
	const bool runGdi = true;
#else
	const CGdippSettings* pSettings = CGdippSettings::GetInstanceNoInit();
	const bool hookCP = pSettings->HookChildProcesses();
	const bool runGdi = pSettings->RunFromGdiExe();
#endif

#ifdef _GDIPP_EXE
	if (!hookCP || (!lpApp && !lpCmd) || (dwFlags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS))	|| !psi || psi->cb < sizeof(STARTUPINFO)) {
			return fn(hToken, lpApp, lpCmd, pa, ta, bInherit, dwFlags, lpEnv, lpDir, psi, ppi, hNewToken);
	}

	STARTUPINFO& si = *static_cast<STARTUPINFO*>(_alloca(psi->cb));
	memcpy(&si, psi, psi->cb);
	psi = &si;

	GDIPP_CREATE_MAGIC gppcm;
	if (runGdi && !si.cbReserved2) {
		FillGdiPPStartupInfo(si, gppcm);
	}

	PROCESS_INFORMATION _pi = { 0 };
	if (!ppi) {
		ppi = &_pi;
	}

	renderer_raii::UniqueMallocMemory<WCHAR> environment(GdippEnvironment(dwFlags, lpEnv));
	if (environment) {
		lpEnv = environment.get();
	}
	if (!fn(hToken, lpApp, lpCmd, pa, ta, bInherit, dwFlags | CREATE_SUSPENDED, lpEnv, lpDir, psi, ppi, hNewToken)) {
		ZeroMemory(ppi, sizeof(*ppi));
		return FALSE;
	}
	GdippInjectDLL(ppi);
	if (!(dwFlags & CREATE_SUSPENDED)) {
		ResumeThread(ppi->hThread);
	}
#else
	wstring exe_name = GetExeName(lpApp, lpCmd);
	if (!hookCP || (!lpApp && !lpCmd) || (dwFlags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS)) || IsExeUnload(exe_name.c_str())) {
			return fn(hToken, lpApp, lpCmd, pa, ta, bInherit, dwFlags, lpEnv, lpDir, psi, ppi, hNewToken);
	}
	renderer_raii::UniqueMallocMemory<WCHAR> environment(GdippEnvironment(dwFlags, lpEnv));
	if (environment) {
		lpEnv = environment.get();
	}
	if (!fn(hToken, lpApp, lpCmd, pa, ta, bInherit, dwFlags | CREATE_SUSPENDED, lpEnv, lpDir, psi, ppi, hNewToken)) {
		return FALSE;
	}
	GdippInjectDLL(ppi);
	if (!(dwFlags & CREATE_SUSPENDED)) {
		ResumeThread(ppi->hThread);
	}
#endif

	return TRUE;
}

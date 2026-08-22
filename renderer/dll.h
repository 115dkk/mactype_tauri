#include <windows.h>
#include "renderer_raii.h"

typedef   BOOL (__stdcall *ProcDllMain)(HINSTANCE, DWORD,  LPVOID );

class CMemLoadDll
{
public:
	CMemLoadDll();
	~CMemLoadDll();
	BOOL    MemLoadLibrary(void* lpFileData, int DataLength, bool bInitDllMain);  // Dll file data buffer
	FARPROC MemGetProcAddress(LPCSTR lpProcName);
	DWORD_PTR	GetImageBase() {return pImageBase;};
private:
	BOOL isLoadOk;
	BOOL CheckDataValide(void* lpFileData, int DataLength);
	int  CalcTotalImageSize();
	void CopyDllDatas(void* pDest, void* pSrc);
	static int GetAlignedSize(int Origin, int Alignment);
private:
	ProcDllMain pDllMain;


private:
	DWORD_PTR  pImageBase;
	renderer_raii::UniqueVirtualMemory m_image;
	bool   m_bInitDllMain;
	PIMAGE_DOS_HEADER pDosHeader;
	PIMAGE_NT_HEADERS32 pNTHeader;
	PIMAGE_SECTION_HEADER pSectionHeader;
};

class CDllHelper {
private:
	static int StringLengthA(const char* str);
	static wchar_t* CharToWChar_T(const char* str);
	static wchar_t ToLowerW(wchar_t ch);
	static bool StringMatches(const wchar_t* str1, const wchar_t* str2);
public:
	static const void* MyGetProcAddress(HMODULE dllBase, const wchar_t* procName);
};

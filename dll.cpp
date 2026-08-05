#include "dll.h"

CMemLoadDll::CMemLoadDll()
	: isLoadOk(FALSE), pDllMain(nullptr), pImageBase(0), m_bInitDllMain(true),
	  pDosHeader(nullptr), pNTHeader(nullptr), pSectionHeader(nullptr)
{}
CMemLoadDll::~CMemLoadDll()
{
 if(isLoadOk)
 {
  //ASSERT(pImageBase != NULL);
  //ASSERT(pDllMain   != NULL);
  //脱钩，准备卸载dll
  if (m_bInitDllMain)
	 pDllMain(reinterpret_cast<HINSTANCE>(pImageBase),DLL_PROCESS_DETACH,0);
  VirtualFree(reinterpret_cast<LPVOID>(pImageBase), 0, MEM_RELEASE);
 }
}

//MemLoadLibrary函数从内存缓冲区数据中加载一个dll到当前进程的地址空间，缺省位置0x10000000
//返回值： 成功返回TRUE , 失败返回FALSE
//lpFileData: 存放dll文件数据的缓冲区
//DataLength: 缓冲区中数据的总长度
BOOL CMemLoadDll::MemLoadLibrary(void* lpFileData, int DataLength, bool bInitDllMain)
{
 this->m_bInitDllMain = bInitDllMain;
 if(pImageBase != NULL)
 {
  return FALSE;  //已经加载一个dll，还没有释放，不能加载新的dll
 }
 //检查数据有效性，并初始化
 if(!CheckDataValide(lpFileData, DataLength))return FALSE;
 //计算所需的加载空间
 int ImageSize = CalcTotalImageSize();
 if(ImageSize == 0) return FALSE;

 // 分配虚拟内存
 void *pMemoryAddress = VirtualAlloc(nullptr, ImageSize,
     MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
 if(pMemoryAddress == NULL) return FALSE;
 else
 {
  CopyDllDatas(pMemoryAddress, lpFileData); //复制dll数据，并对齐每个段
  //修改页属性。应该根据每个页的属性单独设置其对应内存页的属性。这里简化一下。
  //统一设置成一个属性PAGE_EXECUTE_READWRITE
  unsigned long old;
  VirtualProtect(pMemoryAddress, ImageSize, PAGE_EXECUTE_READWRITE,&old);
 }
 //修正基地址
 pNTHeader->OptionalHeader.ImageBase = static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(pMemoryAddress));

 //接下来要调用一下dll的入口函数，做初始化工作。
 pDllMain = reinterpret_cast<ProcDllMain>(pNTHeader->OptionalHeader.AddressOfEntryPoint + reinterpret_cast<DWORD_PTR>(pMemoryAddress));
 BOOL InitResult = !bInitDllMain || pDllMain(reinterpret_cast<HINSTANCE>(pMemoryAddress),DLL_PROCESS_ATTACH,0);
 if(!InitResult) //初始化失败
 {
  pDllMain(reinterpret_cast<HINSTANCE>(pMemoryAddress),DLL_PROCESS_DETACH,0);
  VirtualFree(pMemoryAddress,0,MEM_RELEASE);
  pDllMain = NULL;
  return FALSE;
 }

 isLoadOk = TRUE;
 pImageBase = reinterpret_cast<DWORD_PTR>(pMemoryAddress);
 return TRUE;
}

//MemGetProcAddress函数从dll中获取指定函数的地址
//返回值： 成功返回函数地址 , 失败返回NULL
//lpProcName: 要查找函数的名字或者序号
FARPROC  CMemLoadDll::MemGetProcAddress(LPCSTR lpProcName)
{
 if(pNTHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress == 0 ||
  pNTHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size == 0)
  return NULL;
 if(!isLoadOk) return NULL;

 DWORD OffsetStart = pNTHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
 DWORD Size = pNTHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;

 PIMAGE_EXPORT_DIRECTORY pExport = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(pImageBase + pNTHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
 const DWORD iNumberOfFunctions = pExport->NumberOfFunctions;
 const DWORD* pAddressOfFunctions = reinterpret_cast<const DWORD*>(pExport->AddressOfFunctions + pImageBase);
 const WORD* pAddressOfOrdinals = reinterpret_cast<const WORD*>(pExport->AddressOfNameOrdinals + pImageBase);
 const DWORD* pAddressOfNames = reinterpret_cast<const DWORD*>(pExport->AddressOfNames + pImageBase);

 int iOrdinal = -1;

 const DWORD_PTR procNameValue = reinterpret_cast<DWORD_PTR>(lpProcName);
 if((procNameValue & 0xFFFF0000) == 0) //IT IS A ORDINAL!
 {
  iOrdinal = static_cast<int>(procNameValue & 0x0000FFFF) - static_cast<int>(pExport->Base);
 }
 else  //use name
 {
  int iFound = -1;

  for(DWORD i=0;i<pExport->NumberOfNames;i++)
  {
   const char* pName = reinterpret_cast<const char*>(pAddressOfNames[i] + pImageBase);
   if(strcmp(pName, lpProcName) == 0)
   {
    iFound = i; break;
   }
  }
  if(iFound >= 0)
  {
    iOrdinal = static_cast<int>(pAddressOfOrdinals[iFound]);
  }
 }

 if(iOrdinal < 0 || static_cast<DWORD>(iOrdinal) >= iNumberOfFunctions) return NULL;
 else
 {
  DWORD pFunctionOffset = pAddressOfFunctions[iOrdinal];
  if(pFunctionOffset > OffsetStart && pFunctionOffset < (OffsetStart+Size))//maybe Export Forwarding
   return NULL;
   else return reinterpret_cast<FARPROC>(pFunctionOffset + pImageBase);
 }

}


//CheckDataValide函数用于检查缓冲区中的数据是否有效的dll文件
//返回值： 是一个可执行的dll则返回TRUE，否则返回FALSE。
//lpFileData: 存放dll数据的内存缓冲区
//DataLength: dll文件的长度
BOOL CMemLoadDll::CheckDataValide(void* lpFileData, int DataLength)
{
 //检查长度
 if(DataLength < sizeof(IMAGE_DOS_HEADER)) return FALSE;
 pDosHeader = static_cast<PIMAGE_DOS_HEADER>(lpFileData);  // DOSͷ
 //检查dos头的标记
 if(pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;  //0x5A4D : MZ

 //检查长度
 if(static_cast<DWORD>(DataLength) < (pDosHeader->e_lfanew + sizeof(IMAGE_NT_HEADERS32)) ) return FALSE;
 //取得pe头
 pNTHeader = reinterpret_cast<PIMAGE_NT_HEADERS32>(reinterpret_cast<DWORD_PTR>(lpFileData) + static_cast<DWORD_PTR>(pDosHeader->e_lfanew)); // PEͷ
 //检查pe头的合法性
 if(pNTHeader->Signature != IMAGE_NT_SIGNATURE) return FALSE;  //0x00004550 : PE00
 if((pNTHeader->FileHeader.Characteristics & IMAGE_FILE_DLL) == 0) //0x2000  : File is a DLL
  return FALSE; 
 if((pNTHeader->FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0) //0x0002 : 指出文件可以运行
  return FALSE;
 if(pNTHeader->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER32)) return FALSE;

 
 //取得节表（段表）
 pSectionHeader = reinterpret_cast<PIMAGE_SECTION_HEADER>(reinterpret_cast<DWORD_PTR>(pNTHeader) + sizeof(IMAGE_NT_HEADERS32));
 //验证每个节表的空间
 for(int i=0; i< pNTHeader->FileHeader.NumberOfSections; i++)
 {
  if((pSectionHeader[i].PointerToRawData + pSectionHeader[i].SizeOfRawData) > static_cast<DWORD>(DataLength))return FALSE;
 }
 return TRUE;
}

//计算对齐边界
int CMemLoadDll::GetAlignedSize(int Origin, int Alignment)
{
 return (Origin + Alignment - 1) / Alignment * Alignment;
}
//计算整个dll映像文件的尺寸
int CMemLoadDll::CalcTotalImageSize()
{
 int Size;
 if(pNTHeader == NULL)return 0;
 int nAlign = pNTHeader->OptionalHeader.SectionAlignment; //段对齐字节数

 // 计算所有头的尺寸。包括dos, coff, pe头 和 段表的大小
 Size = GetAlignedSize(pNTHeader->OptionalHeader.SizeOfHeaders, nAlign);
 // 计算所有节的大小
 for(int i=0; i < pNTHeader->FileHeader.NumberOfSections; ++i)
 {
  //得到该节的大小
  int CodeSize = pSectionHeader[i].Misc.VirtualSize ;
  int LoadSize = pSectionHeader[i].SizeOfRawData;
  int MaxSize = (LoadSize > CodeSize)?(LoadSize):(CodeSize);

  int SectionSize = GetAlignedSize(pSectionHeader[i].VirtualAddress + MaxSize, nAlign);
  if(Size < SectionSize)
   Size = SectionSize;  //Use the Max;
 }
 return Size;
}
//CopyDllDatas函数将dll数据复制到指定内存区域，并对齐所有节
//pSrc: 存放dll数据的原始缓冲区
//pDest:目标内存地址
void CMemLoadDll::CopyDllDatas(void* pDest, void* pSrc)
{
 // 计算需要复制的PE头+段表字节数
 int  HeaderSize = pNTHeader->OptionalHeader.SizeOfHeaders;
 int  SectionSize = pNTHeader->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
 int  MoveSize = HeaderSize + SectionSize;
 //复制头和段信息
 memmove(pDest, pSrc, MoveSize);

 //复制每个节
 for(int i=0; i < pNTHeader->FileHeader.NumberOfSections; ++i)
 {
  if(pSectionHeader[i].VirtualAddress == 0 || pSectionHeader[i].SizeOfRawData == 0)continue;
  // 定位该节在内存中的位置
  void *pSectionAddress = reinterpret_cast<void*>(reinterpret_cast<DWORD_PTR>(pDest) + pSectionHeader[i].VirtualAddress);
  // 复制段数据到虚拟内存
  memmove(pSectionAddress,
       reinterpret_cast<void*>(reinterpret_cast<DWORD_PTR>(pSrc) + pSectionHeader[i].PointerToRawData),
    pSectionHeader[i].SizeOfRawData);
 }

 //修正指针，指向新分配的内存
 //新的dos头
 pDosHeader = static_cast<PIMAGE_DOS_HEADER>(pDest);
 //新的pe头地址
 pNTHeader = reinterpret_cast<PIMAGE_NT_HEADERS32>(reinterpret_cast<DWORD_PTR>(pDest) + static_cast<DWORD_PTR>(pDosHeader->e_lfanew));
 //新的节表地址
 pSectionHeader = reinterpret_cast<PIMAGE_SECTION_HEADER>(reinterpret_cast<DWORD_PTR>(pNTHeader) + sizeof(IMAGE_NT_HEADERS32));
 return ;
}




// =========== Dll helper to treat dlls our own way ================
/*
* StringLengthA
*
* Use: Retrieve length of char string
* Parameters: char string
* Return: Length of string
*/
int CDllHelper::StringLengthA(const char* str) {
	int length;

	for (length = 0; str[length] != '\0'; length++) {}
	return length;
}
/*
* CharToWChar_T
*
* Use: Convert char string to wchar_t string - caller responsible for freeing memory
* Parameters: char string
* Return: wchar_t string
*/
wchar_t* CDllHelper::CharToWChar_T(const char* str) {
	if (str == nullptr) {
		return nullptr;
	}

	const int length = StringLengthA(str);
	wchar_t* wstr_t = static_cast<wchar_t*>(malloc(sizeof(wchar_t) * (length + 1)));
	if (wstr_t == nullptr) {
		return nullptr;
	}

	for (int i = 0; i < length; i++) {
		wstr_t[i] = str[i];
	}
	wstr_t[length] = '\0';
	return wstr_t;
}
/*
* ToLowerW
*
* Use: Convert char to lower case if necessary
* Parameters: char
* Return: char
*/
wchar_t CDllHelper::ToLowerW(wchar_t ch) {
	if (ch > 0x40 && ch < 0x5B) {
		return ch + 0x20;
	}
	return ch;
}
/*
* StringMatches
*
* Use: Case Insensitive String Compare
* Parameters: two wchar_t strings
* Return: Result of wchar_t equality
*/
bool CDllHelper::StringMatches(const wchar_t* str1, const wchar_t* str2) {
	if (str1 == nullptr || str2 == nullptr || wcslen(str1) != wcslen(str2)) {
		return false;
	}

	for (int i = 0; str1[i] != '\0' && str2[i] != '\0'; i++) {
		if (ToLowerW(str1[i]) != ToLowerW(str2[i])) {
			return false;
		}
	}
	return true;
}

/*
* GetProcAddress
*
* Use: Independent GetProcAddress using TIB/PEB/LDR
* Parameters: wchar_t string with DLL Name, wchar_t string with Function Name
* Return: void* to Function - nullptr if not found
*/
const void* CDllHelper::MyGetProcAddress(HMODULE dllBase, const wchar_t* procName) {
	const void* procAddr = nullptr;

	//DllBase as unsigned long for arithmetic
	const DWORD_PTR dllBaseAddr = reinterpret_cast<DWORD_PTR>(dllBase);

	//Cast DllBase to use struct
	const PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(dllBaseAddr);

	//Calculate NTHeader and Cast
	const PIMAGE_NT_HEADERS64 pNtHeader = reinterpret_cast<PIMAGE_NT_HEADERS64>(dllBaseAddr + dosHeader->e_lfanew);

	//Calculate ExportDir Address and Cast
	const PIMAGE_EXPORT_DIRECTORY pExportDir = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(dllBaseAddr + pNtHeader->OptionalHeader.DataDirectory[0].VirtualAddress);

	//Calculate AddressOfNames Absolute and Cast
	const unsigned int* NameRVA = reinterpret_cast<const unsigned int*>(dllBaseAddr + pExportDir->AddressOfNames);

	//Iterate over AddressOfNames
	for (DWORD i = 0; i < pExportDir->NumberOfNames; i++) {
		//Calculate Absolute Address and cast
		const char* name = reinterpret_cast<const char*>(dllBaseAddr + NameRVA[i]);
		wchar_t* wname = CharToWChar_T(name);
		if (wname == nullptr) {
			return nullptr;
		}
		const bool matches = StringMatches(wname, procName);
		free(wname);
		if (matches) {

			//Lookup Ordinal
			const unsigned short NameOrdinal = reinterpret_cast<const unsigned short*>(dllBaseAddr + pExportDir->AddressOfNameOrdinals)[i];

			//Use Ordinal to Lookup Function Address and Calculate Absolute
			const unsigned int* functionAddresses = reinterpret_cast<const unsigned int*>(dllBaseAddr + pExportDir->AddressOfFunctions);
			const unsigned int addr = functionAddresses[NameOrdinal];
			const void* paddr = &functionAddresses[NameOrdinal];

			//Function is forwarded
			if (addr > pNtHeader->OptionalHeader.DataDirectory[0].VirtualAddress && addr < pNtHeader->OptionalHeader.DataDirectory[0].VirtualAddress + pNtHeader->OptionalHeader.DataDirectory[0].Size) {
				//Grab and Parse Forward String
				continue;
				/*char* forwardStr = (char*)(dllBaseAddr + addr);

				wchar_t** str_arr = ParseForwardString(forwardStr);

				//Attempt to load library if not loaded
				if (!LibraryLoaded(str_arr[0])) {
				void* dllBase = _LoadLibraryW(str_arr[0]);
				}

				//Recurse using forward information
				procAddr = GetProcAddress(str_arr[0], str_arr[1]);
				free(str_arr[0]);
				free(str_arr[1]);
				free(str_arr);*/
			}
			else {
				// but in some cases, the IAT can be forged, meaning that the IAT we got here is not the real one in the file.
				// hence, we only pass the offset of IAT to the shellcode, and let shellcode search for its IAT and caculate the function address there.
				procAddr = paddr; // the offset to the func IAT, it's a DWORD, you get real address by (imgbase + *(dword*)(imgbase+paddr))
			}
			break;
		}
	}
	return procAddr;
}

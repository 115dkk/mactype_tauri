#pragma once

#include "malloc.h"
#include "string.h"
#include "windows.h"
#include <map>
#include <string>

using strmap = std::map<std::wstring, LPTSTR>;

class CHashedStringList
{
public:
	void Add(const TCHAR* String, const TCHAR* Value);
	void Delete(const TCHAR* String);
	TCHAR* Find(const TCHAR* String);
	CHashedStringList() : m_bCaseSense(false){}
	explicit CHashedStringList(BOOL bCaseSensative) : m_bCaseSense(bCaseSensative){}
	~CHashedStringList(){
		strmap::iterator it = stringmap.begin();
		while (it != stringmap.end()) {
			free(it->second);
			++it;
		}
	}
protected:
private:
	strmap stringmap;
	BOOL m_bCaseSense;
};

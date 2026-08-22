#include "hash_list.h"
#include <cwctype>
#include <algorithm>

void CHashedStringList::Add(const TCHAR* String, const TCHAR* Value)
{
	std::wstring buff = String;
	if (!m_bCaseSense)
		std::transform(buff.begin(), buff.end(), buff.begin(), ::towlower);

	strmap::iterator it = stringmap.find(buff);
	if (it == stringmap.end()) {
		stringmap[buff] = _wcsdup(Value);
	}
}

void CHashedStringList::Delete(const TCHAR* String)
{
	std::wstring buff = String;
	if (!m_bCaseSense)
		std::transform(buff.begin(), buff.end(), buff.begin(), ::towlower);
	stringmap.erase(buff);
}

TCHAR* CHashedStringList::Find(const TCHAR* String)
{
	std::wstring buff = String;
	if (!m_bCaseSense)
		std::transform(buff.begin(), buff.end(), buff.begin(), ::towlower);
	strmap::iterator it = stringmap.find(buff);
	if (it != stringmap.end())
		return it->second;
	else
		return nullptr;
}

#include <windows.h>

#include <dia2.h>
#include <diacreate.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

template <typename Interface>
class ComOwner final
{
public:
	ComOwner() = default;
	~ComOwner()
	{
		if (value_ != nullptr)
			value_->Release();
	}

	ComOwner(const ComOwner&) = delete;
	ComOwner& operator=(const ComOwner&) = delete;

	Interface** put() noexcept { return &value_; }
	Interface* get() const noexcept { return value_; }
	Interface* operator->() const noexcept { return value_; }

private:
	Interface* value_ = nullptr;
};

class BstrOwner final
{
public:
	~BstrOwner()
	{
		if (value_ != nullptr)
			SysFreeString(value_);
	}

	BSTR* put() noexcept { return &value_; }
	const wchar_t* get() const noexcept
	{
		return value_ != nullptr ? value_ : L"";
	}

private:
	BSTR value_ = nullptr;
};

struct SymbolRecord final
{
	std::wstring name;
	DWORD rva = 0;
	DWORD tag = 0;
};

bool ContainsInsensitive(
	const std::wstring& value,
	const std::wstring& needle);

void PrintTypeChildren(IDiaSymbol* type)
{
	ComOwner<IDiaEnumSymbols> children;
	if (FAILED(type->findChildren(
			SymTagNull, nullptr, nsNone, children.put())))
		return;
	for (;;)
	{
		ComOwner<IDiaSymbol> child;
		ULONG fetched = 0;
		if (children->Next(1, child.put(), &fetched) != S_OK || fetched != 1)
			break;
		DWORD tag = 0;
		if (FAILED(child->get_symTag(&tag)) ||
			(tag != SymTagData && tag != SymTagBaseClass))
			continue;
		BstrOwner name;
		child->get_name(name.put());
		LONG offset = 0;
		HRESULT const offsetStatus = child->get_offset(&offset);
		ComOwner<IDiaSymbol> childType;
		DWORD childTypeTag = 0;
		ULONGLONG childTypeLength = 0;
		BstrOwner childTypeName;
		if (SUCCEEDED(child->get_type(childType.put())) && childType.get() != nullptr)
		{
			childType->get_symTag(&childTypeTag);
			childType->get_length(&childTypeLength);
			childType->get_name(childTypeName.put());
		}
		std::wcout << L"  tag=" << tag << L" offset=";
		if (SUCCEEDED(offsetStatus))
			std::wcout << offset;
		else
			std::wcout << L"?";
		std::wcout << L" size=" << childTypeLength
			<< L" type-tag=" << childTypeTag << L" name=" << name.get();
		if (*childTypeName.get() != L'\0')
			std::wcout << L" type=" << childTypeName.get();
		std::wcout << L"\n";
	}
}

int DumpTypes(IDiaSymbol* global, const std::wstring& needle)
{
	ComOwner<IDiaEnumSymbols> types;
	if (FAILED(global->findChildren(
			SymTagUDT, nullptr, nsNone, types.put())))
		return 1;
	int matches = 0;
	for (;;)
	{
		ComOwner<IDiaSymbol> type;
		ULONG fetched = 0;
		if (types->Next(1, type.put(), &fetched) != S_OK || fetched != 1)
			break;
		BstrOwner name;
		if (FAILED(type->get_name(name.put())) ||
			!ContainsInsensitive(name.get(), needle))
			continue;
		ULONGLONG length = 0;
		type->get_length(&length);
		std::wcout << name.get() << L" size=" << length << L"\n";
		PrintTypeChildren(type.get());
		++matches;
	}
	return matches == 0 ? 3 : 0;
}

bool ContainsInsensitive(
	const std::wstring& value,
	const std::wstring& needle)
{
	if (needle.empty())
		return true;
	return std::search(
		value.begin(), value.end(), needle.begin(), needle.end(),
		[](wchar_t left, wchar_t right) {
			return towlower(left) == towlower(right);
		}) != value.end();
}

void CollectSymbols(
	IDiaSymbol* global,
	DWORD tag,
	const std::wstring& needle,
	std::vector<SymbolRecord>& records)
{
	ComOwner<IDiaEnumSymbols> symbols;
	if (FAILED(global->findChildren(
			static_cast<enum SymTagEnum>(tag), nullptr, nsNone, symbols.put())))
		return;

	for (;;)
	{
		ComOwner<IDiaSymbol> symbol;
		ULONG fetched = 0;
		if (symbols->Next(1, symbol.put(), &fetched) != S_OK || fetched != 1)
			break;

		BstrOwner name;
		if (FAILED(symbol->get_name(name.put())))
			continue;
		const std::wstring value(name.get());
		if (!ContainsInsensitive(value, needle))
			continue;

		DWORD rva = 0;
		if (FAILED(symbol->get_relativeVirtualAddress(&rva)) || rva == 0)
			continue;
		records.push_back({value, rva, static_cast<DWORD>(tag)});
	}
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
	if (argc < 3 || argc > 5)
	{
		std::wcerr << L"usage: mactype-unity-symbol-probe <msdia.dll> <pdb> [needle]\n"
			L"       mactype-unity-symbol-probe <msdia.dll> <pdb> --type <needle>\n";
		return 2;
	}

	bool const typeMode = argc == 5 && wcscmp(argv[3], L"--type") == 0;
	if (argc == 5 && !typeMode)
		return 2;
	const std::wstring needle = typeMode
		? argv[4]
		: argc == 4 ? argv[3] : L"FT_";
	ComOwner<IDiaDataSource> source;
	HRESULT status = NoRegCoCreate(
		argv[1], CLSID_DiaSource, IID_IDiaDataSource,
		reinterpret_cast<void**>(source.put()));
	if (FAILED(status))
	{
		std::wcerr << L"DIA activation failed: 0x" << std::hex << status << L"\n";
		return 1;
	}
	status = source->loadDataFromPdb(argv[2]);
	if (FAILED(status))
	{
		std::wcerr << L"PDB load failed: 0x" << std::hex << status << L"\n";
		return 1;
	}

	ComOwner<IDiaSession> session;
	status = source->openSession(session.put());
	if (FAILED(status))
	{
		std::wcerr << L"DIA session failed: 0x" << std::hex << status << L"\n";
		return 1;
	}
	ComOwner<IDiaSymbol> global;
	status = session->get_globalScope(global.put());
	if (FAILED(status))
	{
		std::wcerr << L"global scope failed: 0x" << std::hex << status << L"\n";
		return 1;
	}
	if (typeMode)
		return DumpTypes(global.get(), needle);

	std::vector<SymbolRecord> records;
	CollectSymbols(global.get(), SymTagFunction, needle, records);
	CollectSymbols(global.get(), SymTagPublicSymbol, needle, records);
	std::sort(
		records.begin(), records.end(),
		[](const SymbolRecord& left, const SymbolRecord& right) {
			if (left.rva != right.rva)
				return left.rva < right.rva;
			return left.name < right.name;
		});
	records.erase(
		std::unique(
			records.begin(), records.end(),
			[](const SymbolRecord& left, const SymbolRecord& right) {
				return left.rva == right.rva && left.name == right.name;
			}),
		records.end());

	for (const SymbolRecord& record : records)
	{
		std::wcout << L"0x" << std::hex << std::setw(8) << std::setfill(L'0')
			<< record.rva << L"\t" << std::dec << record.tag << L"\t"
			<< record.name << L"\n";
	}
	return records.empty() ? 3 : 0;
}

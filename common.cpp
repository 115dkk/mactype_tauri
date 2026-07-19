#include "common.h"

std::wstring to_utf16le(const std::wstring& input) {
	std::wstring utf16_string;
	int len = input.length();
	char* content = (char*)input.c_str();
	utf16_string.reserve(len);
	for (size_t i = 0; i < len; i += 2) {
		char16_t code_unit = (static_cast<char16_t>(content[i]) << 8) |
			static_cast<char16_t>(content[i + 1]);
		utf16_string.push_back(code_unit);
	}
	return utf16_string;
}

std::wstring to_wide_string(const std::string& input)
{
	std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
	return converter.from_bytes(input);
}
// convert wstring to string 
std::string to_byte_string(const std::wstring& input)
{
	//std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
	std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
	return converter.to_bytes(input);
}

wstring to_lower_case(wstring str) {
	transform(str.begin(), str.end(), str.begin(), ::towlower);
	return str;
}

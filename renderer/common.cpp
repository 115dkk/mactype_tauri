#include "common.h"

#include <limits>
#include <stdexcept>

namespace {

int CheckedConversionLength(size_t length)
{
	if (length > static_cast<size_t>((std::numeric_limits<int>::max)())) {
		throw std::range_error("bad conversion");
	}
	return static_cast<int>(length);
}

} // namespace

std::wstring to_utf16le(const std::wstring& input) {
	std::wstring utf16_string;
	const size_t len = input.length();
	const auto* content = reinterpret_cast<const unsigned char*>(input.data());
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
	if (input.empty()) {
		return {};
	}
	const int inputLength = CheckedConversionLength(input.size());
	const int outputLength = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), inputLength, nullptr, 0);
	if (!outputLength) {
		throw std::range_error("bad conversion");
	}
	std::wstring output(static_cast<size_t>(outputLength), L'\0');
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), inputLength,
		output.data(), outputLength) != outputLength) {
		throw std::range_error("bad conversion");
	}
	return output;
}
std::string to_byte_string(const std::wstring& input)
{
	if (input.empty()) {
		return {};
	}
	const int inputLength = CheckedConversionLength(input.size());
	const int outputLength = WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), inputLength, nullptr, 0, nullptr, nullptr);
	if (!outputLength) {
		throw std::range_error("bad conversion");
	}
	std::string output(static_cast<size_t>(outputLength), '\0');
	if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), inputLength,
		output.data(), outputLength, nullptr, nullptr) != outputLength) {
		throw std::range_error("bad conversion");
	}
	return output;
}

wstring to_lower_case(wstring str) {
	transform(str.begin(), str.end(), str.begin(), ::tolower);
	return str;
}

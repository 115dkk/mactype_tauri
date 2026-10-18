#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace renderer::big_endian {

[[nodiscard]] constexpr bool CanRead(
	std::span<const unsigned char> bytes,
	std::size_t offset,
	std::size_t length) noexcept
{
	return offset <= bytes.size() && length <= bytes.size() - offset;
}

[[nodiscard]] constexpr std::optional<std::uint16_t> Read16(
	std::span<const unsigned char> bytes, std::size_t offset) noexcept
{
	if (!CanRead(bytes, offset, 2))
		return std::nullopt;
	return static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(bytes[offset]) << 8) |
		static_cast<std::uint16_t>(bytes[offset + 1]));
}

[[nodiscard]] constexpr std::optional<std::uint32_t> Read32(
	std::span<const unsigned char> bytes, std::size_t offset) noexcept
{
	if (!CanRead(bytes, offset, 4))
		return std::nullopt;
	return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
		(static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
		(static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
		static_cast<std::uint32_t>(bytes[offset + 3]);
}

} // namespace renderer::big_endian

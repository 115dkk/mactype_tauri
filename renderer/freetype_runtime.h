#pragma once

#include "renderer_policy.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace renderer {
namespace freetype {

constexpr unsigned long BoundedStreamReadSize(
	unsigned long size,
	unsigned long offset,
	unsigned long requested) noexcept
{
	if (offset >= size || requested == 0)
		return 0;
	unsigned long const remaining = size - offset;
	return requested < remaining ? requested : remaining;
}

constexpr int BitmapByteSize(int pitch, unsigned int rows) noexcept
{
	std::int64_t const signedPitch = pitch;
	std::uint64_t const rowBytes = static_cast<std::uint64_t>(
		signedPitch < 0 ? -signedPitch : signedPitch);
	std::uint64_t const bytes = rowBytes * rows;
	return bytes > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
		? std::numeric_limits<int>::max()
		: static_cast<int>(bytes);
}

struct BitmapRowView
{
	const unsigned char* data;
	std::size_t size;

	explicit operator bool() const noexcept { return data != nullptr; }
};

// FT_Bitmap::buffer starts at the upper row for positive pitch and at the
// lower row for negative pitch. Expose one top-down logical row and keep all
// signed-pitch, multiplication, and address-overflow checks in this interface.
inline BitmapRowView CheckedBitmapRow(
	const unsigned char* buffer,
	int pitch,
	unsigned int rows,
	unsigned int row,
	std::uint64_t requiredBytes = 0) noexcept
{
	if (buffer == nullptr || pitch == 0 || row >= rows)
		return {nullptr, 0};

	std::int64_t const signedPitch = pitch;
	std::uint64_t const rowBytes = static_cast<std::uint64_t>(
		signedPitch < 0 ? -signedPitch : signedPitch);
	if (rowBytes == 0 || requiredBytes > rowBytes ||
		rowBytes > static_cast<std::uint64_t>(
			std::numeric_limits<std::size_t>::max()))
		return {nullptr, 0};

	std::uint64_t const physicalRow = pitch > 0 ? row : rows - 1u - row;
	if (physicalRow != 0 &&
		rowBytes > std::numeric_limits<std::uint64_t>::max() / physicalRow)
		return {nullptr, 0};
	std::uint64_t const offset = physicalRow * rowBytes;
	std::uintptr_t const base = reinterpret_cast<std::uintptr_t>(buffer);
	if (offset > static_cast<std::uint64_t>(
			std::numeric_limits<std::uintptr_t>::max() - base))
		return {nullptr, 0};

	return {
		reinterpret_cast<const unsigned char*>(
			base + static_cast<std::uintptr_t>(offset)),
		static_cast<std::size_t>(rowBytes)};
}

struct CheckedFaceIndex
{
	std::size_t value;
	bool valid;
};

inline CheckedFaceIndex CheckFaceIndex(
	int oneBasedFaceId,
	std::size_t fontCount) noexcept
{
	if (oneBasedFaceId <= 0)
		return {0, false};
	std::size_t const index = static_cast<std::size_t>(oneBasedFaceId - 1);
	return {index, index < fontCount};
}

class StreamBackingOwnership
{
public:
	StreamBackingOwnership() = default;
	StreamBackingOwnership(const StreamBackingOwnership&) = delete;
	StreamBackingOwnership& operator=(const StreamBackingOwnership&) = delete;

	bool TransferToFace() noexcept
	{
		if (phase_ != Phase::builder)
			return false;
		phase_ = Phase::faceCallback;
		return true;
	}

	bool ReclaimFromCloseCallback() noexcept
	{
		if (phase_ != Phase::faceCallback)
			return false;
		phase_ = Phase::closed;
		return true;
	}

	bool callback_owned() const noexcept
	{
		return phase_ == Phase::faceCallback;
	}

private:
	enum class Phase : unsigned char
	{
		builder,
		faceCallback,
		closed,
	};

	Phase phase_ = Phase::builder;
};

struct RasterCacheKey
{
	int height;
	int width;
	int weightClass;
	bool italic;

	RasterCacheKey(
		int rasterHeight = 0,
		int rasterWidth = 0,
		int rasterWeightClass = 0,
		bool rasterItalic = false) noexcept
		: height(rasterHeight), width(rasterWidth),
		  weightClass(rasterWeightClass), italic(rasterItalic)
	{
	}

	bool operator==(const RasterCacheKey& other) const noexcept
	{
		return height == other.height && width == other.width &&
			weightClass == other.weightClass && italic == other.italic;
	}

	bool operator<(const RasterCacheKey& other) const noexcept
	{
		if (height != other.height)
			return height < other.height;
		if (width != other.width)
			return width < other.width;
		if (weightClass != other.weightClass)
			return weightClass < other.weightClass;
		return italic < other.italic;
	}
};

// Compatibility name for the immutable per-render view published by
// ProfileRuntime. The policy owns its root snapshot generation.
using RasterPolicy = ::renderer::RasterPolicy;
using StartupPolicy = ::renderer::FreeTypeStartupPolicy;

// FTC_Manager owns faces and caches that call back into its FT_Library. This
// pair makes the required manager-before-library release order impossible to
// accidentally invert during retry, explicit unload, or partial startup.
template <typename LibraryOwner, typename ManagerOwner>
class OrderedRuntimeOwners
{
public:
	OrderedRuntimeOwners() = default;
	~OrderedRuntimeOwners() noexcept { Reset(); }

	OrderedRuntimeOwners(const OrderedRuntimeOwners&) = delete;
	OrderedRuntimeOwners& operator=(const OrderedRuntimeOwners&) = delete;
	OrderedRuntimeOwners(OrderedRuntimeOwners&&) = delete;
	OrderedRuntimeOwners& operator=(OrderedRuntimeOwners&&) = delete;

	void Publish(LibraryOwner library, ManagerOwner manager)
	{
		Reset();
		library_ = std::move(library);
		manager_ = std::move(manager);
	}

	void Reset() noexcept
	{
		manager_.reset();
		library_.reset();
	}

	[[nodiscard]] bool initialized() const noexcept
	{
		return static_cast<bool>(library_) && static_cast<bool>(manager_);
	}

	[[nodiscard]] auto library() const noexcept { return library_.get(); }
	[[nodiscard]] auto manager() const noexcept { return manager_.get(); }

private:
	LibraryOwner library_;
	ManagerOwner manager_;
};

} // namespace freetype
} // namespace renderer

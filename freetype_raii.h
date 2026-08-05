#pragma once

#include <freetype/freetype.h>
#include <freetype/ftcache.h>
#include <freetype/ftglyph.h>
#include <freetype/ftmm.h>

#include <memory>
#include <vector>

#include "ftref.h"

namespace renderer_raii {

namespace detail {

struct FreeTypeResourceApi
{
	static void DoneLibrary(FT_Library value) noexcept { FT_Done_FreeType(value); }
	static void DoneManager(FTC_Manager value) noexcept { FTC_Manager_Done(value); }
	static void DoneFace(FT_Face value) noexcept { FT_Done_Face(value); }
	static void DoneGlyph(FT_Glyph value) noexcept { FT_Done_Glyph(value); }
	static void DoneReferencedGlyph(FT_Referenced_Glyph value) noexcept
	{
		FT_Referenced_Glyph local = value;
		FT_Done_Ref_Glyph(&local);
	}
	static void DoneMmVar(FT_Library library, FT_MM_Var* value) noexcept
	{
		FT_Done_MM_Var(library, value);
	}
};

template <typename Api>
struct FreeTypeLibraryDeleter
{
	using pointer = FT_Library;
	void operator()(FT_Library value) const noexcept { Api::DoneLibrary(value); }
};

template <typename Api>
struct FreeTypeManagerDeleter
{
	using pointer = FTC_Manager;
	void operator()(FTC_Manager value) const noexcept { Api::DoneManager(value); }
};

template <typename Api>
struct FreeTypeFaceDeleter
{
	using pointer = FT_Face;
	void operator()(FT_Face value) const noexcept { Api::DoneFace(value); }
};

template <typename Api>
struct FreeTypeGlyphDeleter
{
	using pointer = FT_Glyph;
	void operator()(FT_Glyph value) const noexcept { Api::DoneGlyph(value); }
};

template <typename Api>
struct ReferencedGlyphDeleter
{
	using pointer = FT_Referenced_Glyph;
	void operator()(FT_Referenced_Glyph value) const noexcept { Api::DoneReferencedGlyph(value); }
};

template <typename Api>
struct MmVarDeleter
{
	using pointer = FT_MM_Var*;
	FT_Library library = nullptr;
	void operator()(FT_MM_Var* value) const noexcept { Api::DoneMmVar(library, value); }
};

} // namespace detail

template <typename Api = detail::FreeTypeResourceApi>
using BasicUniqueFreeTypeLibrary = std::unique_ptr<void, detail::FreeTypeLibraryDeleter<Api>>;

using UniqueFreeTypeLibrary = BasicUniqueFreeTypeLibrary<>;

template <typename Api = detail::FreeTypeResourceApi>
using BasicUniqueFreeTypeManager = std::unique_ptr<void, detail::FreeTypeManagerDeleter<Api>>;

using UniqueFreeTypeManager = BasicUniqueFreeTypeManager<>;

template <typename Api = detail::FreeTypeResourceApi>
using BasicUniqueFreeTypeFace = std::unique_ptr<void, detail::FreeTypeFaceDeleter<Api>>;

using UniqueFreeTypeFace = BasicUniqueFreeTypeFace<>;

template <typename Api = detail::FreeTypeResourceApi>
using BasicUniqueFreeTypeGlyph = std::unique_ptr<void, detail::FreeTypeGlyphDeleter<Api>>;

using UniqueFreeTypeGlyph = BasicUniqueFreeTypeGlyph<>;

template <typename Api = detail::FreeTypeResourceApi>
using BasicUniqueReferencedGlyph = std::unique_ptr<void, detail::ReferencedGlyphDeleter<Api>>;

using UniqueReferencedGlyph = BasicUniqueReferencedGlyph<>;

template <typename Api = detail::FreeTypeResourceApi>
using BasicUniqueMmVar = std::unique_ptr<void, detail::MmVarDeleter<Api>>;

using UniqueMmVar = BasicUniqueMmVar<>;

inline UniqueMmVar AdoptMmVar(FT_Library library, FT_MM_Var* value) noexcept
{
	return UniqueMmVar(value, detail::MmVarDeleter<detail::FreeTypeResourceApi>{library});
}

// FreeType's referenced-glyph C interface writes a contiguous raw pointer
// array.  This buffer keeps that required layout while applying the matching
// reference-release rule to every populated slot.
template <typename Api = detail::FreeTypeResourceApi>
class BasicReferencedGlyphBuffer
{
public:
	explicit BasicReferencedGlyphBuffer(size_t size) : values_(size, nullptr) {}
	~BasicReferencedGlyphBuffer() noexcept { clear(); }

	BasicReferencedGlyphBuffer(const BasicReferencedGlyphBuffer&) = delete;
	BasicReferencedGlyphBuffer& operator=(const BasicReferencedGlyphBuffer&) = delete;
	BasicReferencedGlyphBuffer(BasicReferencedGlyphBuffer&& other) noexcept
	{
		values_.swap(other.values_);
	}
	BasicReferencedGlyphBuffer& operator=(BasicReferencedGlyphBuffer&& other) noexcept
	{
		if (this != &other) {
			clear();
			values_.clear();
			values_.swap(other.values_);
		}
		return *this;
	}

	FT_Referenced_Glyph* data() noexcept { return values_.data(); }
	const FT_Referenced_Glyph* data() const noexcept { return values_.data(); }
	size_t size() const noexcept { return values_.size(); }
	FT_Referenced_Glyph& operator[](size_t index) noexcept { return values_[index]; }
	const FT_Referenced_Glyph& operator[](size_t index) const noexcept { return values_[index]; }

	void clear() noexcept
	{
		for (FT_Referenced_Glyph& value : values_) {
			if (value != nullptr) {
				Api::DoneReferencedGlyph(value);
				value = nullptr;
			}
		}
	}

private:
	std::vector<FT_Referenced_Glyph> values_;
};

using ReferencedGlyphBuffer = BasicReferencedGlyphBuffer<>;

} // namespace renderer_raii

#pragma once

#include "bold_face_selection.h"

#include <vector>

namespace renderer {
namespace gdi_family_catalog {

// Faces GDI enumerates for one family, each tagged with that family name.
// Returns false and leaves `faces` empty when the family is unknown or the
// lookup failed.
bool FamilyFaces(
	const wchar_t* gdiFamily,
	std::vector<bold_face_selection::FaceCandidate>& faces) noexcept;

// Every face of every GDI family that shares the typographic family (name
// ID 16, else name ID 1) of `gdiFamily`, including `gdiFamily` itself.
bool TypographicFamilyFaces(
	const wchar_t* gdiFamily,
	std::vector<bold_face_selection::FaceCandidate>& faces) noexcept;

void ClearForQuietUnload() noexcept;

} // namespace gdi_family_catalog
} // namespace renderer

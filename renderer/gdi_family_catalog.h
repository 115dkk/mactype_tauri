#pragma once

#include "bold_face_selection.h"

#include <optional>
#include <vector>

namespace renderer {
namespace gdi_family_catalog {

// Faces GDI enumerates for one family, each tagged with that family name.
// Returns no value when the family is unknown or the lookup failed.
[[nodiscard]] std::optional<std::vector<bold_face_selection::FaceCandidate>>
FamilyFaces(const wchar_t* gdiFamily) noexcept;

// Every face of every GDI family that shares the typographic family (name
// ID 16, else name ID 1) of `gdiFamily`, including `gdiFamily` itself.
[[nodiscard]] std::optional<std::vector<bold_face_selection::FaceCandidate>>
TypographicFamilyFaces(const wchar_t* gdiFamily) noexcept;

void ClearForQuietUnload() noexcept;

} // namespace gdi_family_catalog
} // namespace renderer

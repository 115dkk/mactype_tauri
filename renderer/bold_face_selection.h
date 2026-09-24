#pragma once

#include <string>
#include <vector>

namespace renderer {
namespace bold_face_selection {

constexpr int kBoldClassWeight = 600;

struct FaceCandidate
{
	std::wstring gdiFamily;
	int weight = 400;
	bool italic = false;
};

constexpr bool IsBoldClassWeight(int weight) noexcept
{
	return weight >= kBoldClassWeight;
}

// Mode 2 selection. Only faces heavier than baseWeight qualify; the lightest
// qualifying face at or above requestedWeight wins, otherwise the heaviest.
// Returns nullptr when nothing is heavier than the base face.
const FaceCandidate* SelectBoldFace(
	const std::vector<FaceCandidate>& candidates,
	int baseWeight,
	int requestedWeight,
	bool italic) noexcept;

// Mode 3 selection inside a paired family: nearest weight, ties go heavier.
const FaceCandidate* SelectNearestWeight(
	const std::vector<FaceCandidate>& candidates,
	int requestedWeight,
	bool italic) noexcept;

} // namespace bold_face_selection
} // namespace renderer

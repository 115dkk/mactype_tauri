#include "bold_face_selection.h"

namespace renderer {
namespace bold_face_selection {
namespace {

const FaceCandidate* SelectBoldFaceWithin(
	const std::vector<FaceCandidate>& candidates,
	int baseWeight,
	int requestedWeight,
	bool italic,
	bool requireItalicMatch) noexcept
{
	const FaceCandidate* atOrAbove = nullptr;
	const FaceCandidate* heaviest = nullptr;
	for (const FaceCandidate& candidate : candidates)
	{
		if (candidate.weight <= baseWeight)
			continue;
		if (requireItalicMatch && candidate.italic != italic)
			continue;
		if (candidate.weight >= requestedWeight &&
			(atOrAbove == nullptr || candidate.weight < atOrAbove->weight))
			atOrAbove = &candidate;
		if (heaviest == nullptr || candidate.weight > heaviest->weight)
			heaviest = &candidate;
	}
	return atOrAbove != nullptr ? atOrAbove : heaviest;
}

int Distance(int left, int right) noexcept
{
	return left > right ? left - right : right - left;
}

const FaceCandidate* SelectNearestWithin(
	const std::vector<FaceCandidate>& candidates,
	int requestedWeight,
	bool italic,
	bool requireItalicMatch) noexcept
{
	const FaceCandidate* nearest = nullptr;
	for (const FaceCandidate& candidate : candidates)
	{
		if (requireItalicMatch && candidate.italic != italic)
			continue;
		if (nearest == nullptr)
		{
			nearest = &candidate;
			continue;
		}
		int const distance = Distance(candidate.weight, requestedWeight);
		int const best = Distance(nearest->weight, requestedWeight);
		if (distance < best ||
			(distance == best && candidate.weight > nearest->weight))
			nearest = &candidate;
	}
	return nearest;
}

} // namespace

const FaceCandidate* SelectBoldFace(
	const std::vector<FaceCandidate>& candidates,
	int baseWeight,
	int requestedWeight,
	bool italic) noexcept
{
	const FaceCandidate* const matched = SelectBoldFaceWithin(
		candidates, baseWeight, requestedWeight, italic, true);
	if (matched != nullptr)
		return matched;
	return SelectBoldFaceWithin(
		candidates, baseWeight, requestedWeight, italic, false);
}

const FaceCandidate* SelectNearestWeight(
	const std::vector<FaceCandidate>& candidates,
	int requestedWeight,
	bool italic) noexcept
{
	const FaceCandidate* const matched = SelectNearestWithin(
		candidates, requestedWeight, italic, true);
	if (matched != nullptr)
		return matched;
	return SelectNearestWithin(candidates, requestedWeight, italic, false);
}

} // namespace bold_face_selection
} // namespace renderer

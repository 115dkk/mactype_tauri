#include "../../../renderer/bold_face_selection.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

namespace selection = renderer::bold_face_selection;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

selection::FaceCandidate Face(const wchar_t* family, int weight, bool italic = false)
{
    selection::FaceCandidate face;
    face.gdiFamily = family;
    face.weight = weight;
    face.italic = italic;
    return face;
}

std::vector<selection::FaceCandidate> Weights(std::vector<int> const& weights)
{
    std::vector<selection::FaceCandidate> faces;
    for (int weight : weights) {
        faces.push_back(Face(L"Family", weight));
    }
    return faces;
}

int WeightOf(const selection::FaceCandidate* face)
{
    return face == nullptr ? -1 : face->weight;
}

} // namespace

int main()
{
    Require(!selection::IsBoldClassWeight(599) && selection::IsBoldClassWeight(600) &&
                selection::IsBoldClassWeight(900) && !selection::IsBoldClassWeight(0),
            "bold-class requests must start exactly at weight 600");

    std::vector<selection::FaceCandidate> const full =
        Weights({300, 400, 500, 600, 700, 800, 900});
    Require(WeightOf(selection::SelectBoldFace(full, 500, 700, false)) == 700,
            "a 700 request over a 500 base must pick the 700 face");
    Require(WeightOf(selection::SelectBoldFace(full, 500, 600, false)) == 600,
            "a 600 request over a 500 base must pick the 600 face");
    Require(WeightOf(selection::SelectBoldFace(full, 800, 700, false)) == 900,
            "a base heavier than the request must still move to a heavier face");
    Require(selection::SelectBoldFace(full, 900, 700, false) == nullptr,
            "no face heavier than the base must leave the choice to synthesis");
    Require(WeightOf(selection::SelectBoldFace(Weights({400, 600}), 400, 700, false)) == 600,
            "without a face at the request the heaviest heavier face must win");
    Require(selection::SelectBoldFace({}, 400, 700, false) == nullptr,
            "an empty candidate list must select nothing");

    std::vector<selection::FaceCandidate> const styled = {
        Face(L"Upright", 400), Face(L"Upright", 700),
        Face(L"Slanted", 400, true), Face(L"Slanted", 800, true),
    };
    const selection::FaceCandidate* chosen =
        selection::SelectBoldFace(styled, 400, 700, true);
    Require(chosen != nullptr && chosen->italic && chosen->weight == 800,
            "an italic request must prefer a heavier italic face");
    chosen = selection::SelectBoldFace(styled, 400, 700, false);
    Require(chosen != nullptr && !chosen->italic && chosen->weight == 700,
            "an upright request must prefer a heavier upright face");
    std::vector<selection::FaceCandidate> const uprightOnly = {
        Face(L"Upright", 400), Face(L"Upright", 700),
    };
    chosen = selection::SelectBoldFace(uprightOnly, 400, 700, true);
    Require(chosen != nullptr && chosen->weight == 700,
            "an italic request must fall back to upright faces when no italic qualifies");

    Require(WeightOf(selection::SelectNearestWeight(Weights({600, 800}), 700, false)) == 800,
            "an equidistant nearest-weight tie must go to the heavier face");
    Require(WeightOf(selection::SelectNearestWeight(Weights({400, 650, 900}), 700, false)) == 650,
            "the nearest weight must win inside a paired family");
    Require(WeightOf(selection::SelectNearestWeight(Weights({300}), 700, false)) == 300,
            "a single paired face must be selected whatever its weight");
    Require(selection::SelectNearestWeight({}, 700, false) == nullptr,
            "an absent paired family must select nothing");
    chosen = selection::SelectNearestWeight(styled, 700, true);
    Require(chosen != nullptr && chosen->italic && chosen->weight == 800,
            "nearest-weight selection must prefer the requested italic flag");
    chosen = selection::SelectNearestWeight(uprightOnly, 700, true);
    Require(chosen != nullptr && chosen->weight == 700,
            "nearest-weight selection must ignore italic when nothing matches it");

    std::cout << "Bold face selection tests passed.\n";
    return 0;
}

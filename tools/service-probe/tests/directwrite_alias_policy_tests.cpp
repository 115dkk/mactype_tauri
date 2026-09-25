#include "../../../renderer/directwrite_alias_policy.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

renderer::font_substitution::Rule Rule(
    const wchar_t* source,
    const wchar_t* replacement)
{
    return {source, replacement, false, 1, 0};
}

directwrite_alias::SourceFaceObservation Face(
    std::vector<std::wstring> names,
    int weight,
    bool aliased,
    bool italic = false,
    const wchar_t* replacement = L"Pretendard")
{
    directwrite_alias::SourceFaceObservation face;
    face.familyNames = std::move(names);
    face.weight = weight;
    face.italic = italic;
    face.aliased = aliased;
    if (aliased)
        face.replacementFamily = replacement;
    return face;
}

std::vector<directwrite_alias::SyntheticBoldSlot> Plan(
    const std::vector<directwrite_alias::SourceFaceObservation>& faces)
{
    std::vector<directwrite_alias::SyntheticBoldSlot> slots;
    Require(directwrite_alias::PlanSyntheticBoldSlots(faces, slots),
        "planning synthetic bold slots failed");
    return slots;
}

void TestSyntheticBoldSlots()
{
    Require(Plan({
        Face({L"Malgun Gothic", L"맑은 고딕"}, 400, true),
        Face({L"Malgun Gothic", L"맑은 고딕"}, 700, true),
    }).empty(), "a family with a real bold face received a synthesized bold");

    auto regularOnly = Plan({
        Face({L"Gulim", L"굴림"}, 350, true),
        Face({L"Gulim", L"굴림"}, 400, true),
        Face({L"Gulim", L"굴림"}, 400, true, true),
    });
    Require(regularOnly.size() == 1 &&
        regularOnly[0].familyNames.size() == 2 &&
        regularOnly[0].familyNames[0] == L"Gulim" &&
        regularOnly[0].familyNames[1] == L"굴림" &&
        regularOnly[0].templateFace == 1,
        "a family without a bold face did not get one slot from its upright regular face");

    Require(Plan({
        Face({L"Dotum", L"돋움"}, 400, true),
        Face({L"Dotum", L"돋움"}, 700, false),
    }).empty(), "a bold face kept native received a synthesized twin");

    auto partlyNative = Plan({
        Face({L"Batang", L"바탕"}, 400, true),
        Face({L"BATANG"}, 700, false),
    });
    Require(partlyNative.size() == 1 &&
        partlyNative[0].familyNames.size() == 1 &&
        partlyNative[0].familyNames[0] == L"바탕",
        "a native bold face did not hold exactly the names it carries");

    auto shared = Plan({
        Face({L"Family A", L"Shared"}, 400, true),
        Face({L"Shared", L"Family B"}, 400, true),
    });
    Require(shared.size() == 2 &&
        shared[0].templateFace == 0 &&
        shared[0].familyNames.size() == 2 &&
        shared[0].familyNames[0] == L"Family A" &&
        shared[0].familyNames[1] == L"Shared" &&
        shared[1].templateFace == 1 &&
        shared[1].familyNames.size() == 1 &&
        shared[1].familyNames[0] == L"Family B",
        "a name shared by two families was not given exactly one slot");

    Require(Plan({
        Face({L"Split"}, 400, true, false, L"Pretendard"),
        Face({L"Split"}, 400, true, false, L"Noto Sans KR"),
    }).empty(), "conflicting replacements for one name produced a synthesized bold");

    Require(Plan({
        Face({L"Native Only"}, 400, false),
    }).empty(), "a family without any alias received a synthesized bold");
}

} // namespace

int main()
{
    auto substitutions = renderer::font_substitution::Snapshot::Build({
        Rule(L"맑은 고딕", L"Pretendard Variable"),
    }, 1);
    directwrite_alias::FamilyAliasResolution resolution;
    Require(directwrite_alias::ResolveFamilyAliases(
        {L"Malgun Gothic", L"맑은 고딕", L"MALGUN GOTHIC"},
        *substitutions, resolution),
        "a localized family rule did not resolve its DirectWrite face");
    Require(resolution.replacementFamily == L"Pretendard Variable" &&
        resolution.sourceAliases.size() == 2 &&
        resolution.sourceAliases[0] == L"Malgun Gothic" &&
        resolution.sourceAliases[1] == L"맑은 고딕",
        "resolving one localized name discarded another name for the same face");

    auto semilight = renderer::font_substitution::Snapshot::Build({
        Rule(L"맑은 고딕", L"Pretendard Variable"),
        Rule(L"맑은 고딕 Semilight", L"Pretendard Variable"),
    }, 2);
    Require(directwrite_alias::ResolveFamilyAliases({
        L"Malgun Gothic Semilight",
        L"맑은 고딕 Semilight",
        L"Malgun Gothic",
        L"맑은 고딕",
    }, *semilight, resolution) && resolution.sourceAliases.size() == 4,
        "a semilight face lost its Win32 or typographic family aliases");

    auto conflicting = renderer::font_substitution::Snapshot::Build({
        Rule(L"Malgun Gothic", L"Noto Sans KR"),
        Rule(L"맑은 고딕", L"Pretendard Variable"),
    }, 3);
    Require(!directwrite_alias::ResolveFamilyAliases(
        {L"Malgun Gothic", L"맑은 고딕"}, *conflicting, resolution),
        "conflicting localized rules silently selected one replacement");

    auto cycle = renderer::font_substitution::Snapshot::Build({
        Rule(L"맑은 고딕", L"Pretendard Variable"),
        Rule(L"Pretendard Variable", L"맑은 고딕"),
    }, 4);
    Require(!directwrite_alias::ResolveFamilyAliases(
        {L"Malgun Gothic", L"맑은 고딕"}, *cycle, resolution),
        "a cyclic substitution produced a DirectWrite alias");

    directwrite_alias::FallbackAliasResolution fallback;
    Require(directwrite_alias::ResolveFallbackAlias(
        {L"Malgun Gothic", L"맑은 고딕"}, *substitutions, fallback) &&
        fallback.matchedSourceName == L"맑은 고딕" &&
        fallback.replacementFamily == L"Pretendard Variable",
        "fallback selection did not preserve the name that matched the snapshot");
    Require(!directwrite_alias::ResolveFallbackAlias(
        {L"Segoe UI", L"Segoe UI Variable"}, *substitutions, fallback),
        "fallback selection matched a family without a substitution rule");
    Require(!directwrite_alias::ResolveFallbackAlias(
        {L"Malgun Gothic", L"맑은 고딕"}, *conflicting, fallback),
        "fallback selection accepted conflicting names for one mapped face");
    Require(!directwrite_alias::ResolveFallbackAlias(
        {L"Malgun Gothic", L"맑은 고딕"}, *cycle, fallback),
        "fallback selection accepted a cyclic substitution");

    TestSyntheticBoldSlots();
    return 0;
}

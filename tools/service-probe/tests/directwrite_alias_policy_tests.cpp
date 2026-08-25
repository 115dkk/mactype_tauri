#include "../../../renderer/directwrite_alias_policy.h"

#include <cstdlib>
#include <iostream>

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
    return 0;
}

#include "../../../renderer/font_substitution.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

renderer::font_substitution::Rule Rule(
    const wchar_t* source,
    const wchar_t* replacement,
    bool charsetSpecific = false,
    unsigned char charset = 1)
{
    return renderer::font_substitution::Rule(
        source, replacement, charsetSpecific, charset, 0);
}

} // namespace

int main()
{
    namespace substitution = renderer::font_substitution;

    auto chain = substitution::Snapshot::Build({
        Rule(L"Arial", L"Helvetica"),
        Rule(L"Helvetica", L"Courier New"),
    }, 7);
    substitution::Resolution result = chain->Resolve({L"aRiAl", 1});
    Require(result.status == substitution::ResolutionStatus::applied,
            "a case-insensitive substitution chain must resolve");
    Require(result.family == L"Courier New" && result.hops == 2,
            "the resolver must return the final family and exact hop count");
    Require(result.ruleId != 0 && result.generation == 7,
            "resolved output must retain stable rule and snapshot identity");
    Require(result.snapshotDigest != 0 && result.snapshotDigest == chain->digest(),
            "resolved output must identify the immutable rule-set digest");
    auto sameChain = substitution::Snapshot::Build({
        Rule(L"Arial", L"Helvetica"),
        Rule(L"Helvetica", L"Courier New"),
    }, 700);
    Require(sameChain->digest() == chain->digest(),
            "the same rules must retain one digest across reload generations");

    auto cycle = substitution::Snapshot::Build({
        Rule(L"Arial", L"Helvetica"),
        Rule(L"Helvetica", L"Arial"),
    }, 8);
    result = cycle->Resolve({L"Arial", 1});
    Require(result.status == substitution::ResolutionStatus::cycle &&
                result.family == L"Arial" && !result.matched,
            "a cycle must fail closed to the requested family");

    auto deep = substitution::Snapshot::Build({
        Rule(L"A", L"B"), Rule(L"B", L"C"), Rule(L"C", L"D"),
    }, 9);
    result = deep->Resolve({L"A", 1}, 2);
    Require(result.status == substitution::ResolutionStatus::depthExceeded &&
                result.family == L"A",
            "a chain beyond the explicit hop budget must fail closed");

    auto charsets = substitution::Snapshot::Build({
        Rule(L"Arial", L"Generic"),
        Rule(L"Arial", L"Japanese", true, 128),
        Rule(L"Arial", L"Ignored duplicate"),
    }, 10);
    Require(charsets->Resolve({L"Arial", 128}).family == L"Japanese",
            "an exact charset rule must take precedence over a generic rule");
    Require(charsets->Resolve({L"Arial", 1}).family == L"Generic",
            "the first generic rule must remain deterministic");

    substitution::Registry registry;
    registry.Publish(chain);
    std::shared_ptr<const substitution::Snapshot> retained = registry.Load();
    registry.Publish(charsets);
    Require(retained->Resolve({L"Arial", 1}).family == L"Courier New",
            "a retained generation must stay immutable after reload");
    Require(registry.Load()->Resolve({L"Arial", 1}).family == L"Generic",
            "new readers must observe the atomically published generation");

    auto one = substitution::Snapshot::Build({Rule(L"Source", L"One")}, 100);
    auto two = substitution::Snapshot::Build({Rule(L"Source", L"Two")}, 101);
    registry.Publish(one);
    std::atomic<bool> start{false};
    std::atomic<bool> coherent{true};
    std::vector<std::thread> readers;
    for (unsigned int index = 0; index < 8; ++index) {
        readers.emplace_back([&registry, &start, &coherent]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (unsigned int read = 0; read < 1000; ++read) {
                auto observed = registry.Load();
                auto resolved = observed->Resolve({L"Source", 1});
                if (!resolved.matched ||
                    (resolved.family != L"One" && resolved.family != L"Two") ||
                    (resolved.generation != 100 && resolved.generation != 101)) {
                    coherent.store(false, std::memory_order_release);
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (unsigned int write = 0; write < 1000; ++write) {
        registry.Publish((write & 1) == 0 ? two : one);
    }
    for (std::thread& reader : readers) {
        reader.join();
    }
    Require(coherent.load(std::memory_order_acquire),
            "concurrent reload must publish only complete immutable generations");

    result = chain->Resolve({L"Missing", 1});
    Require(result.status == substitution::ResolutionStatus::noMatch &&
                result.family == L"Missing",
            "an unmatched family must be returned unchanged");

    substitution::BoldPair pair;
    Require(substitution::ParseBoldPairLine(L"  Pretendard Medium \t=  Pretendard Bold ", pair) &&
                pair.family == L"Pretendard Medium" && pair.boldFamily == L"Pretendard Bold",
            "a bold pair line must trim both sides");
    Require(substitution::ParseBoldPairLine(L"Noto Sans KR,129=Noto Sans KR Bold , 129", pair) &&
                pair.family == L"Noto Sans KR" && pair.boldFamily == L"Noto Sans KR Bold",
            "a bold pair line must drop a trailing charset suffix from either side");
    Require(substitution::ParseBoldPairLine(L"Family,Name=Bold", pair) &&
                pair.family == L"Family,Name",
            "a comma that is not a charset suffix must stay in the family name");
    Require(!substitution::ParseBoldPairLine(L"Pretendard", pair) &&
                pair.family.empty() && pair.boldFamily.empty(),
            "a bold pair line without '=' must be ignored");
    Require(!substitution::ParseBoldPairLine(L"=Pretendard Bold", pair) &&
                !substitution::ParseBoldPairLine(L"Pretendard=  ", pair) &&
                !substitution::ParseBoldPairLine(L",129=Bold", pair),
            "a bold pair line with an empty side must be ignored");
    Require(!substitution::ParseBoldPairLine(L"Pretendard=pretendard", pair),
            "an identity bold pair must be ignored");

    substitution::BoldMode mode = substitution::BoldMode::ignoreWeight;
    Require(substitution::BoldModeFromProfileValue(3, mode) &&
                mode == substitution::BoldMode::pairs &&
                !substitution::BoldModeFromProfileValue(4, mode) &&
                mode == substitution::BoldMode::sameFamily,
            "profile values must map onto bold modes and default to same family");

    std::vector<substitution::Rule> const boldRules = {Rule(L"Malgun Gothic", L"Pretendard Medium")};
    auto pairs = substitution::Snapshot::Build(
        boldRules, substitution::BoldMode::pairs,
        {{L"Pretendard Medium", L"Overridden Bold"},
         {L"PRETENDARD MEDIUM", L"Pretendard Bold"},
         {L"", L"Empty"},
         {L"Same", L"same"},
         {L"Other", L"Other Bold"}},
        20);
    Require(pairs->bold_pairs().size() == 2 &&
                pairs->bold_pairs()[0].boldFamily == L"Pretendard Bold",
            "bold pairs must drop empty and identity pairs and keep the last per family");
    Require(pairs->FindBoldPair(L"pretendard medium") != nullptr &&
                pairs->FindBoldPair(L"pretendard medium")->boldFamily == L"Pretendard Bold" &&
                pairs->FindBoldPair(L"Missing") == nullptr,
            "bold pair lookup must be case-insensitive");

    substitution::BoldPlan plan = pairs->PlanBold(L"Pretendard Medium", 700);
    Require(plan.action == substitution::BoldAction::pairedFamily &&
                plan.pairedFamily == L"Pretendard Bold",
            "pairs mode must direct a bold request to its paired family");
    Require(pairs->PlanBold(L"Unpaired", 700).action == substitution::BoldAction::dropWeight,
            "pairs mode without a pair must ignore the requested weight");
    Require(pairs->PlanBold(L"Pretendard Medium", 599).action ==
                substitution::BoldAction::unchanged,
            "a request below the bold class must keep today's behaviour");
    Require(substitution::Snapshot::Build(boldRules, substitution::BoldMode::ignoreWeight, {}, 21)
                    ->PlanBold(L"Pretendard Medium", 600).action ==
                substitution::BoldAction::dropWeight &&
                substitution::Snapshot::Build(boldRules, substitution::BoldMode::synthetic, {}, 21)
                        ->PlanBold(L"Pretendard Medium", 600).action ==
                    substitution::BoldAction::keepWeight &&
                substitution::Snapshot::Build(boldRules, substitution::BoldMode::sameFamily, {}, 21)
                        ->PlanBold(L"Pretendard Medium", 600).action ==
                    substitution::BoldAction::sameFamilyFace,
            "each bold mode must map onto its adapter action");

    auto twoArgument = substitution::Snapshot::Build(boldRules, 30);
    auto sameFamily = substitution::Snapshot::Build(
        boldRules, substitution::BoldMode::sameFamily, {}, 30);
    Require(twoArgument->bold_mode() == substitution::BoldMode::sameFamily &&
                twoArgument->bold_pairs().empty() &&
                twoArgument->digest() == sameFamily->digest(),
            "the two-argument build must mean same-family mode without pairs");
    auto pairsReloaded = substitution::Snapshot::Build(
        boldRules, substitution::BoldMode::pairs,
        {{L"Pretendard Medium", L"Pretendard Bold"}, {L"Other", L"Other Bold"}}, 40);
    Require(pairsReloaded->digest() == pairs->digest(),
            "identical bold settings must keep one digest across generations");
    auto pairsChanged = substitution::Snapshot::Build(
        boldRules, substitution::BoldMode::pairs,
        {{L"Pretendard Medium", L"Pretendard ExtraBold"}, {L"Other", L"Other Bold"}}, 40);
    auto modeChanged = substitution::Snapshot::Build(
        boldRules, substitution::BoldMode::synthetic,
        {{L"Pretendard Medium", L"Pretendard Bold"}, {L"Other", L"Other Bold"}}, 40);
    Require(pairsChanged->digest() != pairs->digest() &&
                modeChanged->digest() != pairs->digest() &&
                sameFamily->digest() != pairs->digest(),
            "a bold mode or pair change must change the snapshot digest");

    std::cout << "Font substitution snapshot tests passed.\n";
    return 0;
}

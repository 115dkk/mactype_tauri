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

    std::cout << "Font substitution snapshot tests passed.\n";
    return 0;
}

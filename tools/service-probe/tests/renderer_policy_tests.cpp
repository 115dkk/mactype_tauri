#include "../../../renderer/profile_runtime.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
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
    const wchar_t* replacement)
{
    return renderer::font_substitution::Rule(
        source, replacement, false, 1, 0);
}

renderer::RendererPolicyCandidate Candidate(
    float gamma,
    const wchar_t* replacement)
{
    renderer::RendererPolicyCandidate candidate;
    candidate.valid = true;
    candidate.profileDigest = "sha256:" + std::string(64, 'a');
    candidate.hooks.directWrite = true;
    candidate.hooks.fontSubstitution = true;
    candidate.freeType.cacheMaxFaces = 64;
    candidate.freeType.cacheMaxSizes = 1200;
    candidate.freeType.cacheMaxBytes = 10 * 1024 * 1024;
    candidate.raster.fontLoader = 1;
    candidate.raster.gamma = gamma;
    candidate.raster.harmonyLcd = true;
    candidate.directWrite.gamma = gamma * gamma;
    candidate.substitutionsReady = true;
    candidate.substitutionRules.push_back(Rule(L"Arial", replacement));

    CFontSettings common;
    common.SetHintingMode(1);
    common.SetAntiAliasMode(2);
    candidate.commonFontSettings = common;

    CFontSettings individual;
    individual.SetHintingMode(2);
    individual.SetAntiAliasMode(1);
    candidate.individualFonts.push_back({L"Consolas", individual});
    return candidate;
}

} // namespace

int main()
{
    std::string knownDigest;
    const unsigned char knownBytes[] = {'a', 'b', 'c'};
    Require(renderer::Sha256ProfileDigest(
                knownBytes, sizeof(knownBytes), knownDigest) &&
                knownDigest ==
                    "sha256:ba7816bf8f01cfea414140de5dae2223"
                    "b00361a396177a9cb410ff61f20015ad",
            "profile digest must hash the exact byte sequence canonically");

    renderer::ProfileRuntime runtime;
    Require(!runtime.Load(), "a private profile runtime must begin unpublished");

    renderer::ProfilePublication first = runtime.Publish(
        Candidate(1.25f, L"Courier New"));
    Require(first.published(), "a valid policy candidate must publish");
    Require(first.snapshot && first.generation == 1 && first.revision == 1,
            "the first complete policy must publish one coherent identity");
    Require(first.snapshot->generation() == first.generation &&
                first.snapshot->revision() == first.revision,
            "publication and snapshot identities must match");
    Require(first.snapshot->profile_digest() ==
                "sha256:" + std::string(64, 'a'),
            "the immutable snapshot must retain the exact profile digest");
    Require(first.snapshot->raster().generation == first.generation &&
                first.snapshot->raster().gamma == 1.25f,
            "the raster view must retain its root policy generation");
    Require(first.snapshot->font_substitutions()->generation() == first.revision,
            "font substitution must be tied to the root policy revision");
    Require(renderer::font_substitution::ProcessRegistry().Load()->generation() ==
                first.revision &&
                renderer::font_substitution::ProcessRegistry().Load()->digest() ==
                    first.snapshot->font_substitutions()->digest(),
            "legacy substitution readers must receive the root-owned revision");
    Require(first.snapshot->font_substitutions()
                    ->Resolve({L"Arial", 1})
                    .family == L"Courier New",
            "the root snapshot must own the effective substitution rules");
    Require(first.snapshot->font_settings_for(L"consolas").GetHintingMode() == 2,
            "individual font policy lookup must be case insensitive");
    Require(first.snapshot->font_settings_for(L"@Consolas").GetHintingMode() == 2,
            "vertical fonts must reuse their horizontal policy");
    Require(first.snapshot->font_settings_for(L"Missing").GetHintingMode() == 1,
            "unknown fonts must use the immutable common policy");

    renderer::RendererPolicyRef retained = first.snapshot;
    renderer::RendererPolicyCandidate invalid = Candidate(9.0f, L"Invalid");
    invalid.freeType.cacheMaxBytes = -1;
    renderer::ProfilePublication rejected = runtime.Publish(std::move(invalid));
    Require(!rejected.published(), "an invalid reload must be rejected");
    Require(runtime.Load() == retained &&
                runtime.Load()->raster().gamma == 1.25f,
            "a failed reload must preserve the previous complete snapshot");

    renderer::RendererPolicyCandidate nonCanonical =
        Candidate(1.75f, L"Invalid");
    nonCanonical.profileDigest[7] = 'A';
    rejected = runtime.Publish(std::move(nonCanonical));
    Require(!rejected.published() && runtime.Load() == retained,
            "a non-canonical profile digest must preserve the prior snapshot");
    Require(runtime.MatchesProfileDigest(retained->profile_digest()),
            "the active profile digest must match its runtime policy");
    Require(!runtime.MatchesProfileDigest(
                "sha256:" + std::string(64, 'b')) &&
                runtime.Load() == retained,
            "a profile digest mismatch must not mutate the active snapshot");

    renderer::RendererPolicyCandidate failed =
        Candidate(1.75f, L"Failed");
    failed.valid = false;
    rejected = runtime.Publish(std::move(failed));
    Require(!rejected.published() && runtime.Load() == retained,
            "a failed publication must preserve the prior snapshot");

    renderer::ProfilePublication second = runtime.Publish(
        Candidate(1.5f, L"Segoe UI"));
    Require(second.published() && second.generation == 2 && second.revision == 2,
            "a later publication must advance one coherent identity");
    Require(retained->raster().gamma == 1.25f &&
                retained->font_substitutions()
                        ->Resolve({L"Arial", 1})
                        .family == L"Courier New",
            "retained readers must keep the old immutable policy generation");

    std::atomic<bool> start{false};
    std::atomic<bool> coherent{true};
    std::vector<std::thread> readers;
    for (unsigned int index = 0; index < 8; ++index) {
        readers.emplace_back([&runtime, &start, &coherent]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (unsigned int read = 0; read < 1000; ++read) {
                renderer::RendererPolicyRef observed = runtime.Load();
                if (!observed ||
                    observed->raster().generation != observed->generation() ||
                    observed->font_substitutions()->generation() !=
                        observed->revision()) {
                    coherent.store(false, std::memory_order_release);
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (unsigned int write = 0; write < 100; ++write) {
        renderer::ProfilePublication publication = runtime.Publish(
            Candidate((write & 1U) == 0U ? 1.25f : 1.5f,
                      (write & 1U) == 0U ? L"Courier New" : L"Segoe UI"));
        Require(publication.published(), "concurrent valid publication failed");
    }
    for (std::thread& reader : readers) {
        reader.join();
    }
    Require(coherent.load(std::memory_order_acquire),
            "concurrent readers observed a torn policy generation");

    std::cout << "Renderer policy publication tests passed.\n";
    return 0;
}

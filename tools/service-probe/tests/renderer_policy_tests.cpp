#include "../../../renderer/profile_runtime.h"
#include "../../../renderer/control_message.h"

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
    candidate.hooks.skipPrivateFreeType = true;
    candidate.hooks.skipConsoleProcesses = true;
    candidate.hooks.unityFontMode = renderer::UnityFontHookMode::mostGames;
    candidate.hooks.unityFontEnabledForProcess = true;
    candidate.freeType.cacheMaxFaces = 64;
    candidate.freeType.cacheMaxSizes = 1200;
    candidate.freeType.cacheMaxBytes = 10 * 1024 * 1024;
    candidate.raster.fontLoader = 1;
    candidate.raster.fontLinkMode = 2;
    candidate.raster.gammaMode = 3;
    candidate.raster.bolderMode = 2;
    candidate.raster.useMapping = true;
    candidate.raster.substituteAllFonts = true;
    candidate.raster.gamma = gamma;
    candidate.admission.displayAffinity = {gamma == 1.25f ? 1 : 2};
    candidate.raster.renderWeight = 1.0f;
    candidate.raster.contrast = 1.0f;
    for (unsigned int value = 0; value < 256; ++value) {
        candidate.raster.coverageTuning[value] =
            static_cast<unsigned char>(value);
        candidate.raster.coverageTuningR[value] =
            static_cast<unsigned char>(value);
        candidate.raster.coverageTuningG[value] =
            static_cast<unsigned char>(value);
        candidate.raster.coverageTuningB[value] =
            static_cast<unsigned char>(value);
    }
    candidate.raster.harmonyLcd = true;
    candidate.raster.shadowAlpha = 45;
    candidate.raster.shadowLightAlpha = 67;
    std::shared_ptr<renderer::FontLinkPolicy> links =
        std::make_shared<renderer::FontLinkPolicy>();
    links->allowDefault[128] = true;
    links->defaultFamilies[16] = L"Times New Roman";
    links->entries.push_back({L"Arial", {L"Segoe UI", L"SimSun"}});
    candidate.raster.fontLinks = std::move(links);
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


void TestAdmissionAndReload()
{
    renderer::ProfileRuntime runtime;
    auto candidate = Candidate(1.25f, L"Replacement");
    candidate.admission.displayAffinity = {1};
    candidate.admission.excludeFonts = {L"Excluded"};
    candidate.admission.includeFonts = {L"Included", L"Excluded"};
    candidate.admission.executableDirectory = L"c:\\apps\\child\\";
    candidate.admission.excludeModules = {L"C:\\Apps\\"};
    candidate.admission.includeModules = {L"C:\\Apps\\"};
    candidate.admission.unloadModules = {L"C:\\Apps\\"};
    candidate.raster.minHeight = 3;
    candidate.raster.maxHeight = 90;
    candidate.raster.clipBoxFix = true;
    candidate.raster.shadowEnabled = true;
    candidate.raster.shadowParams = {1, 2, 3, 4};
    const auto first = runtime.Publish(candidate).snapshot;
    Require(first && first->display_allowed(1) && !first->display_allowed(2) && first->display_allowed(-1),
        "display selection must permit unknown displays and only configured known displays");
    Require(first->font_excluded(L"Excluded") && !first->font_excluded(L"Included") &&
        first->font_excluded(L"included") && first->font_excluded(L"Other"),
        "font exclusion takes precedence over the case-sensitive include list");
    Require(first->process_excluded(false, false, nullptr) && !first->process_unload(false, nullptr),
        "exclude directories match descendants, while unload directories match exactly");
    Require(!first->process_excluded(true, false, nullptr) && first->process_excluded(true, true, nullptr),
        "forced exclusion precedes forced loading outside include-only mode");
    Require(first->raster().minHeight == 3 && first->raster().maxHeight == 90 &&
        first->raster().clipBoxFix && first->raster().shadowParams[3] == 4,
        "GDI limits and raw shadow parameters must be retained in the snapshot");

    candidate.admission.displayAffinity = {2};
    candidate.admission.includeOnly = true;
    const auto second = runtime.Publish(candidate).snapshot;
    Require(second && !second->display_allowed(1) && second->display_allowed(2) &&
        first->display_allowed(1) && !first->display_allowed(2),
        "reload replaces affinity rather than accumulating it and leaves retained readers intact");
    Require(second->process_excluded(false, false, nullptr) && second->process_unload(false, nullptr),
        "include-only directory rules must not admit descendants");
    Require(!second->process_excluded(true, true, nullptr) && !second->process_unload(true, nullptr),
        "include-only process admission preserves the force-load override");
    Require(second->exe_unload(L"child.exe", true),
        "legacy child include-only behavior must retain the force-load exclusion result");

    candidate.admission.displayAffinity.clear();
    candidate.admission.includeModules = {L"Child.EXE"};
    candidate.admission.unloadModules = {L"Blocked.EXE"};
    candidate.admission.unityIncludeGames = {L"C:\\Games\\Allowed.exe", L"C:\\Exact\\"};
    candidate.admission.unityExcludeGames = {L"Blocked.exe"};
    candidate.hooks.unityFontMode = renderer::UnityFontHookMode::selectedGames;
    const auto third = runtime.Publish(candidate).snapshot;
    Require(third && third->display_allowed(1) && third->display_allowed(2) && third->display_allowed(99),
        "an empty affinity reload must remove every prior display restriction");
    Require(!third->exe_unload(L"child.exe", false) && third->exe_unload(L"C:\\Child.exe", false),
        "child inclusion compares the complete supplied executable name case-insensitively");
    auto loaded = [](const wchar_t* name) { return std::wstring(name) == L"Child.EXE"; };
    Require(!third->process_excluded(false, false, loaded),
        "included loaded modules admit the process without mutable parser reads");
    Require(third->unity_font_enabled_for_executable(L"D:\\Other\\allowed.exe") &&
        third->unity_font_enabled_for_executable(L"C:\\Exact\\game.exe") &&
        !third->unity_font_enabled_for_executable(L"C:\\Exact\\child\\game.exe"),
        "Unity executable basename and exact-directory matching must remain unchanged");
    Require(first->digest() != second->digest() && second->digest() != third->digest(),
        "admission changes must contribute to the policy digest");

    candidate.admission.includeOnly = false;
    candidate.admission.includeFonts.clear();
    candidate.hooks.unityFontMode = renderer::UnityFontHookMode::allGames;
    const auto fourth = runtime.Publish(candidate).snapshot;
    Require(fourth->exe_unload(L"blocked.exe", false) && !fourth->exe_unload(L"blocked.exe", true) &&
        !fourth->font_excluded(L"Other") && fourth->font_excluded(L"Excluded"),
        "unload and font exclusion semantics must survive removal of include lists");
    Require(!fourth->unity_font_enabled_for_executable(L"D:\\blocked.exe") &&
        fourth->unity_font_enabled_for_executable(L"D:\\allowed.exe"),
        "all-games Unity policy must honor its immutable exclusion list");

    auto& process = renderer::ProcessProfileRuntime();
    Require(process.Publish(Candidate(1.0f, L"First")).published(), "process policy setup failed");
    const auto before = renderer::CurrentRendererPolicy();
    {
        const renderer::RendererPolicyScope outer(before);
        Require(process.Publish(Candidate(2.0f, L"Second")).published(), "policy reload failed");
        Require(renderer::CurrentRendererPolicy() == before,
            "nested raster readers must retain the outer call's generation during reload");
        const renderer::RendererPolicyScope inner(process.Load());
        Require(renderer::CurrentRendererPolicy() == before,
            "nested hooks must not replace the outer call's snapshot");
        bool seesLatest = false;
        std::thread other([&] { seesLatest = renderer::CurrentRendererPolicy() == process.Load(); });
        other.join();
        Require(seesLatest, "a retained call must not pin another thread's generation");
    }
    Require(renderer::CurrentRendererPolicy() == process.Load() && renderer::CurrentRendererPolicy() != before,
        "leaving the outer call must restore current-generation lookup");
}

void TestControlMessages()
{
    using renderer::ControlMessageKind;
    auto parse = [](const std::string& text) { return renderer::ParseControlMessage(text.data(), text.size()); };
    for (const char* invalid : {"", "{", "[]", "null", "{}", "{\"command\":1}",
        "{\"command\":\"unknown\"}", "{\"command\":\"loadprofile\"}",
        "{\"command\":\"loadprofile\",\"file\":false}",
        "{\"command\":\"loadprofile\",\"file\":\"\"}",
        "{\"command\":\"loadprofile\",\"file\":\"a\\u0000b\"}"}) {
        Require(parse(invalid).kind == ControlMessageKind::invalid,
            "malformed or wrongly typed control messages must cause no action");
    }
    const std::string ping = "{\"command\":\"ping\"}";
    Require(parse(ping).kind == ControlMessageKind::ping &&
        parse(ping + '\0').kind == ControlMessageKind::ping,
        "ping must accept counted and uncounted terminating NULs");
    Require(parse(ping + '\0' + "garbage").kind == ControlMessageKind::invalid,
        "an embedded NUL must not conceal trailing input");
    const auto load = parse("{\"command\":\"loadprofile\",\"file\":\"C:\\\\Temp\\\\preview.ini\"}");
    Require(load.kind == ControlMessageKind::loadProfile && load.path == "C:\\Temp\\preview.ini",
        "a valid temporary profile path must remain accepted");
    Require(renderer::ParseControlMessage(nullptr, 1).kind == ControlMessageKind::invalid &&
        renderer::ParseControlMessage(ping.data(), renderer::kMaxControlMessageBytes + 1).kind == ControlMessageKind::invalid,
        "null and oversized payloads must be rejected before any copy");
    std::string boundary = ping;
    boundary.resize(renderer::kMaxControlMessageBytes, ' ');
    Require(parse(boundary).kind == ControlMessageKind::ping, "the documented byte boundary must be accepted");
    boundary.push_back(' ');
    Require(parse(boundary).kind == ControlMessageKind::invalid, "the next byte must exceed the bound");
    Require(parse(std::string(32, '[') + "0" + std::string(32, ']')).kind == ControlMessageKind::invalid,
        "deeply nested input must be rejected without unbounded parser recursion");
}

} // namespace

int main()
{
    TestAdmissionAndReload();
    TestControlMessages();
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
                first.snapshot->raster().gamma == 1.25f &&
                first.snapshot->raster().gammaMode == 3 &&
                first.snapshot->raster().bolderMode == 2 &&
                first.snapshot->raster().useMapping &&
                first.snapshot->raster().substituteAllFonts &&
                first.snapshot->raster().shadowAlpha == 45 &&
                first.snapshot->raster().shadowLightAlpha == 67,
            "the raster view must retain its root policy generation");
    Require(first.snapshot->raster().fontLinks &&
                first.snapshot->raster().fontLinks->allowDefault[128] &&
                first.snapshot->raster().fontLinks
                        ->defaultFamilies[16] == L"Times New Roman" &&
                first.snapshot->raster().fontLinks->entries.size() == 1 &&
                first.snapshot->raster().fontLinks->entries[0]
                        .linkedFamilies[1] == L"SimSun",
            "the immutable raster view must retain the complete font-link policy");
    Require(first.snapshot->hooks().unityFontMode ==
            renderer::UnityFontHookMode::mostGames &&
            first.snapshot->hooks().unityFontEnabledForProcess,
        "the Unity hook selection must belong to the immutable policy");
    Require(first.snapshot->hooks().skipPrivateFreeType,
        "private FreeType avoidance must belong to the immutable hook policy");
    Require(first.snapshot->hooks().skipConsoleProcesses,
        "console avoidance must belong to the immutable hook policy");
    Require(first.snapshot->unity_coverage().gray[128] == 128 &&
            first.snapshot->unity_coverage().rgb[0][192] == 192 &&
            first.snapshot->unity_coverage().rgb[2][64] == 64,
        "identity profile tuning must preserve Unity glyph coverage");
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

    Require(first.snapshot->font_substitutions()->bold_mode() ==
                    renderer::font_substitution::BoldMode::sameFamily &&
                first.snapshot->font_substitutions()->bold_pairs().empty(),
            "a candidate without bold settings must publish same-family mode");

    renderer::ProfileRuntime boldRuntime;
    renderer::RendererPolicyCandidate boldCandidate =
        Candidate(1.25f, L"Courier New");
    boldCandidate.substitutionBoldMode = renderer::font_substitution::BoldMode::pairs;
    boldCandidate.substitutionBoldPairs.push_back({L"Courier New", L"Courier New Bold"});
    renderer::RendererPolicyCandidate boldChanged = boldCandidate;
    boldChanged.substitutionBoldMode = renderer::font_substitution::BoldMode::synthetic;
    const renderer::ProfilePublication boldPolicy =
        boldRuntime.Publish(std::move(boldCandidate));
    Require(boldPolicy.published() &&
                boldPolicy.snapshot->font_substitutions()->bold_mode() ==
                    renderer::font_substitution::BoldMode::pairs &&
                boldPolicy.snapshot->font_substitutions()->bold_pairs().size() == 1 &&
                boldPolicy.snapshot->font_substitutions()
                        ->FindBoldPair(L"courier new")
                        ->boldFamily == L"Courier New Bold" &&
                renderer::font_substitution::ProcessRegistry().Load()->bold_mode() ==
                    renderer::font_substitution::BoldMode::pairs,
            "the bold substitution mode and pairs must reach the published snapshot");
    const renderer::ProfilePublication boldModePolicy =
        boldRuntime.Publish(std::move(boldChanged));
    Require(boldModePolicy.published() &&
                boldModePolicy.snapshot->digest() != boldPolicy.snapshot->digest(),
            "the bold substitution mode must contribute to the root policy digest");
    renderer::RendererPolicyCandidate outOfRange = Candidate(1.25f, L"Courier New");
    outOfRange.substitutionBoldMode =
        static_cast<renderer::font_substitution::BoldMode>(4);
    Require(!boldRuntime.Publish(std::move(outOfRange)).published() &&
                boldRuntime.Load() == boldModePolicy.snapshot,
            "an out-of-range bold mode must preserve the prior snapshot");

    renderer::ProfileRuntime digestRuntime;
    renderer::RendererPolicyCandidate consoleEnabled =
        Candidate(1.25f, L"Courier New");
    renderer::RendererPolicyCandidate consoleDisabled = consoleEnabled;
    consoleDisabled.hooks.skipConsoleProcesses = false;
    const renderer::ProfilePublication enabledPolicy =
        digestRuntime.Publish(std::move(consoleEnabled));
    const renderer::ProfilePublication disabledPolicy =
        digestRuntime.Publish(std::move(consoleDisabled));
    Require(enabledPolicy.published() && disabledPolicy.published() &&
                enabledPolicy.snapshot->digest() != disabledPolicy.snapshot->digest(),
            "console avoidance must contribute to the root policy digest");

    renderer::RasterPolicy inFlightRaster =
        first.snapshot->raster();
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
    Require(inFlightRaster.generation == first.generation &&
                inFlightRaster.gamma == 1.25f &&
                inFlightRaster.gammaMode == 3 &&
                inFlightRaster.fontLinks &&
                inFlightRaster.fontLinks->allowDefault[128] &&
                inFlightRaster.fontLinks->entries[0].linkedFamilies[0] ==
                    L"Segoe UI",
            "a later snapshot revision must not alter in-flight raster policy");
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
                    !observed->display_allowed(observed->raster().gamma == 1.25f ? 1 : 2) ||
                    observed->display_allowed(observed->raster().gamma == 1.25f ? 2 : 1) ||
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

#include "../../../settings_snapshot.h"
#include "../../../hash_list.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <iostream>
#include <map>
#include <new>
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

std::wstring Lowercase(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), ::towlower);
    return value;
}

void TestAdmissionDecisions()
{
    settings_snapshot::AdmissionLists lists;
    int queries = 0;
    Require(lists.AllowsDisplay([&] { ++queries; return 1; }) && queries == 0,
        "empty affinity must not query the display");
    lists.displayAffinity.insert(1);
    Require(lists.AllowsDisplay([&] { ++queries; return 1; }) && queries == 1,
        "a listed display must be allowed");
    Require(!lists.AllowsDisplay([] { return 2; }), "an unlisted display must be excluded");
    Require(lists.AllowsDisplay([] { return -1; }), "an unknown display must remain allowed");

    lists.excludeModules.insert(L"C:\\Games\\");
    const auto directory = [] { return std::wstring(L"c:\\games\\child\\"); };
    int moduleQueries = 0;
    const auto absent = [&](const wchar_t*) { ++moduleQueries; return false; };
    Require(settings_snapshot::MatchesProcess(lists.excludeModules, directory, true, Lowercase, absent),
        "excluded folders match descendant directories");
    Require(!settings_snapshot::MatchesProcess(lists.excludeModules, directory, false, Lowercase, absent),
        "include and unload folders must not match a descendant");
    Require(settings_snapshot::MatchesProcess(lists.excludeModules,
        [] { return std::wstring(L"c:\\games\\"); }, false, Lowercase, absent),
        "include and unload folders match their exact directory");
    Require(moduleQueries == 0, "folder entries must not query modules");
    Require(!settings_snapshot::FolderMatches(L"c:\\games-extra\\", L"c:\\games\\", true),
        "a folder prefix must preserve the trailing separator");

    lists.includeModules.insert(L"Example.EXE");
    int directoryQueries = 0;
    Require(settings_snapshot::MatchesProcess(lists.includeModules,
        [&] { ++directoryQueries; return std::wstring(); }, false, Lowercase,
        [](const wchar_t* name) { return lstrcmpiW(name, L"example.exe") == 0; }),
        "named modules must use the supplied module observation");
    Require(directoryQueries == 0, "named modules must not query the image directory");
    Require(settings_snapshot::MatchesExecutable(lists.includeModules, L"example.exe",
        [](const wchar_t* left, const wchar_t* right) { return lstrcmpiW(left, right) == 0; }),
        "executable matching must retain case-insensitive name comparison");
    Require(!settings_snapshot::MatchesExecutable(lists.includeModules, L"example.exe.bak",
        [](const wchar_t* left, const wchar_t* right) { return lstrcmpiW(left, right) == 0; }),
        "executable names must match exactly");
}

void TestFontFilters()
{
    settings_snapshot::FontFilterLists filters;
    Require(!filters.IsExcluded(L"Family"), "empty filters must admit fonts");
    filters.includeFonts.insert(L"Family");
    Require(!filters.IsExcluded(L"Family") && filters.IsExcluded(L"Other"),
        "a nonempty include list is an allow-list");
    Require(filters.IsExcluded(L"family"), "font filter lookup must retain exact case semantics");
    filters.excludeFonts.insert(L"Family");
    Require(filters.IsExcluded(L"Family"), "an exclusion must override an include entry");

    std::shared_ptr<const settings_snapshot::FontFilterLists> published =
        std::make_shared<const settings_snapshot::FontFilterLists>(filters);
    const auto retained = std::atomic_load(&published);
    filters.excludeFonts.clear();
    filters.includeFonts.clear();
    Require(std::atomic_load(&published)->IsExcluded(L"Family"),
        "clearing writer staging must not clear the published filter");
    std::atomic_store(&published,
        std::make_shared<const settings_snapshot::FontFilterLists>(filters));
    Require(!std::atomic_load(&published)->IsExcluded(L"Family") && retained->IsExcluded(L"Family"),
        "replacing the current owner must leave retained font filters intact");
}

struct TestSubstitutionTable
{
    std::map<std::wstring, std::wstring> replacements;
};

struct TestFontLinkTable
{
    std::vector<std::wstring> families;
    std::wstring defaultFamily;
    bool charsetAllowed = true;
    static bool failDefaultCopy;

    void CopyDefaultsFrom(const TestFontLinkTable& previous)
    {
        if (failDefaultCopy)
            throw std::bad_alloc();
        defaultFamily = previous.defaultFamily;
        charsetAllowed = previous.charsetAllowed;
    }
};

bool TestFontLinkTable::failDefaultCopy = false;

void TestReloadResetWithoutLaterInitialization()
{
    auto oldFilters = std::make_shared<settings_snapshot::FontFilterLists>();
    oldFilters->includeFonts.insert(L"A");
    auto oldSubstitutes = std::make_shared<TestSubstitutionTable>();
    oldSubstitutes->replacements.emplace(L"A", L"B");
    auto oldLinks = std::make_shared<TestFontLinkTable>();
    oldLinks->families.push_back(L"Linked family");
    oldLinks->defaultFamily = L"Default family";
    oldLinks->charsetAllowed = false;

    std::shared_ptr<const settings_snapshot::FontFilterLists> filters = oldFilters;
    std::shared_ptr<const TestSubstitutionTable> substitutes = oldSubstitutes;
    std::shared_ptr<const TestFontLinkTable> links = oldLinks;
    const auto retainedFilters = std::atomic_load(&filters);
    const auto retainedSubstitutes = std::atomic_load(&substitutes);
    const auto retainedLinks = std::atomic_load(&links);

    settings_snapshot::ResetFontLists(filters, substitutes, links);
    // No subsequent publication models either early return from DelayedInit.
    Require(!std::atomic_load(&filters)->IsExcluded(L"Other"),
        "failed later initialization must not keep the preceding font allow-list active");
    Require(std::atomic_load(&substitutes)->replacements.empty(),
        "a reload that disables substitution must not keep A-to-B active after initialization fails");
    const auto currentLinks = std::atomic_load(&links);
    Require(currentLinks->families.empty() && currentLinks->defaultFamily == L"Default family" &&
        !currentLinks->charsetAllowed,
        "reload must clear family rows while retaining legacy clear's default metadata");
    Require(retainedFilters->IsExcluded(L"Other") &&
        retainedSubstitutes->replacements.at(L"A") == L"B" &&
        retainedLinks->families.size() == 1 && retainedLinks->families[0] == L"Linked family",
        "reload must replace owners without mutating an in-flight reader's tables");
    Require(filters != retainedFilters && substitutes != retainedSubstitutes && links != retainedLinks,
        "each current owner must be replaced even if delayed initialization never publishes");

    settings_snapshot::ResetFontLists(filters, substitutes, links);
    Require(filters && substitutes && links && links->families.empty() &&
        links->defaultFamily == L"Default family" && !links->charsetAllowed,
        "repeated disabled reloads must remain nonnull and retain default metadata");
}

void TestReloadResetPreparationFailure()
{
    auto originalFilters = std::make_shared<settings_snapshot::FontFilterLists>();
    originalFilters->excludeFonts.insert(L"A");
    auto originalSubstitutes = std::make_shared<TestSubstitutionTable>();
    originalSubstitutes->replacements.emplace(L"A", L"B");
    auto originalLinks = std::make_shared<TestFontLinkTable>();
    originalLinks->families.push_back(L"Linked family");
    std::shared_ptr<const settings_snapshot::FontFilterLists> filters = originalFilters;
    std::shared_ptr<const TestSubstitutionTable> substitutes = originalSubstitutes;
    std::shared_ptr<const TestFontLinkTable> links = originalLinks;

    TestFontLinkTable::failDefaultCopy = true;
    bool threw = false;
    try {
        settings_snapshot::ResetFontLists(filters, substitutes, links);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    TestFontLinkTable::failDefaultCopy = false;
    Require(threw && filters == originalFilters && substitutes == originalSubstitutes && links == originalLinks,
        "preparation failure must not partially replace published owners");

    filters.reset();
    substitutes.reset();
    links.reset();
    settings_snapshot::ResetFontLists(filters, substitutes, links);
    Require(filters && substitutes && links && filters->excludeFonts.empty() &&
        substitutes->replacements.empty() && links->families.empty() &&
        links->defaultFamily.empty() && links->charsetAllowed,
        "cold initialization must publish nonnull empty owners with constructor defaults");
}

void TestConcurrentPublication()
{
    auto a = std::make_shared<settings_snapshot::AdmissionLists>();
    a->displayAffinity.insert(1);
    a->includeModules.insert(L"a.exe");
    a->useInclude = true;
    auto b = std::make_shared<settings_snapshot::AdmissionLists>();
    b->displayAffinity.insert(2);
    b->unloadModules.insert(L"b.exe");
    std::shared_ptr<const settings_snapshot::AdmissionLists> first = a;
    std::shared_ptr<const settings_snapshot::AdmissionLists> second = b;
    std::shared_ptr<const settings_snapshot::AdmissionLists> published = first;
    const auto retained = std::atomic_load(&published);
    std::atomic_store(&published, second);
    Require(!std::atomic_load(&published)->AllowsDisplay([] { return 1; }) &&
        retained->AllowsDisplay([] { return 1; }),
        "new affinity must replace the old list without changing an in-flight decision");

    std::atomic<bool> start{false};
    std::vector<std::thread> readers;
    for (int thread = 0; thread < 8; ++thread) {
        readers.emplace_back([&] {
            while (!start.load())
                std::this_thread::yield();
            for (int iteration = 0; iteration < 2000; ++iteration) {
                const auto current = std::atomic_load(&published);
                if (current->useInclude) {
                    Require(current->includeModules.count(L"a.exe") == 1 &&
                        current->unloadModules.empty() &&
                        current->AllowsDisplay([] { return 1; }) &&
                        !current->AllowsDisplay([] { return 2; }),
                        "one decision must retain all fields of admission A");
                } else {
                    Require(current->includeModules.empty() &&
                        current->unloadModules.count(L"b.exe") == 1 &&
                        current->AllowsDisplay([] { return 2; }) &&
                        !current->AllowsDisplay([] { return 1; }),
                        "one decision must retain all fields of admission B");
                }
            }
        });
    }
    start.store(true);
    for (int iteration = 0; iteration < 4000; ++iteration)
        std::atomic_store(&published, iteration % 2 == 0 ? first : second);
    for (auto& reader : readers)
        reader.join();
    Require(retained->includeModules.count(L"a.exe") == 1,
        "a retained snapshot must survive repeated replacement");
}

void TestConcurrentFontNameCache()
{
    CHashedStringList cache;
    wchar_t family[] = L"Family";
    wchar_t value[] = L"Original";
    cache.Add(family, value);
    wchar_t* const retained = cache.Find(family);
    Require(retained != nullptr, "the initial cache entry must exist");
    std::vector<std::thread> workers;
    for (int thread = 0; thread < 8; ++thread) {
        workers.emplace_back([&, thread] {
            wchar_t duplicate[] = L"FAMILY";
            wchar_t replacement[] = L"Must not replace";
            for (int iteration = 0; iteration < 128; ++iteration) {
                cache.Add(duplicate, replacement);
                Require(cache.Find(duplicate) == retained && wcscmp(retained, L"Original") == 0,
                    "duplicate additions must preserve the pointer and its immutable value");
                std::wstring key = L"key-" + std::to_wstring(thread) + L"-" + std::to_wstring(iteration);
                cache.Add(&key[0], replacement);
                const wchar_t* stored = cache.Find(&key[0]);
                Require(stored && wcscmp(stored, replacement) == 0,
                    "concurrent independent entries must remain retrievable");
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    Require(cache.Find(family) == retained && wcscmp(retained, L"Original") == 0,
        "map growth must not invalidate a borrowed name pointer");

    CHashedStringList sensitive(TRUE);
    wchar_t lower[] = L"family";
    sensitive.Add(family, value);
    Require(sensitive.Find(lower) == nullptr, "case-sensitive caches must remain case-sensitive");
}

} // namespace

int main()
{
    TestAdmissionDecisions();
    TestFontFilters();
    TestReloadResetWithoutLaterInitialization();
    TestReloadResetPreparationFailure();
    TestConcurrentPublication();
    TestConcurrentFontNameCache();
    std::cout << "Settings snapshot and name-cache tests passed\n";
    return 0;
}

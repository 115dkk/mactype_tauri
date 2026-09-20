#include "../../../renderer/gdi_font_identity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <iostream>
#include <cwchar>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool OnlyQt5GuiLoaded(const wchar_t* name) noexcept
{
    return std::wcscmp(name, L"Qt5Gui.dll") == 0;
}

bool NothingLoaded(const wchar_t*) noexcept
{
    return false;
}

bool OnlyQt5CoreLoaded(const wchar_t* name) noexcept
{
    return std::wcscmp(name, L"Qt5Core.dll") == 0;
}

} // namespace

int main()
{
    using renderer::gdi_font_identity::CommitsStockIdentity;
    using renderer::gdi_font_identity::GdiObjectsAtHookInstallForTesting;
    using renderer::gdi_font_identity::GlyphIndexTextStackLoaded;
    using renderer::gdi_font_identity::IsGlyphIndexTextStackModule;
    using renderer::gdi_font_identity::RecordHookInstall;
    using renderer::gdi_font_identity::ResetForTesting;
    using renderer::gdi_font_identity::StockIdentityCommitted;

    Require(IsGlyphIndexTextStackModule(L"Qt5Gui.dll"),
        "Qt5 GUI was not recognized");
    Require(IsGlyphIndexTextStackModule(L"qt5gui.DLL"),
        "Qt5 GUI matching was case-sensitive");
    Require(IsGlyphIndexTextStackModule(L"Qt6Gui.dll"),
        "Qt6 GUI was not recognized");
    Require(IsGlyphIndexTextStackModule(L"QtGui4.dll"),
        "Qt4 GUI was not recognized");
    Require(IsGlyphIndexTextStackModule(L"Qt5Guid.dll"),
        "Qt5 debug GUI was not recognized");
    Require(!IsGlyphIndexTextStackModule(L"Qt5Core.dll"),
        "Qt5 Core was mistaken for a GUI text stack");
    Require(!IsGlyphIndexTextStackModule(L"Qt5Gui.dll.bak"),
        "a suffixed Qt5 GUI name was accepted");
    Require(!IsGlyphIndexTextStackModule(L"Gui.dll"),
        "an unrelated GUI module was accepted");
    Require(!IsGlyphIndexTextStackModule(L""),
        "an empty module name was accepted");
    Require(!IsGlyphIndexTextStackModule(nullptr),
        "a null module name was accepted");

    Require(!CommitsStockIdentity(0, false),
        "an empty process committed without a text stack");
    Require(!CommitsStockIdentity(0, true),
        "an empty process committed with a text stack");
    Require(!CommitsStockIdentity(5, false),
        "GDI objects committed without a text stack");
    Require(CommitsStockIdentity(5, true),
        "existing GDI objects and a text stack did not commit");
    Require(CommitsStockIdentity(1, true),
        "one existing GDI object did not commit");

    Require(GlyphIndexTextStackLoaded(OnlyQt5GuiLoaded),
        "the module probe missed Qt5 GUI");
    Require(!GlyphIndexTextStackLoaded(NothingLoaded),
        "the empty module probe reported a text stack");
    Require(!GlyphIndexTextStackLoaded(OnlyQt5CoreLoaded),
        "the module probe queried an unrelated Qt module");

    ResetForTesting();
    RecordHookInstall();
    Require(GdiObjectsAtHookInstallForTesting() == 0,
        "the fresh test process already owned a GDI object");
    Require(!StockIdentityCommitted(),
        "the fresh test process committed stock identity");

    LOGFONTW firstDescription{};
    HFONT const firstFont = CreateFontIndirectW(&firstDescription);
    Require(firstFont != nullptr, "the first test font could not be created");
    ResetForTesting();
    RecordHookInstall();
    unsigned long const firstSample = GdiObjectsAtHookInstallForTesting();
    Require(firstSample >= 1,
        "the first test font was absent from the GDI sample");
    Require(!StockIdentityCommitted(),
        "a process without a Qt GUI module committed stock identity");

    LOGFONTW secondDescription{};
    secondDescription.lfHeight = 12;
    HFONT const secondFont = CreateFontIndirectW(&secondDescription);
    Require(secondFont != nullptr, "the second test font could not be created");
    RecordHookInstall();
    Require(GdiObjectsAtHookInstallForTesting() == firstSample,
        "a second hook-install call replaced the first sample");

    DeleteObject(secondFont);
    DeleteObject(firstFont);

    std::cout << "GDI font identity: 10 module names, 5 decisions, "
        "3 probe checks, 4 process-seam checks passed\n";
    return 0;
}

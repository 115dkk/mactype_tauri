#include "../../../renderer/gdi_font_identity.h"
#include "../../../renderer/arrival_evidence.h"

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

std::uint32_t NoGdiObjects() noexcept
{
    return 0;
}

std::uint32_t ExistingGdiObjects() noexcept
{
    return 2;
}

bool FalseSample() noexcept
{
    return false;
}

renderer::arrival_evidence::Samplers EvidenceSamplers(
    std::uint32_t (*gdiObjects)() noexcept)
{
    return renderer::arrival_evidence::Samplers{
        gdiObjects,
        FalseSample,
        FalseSample,
        FalseSample,
    };
}

} // namespace

int main()
{
    using renderer::gdi_font_identity::CommitsStockIdentity;
    using renderer::gdi_font_identity::GlyphIndexTextStackLoaded;
    using renderer::gdi_font_identity::IsGlyphIndexTextStackModule;
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

    renderer::arrival_evidence::ResetForTests();
    ResetForTesting();
    renderer::arrival_evidence::Record(EvidenceSamplers(NoGdiObjects));
    Require(!StockIdentityCommitted(),
        "zero recorded GDI objects committed stock identity");

    renderer::arrival_evidence::ResetForTests();
    ResetForTesting();
    renderer::arrival_evidence::Record(EvidenceSamplers(ExistingGdiObjects));
    Require(!StockIdentityCommitted(),
        "recorded GDI objects committed without a Qt GUI module");

    std::cout << "GDI font identity: 10 module names, 5 decisions, "
        "3 probe checks, 2 evidence checks passed\n";
    return 0;
}

#include "../../../renderer/arrival_evidence.h"

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

std::uint32_t FirstGdiObjects() noexcept
{
    return 17;
}

bool FirstDwriteMapped() noexcept
{
    return true;
}

bool FirstDwriteCoreMapped() noexcept
{
    return false;
}

bool FirstVisibleWindow() noexcept
{
    return true;
}

std::uint32_t SecondGdiObjects() noexcept
{
    return 91;
}

bool SecondDwriteMapped() noexcept
{
    return false;
}

bool SecondDwriteCoreMapped() noexcept
{
    return true;
}

bool SecondVisibleWindow() noexcept
{
    return false;
}

bool deferredVisibleSampled = false;

bool DeferredVisibleWindow() noexcept
{
    deferredVisibleSampled = true;
    return true;
}

} // namespace

int main()
{
    namespace arrival = renderer::arrival_evidence;

    arrival::ResetForTests();
    Require(arrival::Recorded() == nullptr,
        "arrival evidence existed before Record");

    arrival::Samplers const firstSamplers{
        FirstGdiObjects,
        FirstDwriteMapped,
        FirstDwriteCoreMapped,
        FirstVisibleWindow,
    };
    arrival::ArrivalEvidence const& first = arrival::Record(firstSamplers);
    Require(first.gdiObjectsAtInstall == 17,
        "Record lost the GDI object sample");
    Require(first.dwriteMappedBeforePin,
        "Record lost the dwrite.dll mapping sample");
    Require(!first.dwriteCoreMappedBeforePin,
        "Record changed the DWriteCore.dll mapping sample");
    Require(first.visibleTopLevelWindowExisted,
        "Record lost the visible-window sample");
    Require(arrival::Recorded() == &first,
        "Recorded did not publish the first record");

    arrival::Samplers const secondSamplers{
        SecondGdiObjects,
        SecondDwriteMapped,
        SecondDwriteCoreMapped,
        SecondVisibleWindow,
    };
    arrival::ArrivalEvidence const& second = arrival::Record(secondSamplers);
    Require(&second == &first,
        "a second Record returned a different record");
    Require(second.gdiObjectsAtInstall == 17 &&
            second.dwriteMappedBeforePin &&
            !second.dwriteCoreMappedBeforePin &&
            second.visibleTopLevelWindowExisted,
        "a second Record replaced the first samples");

    arrival::ResetForTests();
    Require(arrival::Recorded() == nullptr,
        "ResetForTests did not clear the record");

    deferredVisibleSampled = false;
    arrival::Samplers const deferredSamplers{
        FirstGdiObjects,
        FirstDwriteMapped,
        FirstDwriteCoreMapped,
        DeferredVisibleWindow,
    };
    arrival::Record(deferredSamplers);
    Require(deferredVisibleSampled,
        "injected samplers must remain synchronous");
    arrival::CompleteDeferredSamples();
    Require(arrival::Recorded()->visibleTopLevelWindowExisted,
        "CompleteDeferredSamples changed a completed injected sample");

    std::cout << "Arrival evidence tests passed.\n";
    return 0;
}

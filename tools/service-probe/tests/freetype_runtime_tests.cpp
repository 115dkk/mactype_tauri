#include "../../../renderer/freetype_runtime.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <vector>

namespace {

std::vector<int> releases;

struct RecordingDelete
{
    void operator()(int* value) const noexcept
    {
        releases.push_back(*value);
        delete value;
    }
};

using Owner = std::unique_ptr<int, RecordingDelete>;

struct StreamBackingProbe
{
    explicit StreamBackingProbe(int* destructionCount)
        : destructionCount(destructionCount)
    {
    }
    ~StreamBackingProbe() { ++*destructionCount; }

    renderer::freetype::StreamBackingOwnership ownership;
    int* destructionCount;
};

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using renderer::freetype::BoundedStreamReadSize;
    Require(BoundedStreamReadSize(100, 20, 40) == 40,
            "an in-range stream read must preserve its requested size");
    Require(BoundedStreamReadSize(100, 80, 40) == 20,
            "a stream read must be clipped at the byte boundary");
    Require(BoundedStreamReadSize(100, 100, 1) == 0,
            "a read at end-of-stream must return no bytes");
    Require(BoundedStreamReadSize(100, 101, 1) == 0,
            "an out-of-range offset must not unsigned-underflow");
    Require(renderer::freetype::BitmapByteSize(12, 4) == 48,
            "a top-down bitmap must report its complete byte charge");
    Require(renderer::freetype::BitmapByteSize(-12, 4) == 48,
            "a bottom-up bitmap pitch must not produce a negative cache charge");
    Require(renderer::freetype::BitmapByteSize(-12, 0) == 0,
            "an empty bitmap must not consume cache bytes");

    unsigned char bitmap[12]{};
    auto row = renderer::freetype::CheckedBitmapRow(bitmap, 4, 3, 0, 4);
    Require(row && row.data == bitmap && row.size == 4,
            "positive pitch must expose its first logical row at the buffer start");
    row = renderer::freetype::CheckedBitmapRow(bitmap, 4, 3, 2, 4);
    Require(row && row.data == bitmap + 8,
            "positive pitch must advance down through the backing buffer");
    row = renderer::freetype::CheckedBitmapRow(bitmap, -4, 3, 0, 4);
    Require(row && row.data == bitmap + 8,
            "negative pitch must expose the physical last row as the logical top");
    row = renderer::freetype::CheckedBitmapRow(bitmap, -4, 3, 2, 4);
    Require(row && row.data == bitmap,
            "negative pitch must finish at the physical first row");
    Require(!renderer::freetype::CheckedBitmapRow(bitmap, 4, 3, 3, 1),
            "a row index at the bitmap boundary must be rejected");
    Require(!renderer::freetype::CheckedBitmapRow(bitmap, 4, 3, 0, 5),
            "a consumer wider than the physical row must be rejected");
    Require(!renderer::freetype::CheckedBitmapRow(nullptr, 4, 3, 0, 1),
            "a non-empty row must reject a null backing buffer");
    const auto nearAddressLimit = reinterpret_cast<const unsigned char*>(
        std::numeric_limits<std::uintptr_t>::max() - 1u);
    Require(!renderer::freetype::CheckedBitmapRow(
                nearAddressLimit, 4, 1, 0, 1),
            "a returned row span must not wrap past the address boundary");

    using renderer::freetype::InvokeFreeTypeCallbackBoundary;
    Require(InvokeFreeTypeCallbackBoundary(
                [] { return 17; }, 12, 13) == 17,
            "a successful FreeType callback must preserve its result");
    Require(InvokeFreeTypeCallbackBoundary(
                []() -> int { throw std::bad_alloc(); }, 12, 13) == 12,
            "allocation failure must stay inside the FreeType C callback boundary");
    Require(InvokeFreeTypeCallbackBoundary(
                []() -> int { throw std::runtime_error("callback failure"); },
                12, 13) == 13,
            "an unexpected C++ exception must stay inside the FreeType C callback boundary");

    auto faceIndex = renderer::freetype::CheckFaceIndex(1, 2);
    Require(faceIndex.valid && faceIndex.value == 0,
            "face id one must address the first font");
    faceIndex = renderer::freetype::CheckFaceIndex(2, 2);
    Require(faceIndex.valid && faceIndex.value == 1,
            "the upper one-based face id must remain valid");
    Require(!renderer::freetype::CheckFaceIndex(0, 2).valid,
            "face id zero must not underflow to the end of the font vector");
    Require(!renderer::freetype::CheckFaceIndex(-1, 2).valid,
            "negative face ids must be rejected");
    Require(!renderer::freetype::CheckFaceIndex(3, 2).valid,
            "face ids above the font count must be rejected");

    int streamBackingDestructions = 0;
    {
        std::unique_ptr<StreamBackingProbe> builder(
            new StreamBackingProbe(&streamBackingDestructions));
        Require(!builder->ownership.ReclaimFromCloseCallback(),
                "a failed FT_Open_Face close callback must not delete builder-owned backing");
        Require(builder->ownership.TransferToFace(),
                "a complete face must accept exactly one backing transfer");
        Require(!builder->ownership.TransferToFace(),
                "stream backing ownership must not transfer twice");
        StreamBackingProbe* callbackBacking = builder.release();
        Require(callbackBacking->ownership.callback_owned(),
                "released stream backing must be visibly callback-owned");
        Require(callbackBacking->ownership.ReclaimFromCloseCallback(),
                "the face close callback must reclaim transferred backing");
        std::unique_ptr<StreamBackingProbe> reclaimed(callbackBacking);
    }
    Require(streamBackingDestructions == 1,
            "callback-owned stream backing must be destroyed exactly once");

    const renderer::freetype::RasterCacheKey normal{12, 0, 0, false};
    const renderer::freetype::RasterCacheKey wide{12, 32, 0, false};
    const renderer::freetype::RasterCacheKey bold{12, 0, 1, false};
    const renderer::freetype::RasterCacheKey italic{12, 0, 0, true};
    std::set<renderer::freetype::RasterCacheKey> rasterKeys{
        normal, wide, bold, italic};
    Require(rasterKeys.size() == 4,
            "typed raster cache keys must preserve width, weight, and italic policy");
    Require(normal == renderer::freetype::RasterCacheKey{12, 0, 0, false},
            "equivalent raster policy must reuse one cache key");

    releases.clear();
    {
        renderer::freetype::OrderedRuntimeOwners<Owner, Owner> runtime;
        runtime.Publish(Owner(new int(1)), Owner(new int(2)));
        Require(runtime.initialized(), "a complete FreeType owner pair must be initialized");
        runtime.Publish(Owner(new int(3)), Owner(new int(4)));
        Require(releases == std::vector<int>({2, 1}),
                "republishing must release the old manager before its library");
        runtime.Reset();
        Require(releases == std::vector<int>({2, 1, 4, 3}),
                "reset must release the manager before its library");
        runtime.Reset();
    }
    Require(releases == std::vector<int>({2, 1, 4, 3}),
            "idempotent reset must not release an owner twice");

    std::cout << "FreeType runtime ownership tests passed.\n";
    return 0;
}

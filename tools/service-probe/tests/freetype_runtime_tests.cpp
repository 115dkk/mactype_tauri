#include "../../../renderer/freetype_runtime.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
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

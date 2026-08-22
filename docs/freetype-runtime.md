# FreeType runtime contract

## Purpose

This document records the FreeType boundary used by
`codex/alpha-plus-dll`. It separates facts enforced by the build from rendering
behaviour that still needs application or golden-image evidence. The renderer
must not silently replace this dependency with upstream FreeType or infer ABI
compatibility from a version number alone.

## Pinned dependency

| Property | Required value |
| --- | --- |
| Repository | `snowie2000/freetype` |
| Commit | `ef771574d04721baf45a1b66bfb4692193603088` |
| Header version | FreeType 2.14.3 |
| Linkage | Static, Visual C++ multithreaded runtime (`/MT`) |
| Optional libraries | Brotli, BZip2, HarfBuzz, PNG, and Zlib disabled by the open-core build |
| Subpixel renderer | `FT_CONFIG_OPTION_SUBPIXEL_RENDERING` required |

The fork exposes a private extension that ordinary upstream FreeType does not:

```cpp
FT_Error FT_Glyph_To_BitmapEx(
    FT_Glyph* glyph,
    FT_Render_Mode renderMode,
    FT_Vector* origin,
    FT_Bool destroy,
    FT_Bool loadColor,
    FT_UInt glyphIndex,
    FT_Face face);
```

`Build-OpenCore.ps1` validates the pinned commit, version tokens, subpixel
option, function name, and the `loadcolor`, `glyphindex`, and `face` parameters
before compiling either architecture. A matching tag without this exact ABI is
not a valid replacement.

## Ownership and initialization

`renderer/freetype_runtime.h` defines the narrow runtime interface used by the
legacy renderer implementation.

- `OrderedRuntimeOwners` publishes an `FT_Library` and `FTC_Manager` only as a
  complete pair and always releases the manager before its library.
- `FontLInit` builds new owners transactionally, publishes them only after both
  are valid, and is idempotent across a retry.
- `FontLFree`, the font-engine owner, and thread-local bitmap-cache reset paths
  are idempotent. No code calls a C++ destructor manually.
- `BoundedStreamReadSize` rejects offsets at or beyond the source size before
  subtraction, preventing unsigned wraparound in font-backed streams.
- `BitmapByteSize` uses the absolute bitmap pitch and saturates its public
  `int` charge, so bottom-up bitmaps cannot create a negative or wrapped cache
  size.

Focused tests inject recording owners to prove manager-before-library release,
republish cleanup, repeated reset, stream boundaries, and signed bitmap pitch.
Both Win32 and x64 targets are part of the service-probe contract and the ASan
matrix.

## Cache and render-policy boundary

The renderer now uses a typed `RasterCacheKey` containing height, width,
weight class, and italic state. Size-cache entries are owned by
`std::unique_ptr`, and capacity pressure evicts the least-recently-used entry
rather than whichever key happens to sort first.

`RasterPolicy` is copied once for a render request. Reloading settings can
publish a new policy for later work, but one glyph run cannot observe a mixture
of old and new font-loader, linking, bitmap, weight, LCD, color, inversion,
gamma, or shadow values.

These changes make lifetime and cache bounds deterministic. They do not claim
a faster rasterizer or a different preferred image. Any future cache threshold
or raster-policy change needs before/after allocation, working-set, CPU, and
pixel evidence.

## Existing format paths

The following paths are retained and made safer; this tranche does not present
them as newly invented support.

- Variable fonts use `FT_Get_MM_Var`, RAII ownership for `FT_MM_Var`, the
  `wght` design axis, and named instances. DirectWrite virtual-font references
  also preserve `IDWriteFontFaceReference1` axis values through
  `IDWriteFactory6` when those interfaces exist.
- TrueType Collections preserve the TTC container and selected face index in
  both FreeType streams and the coherent DirectWrite virtual-font builder.
- Vertical CJK text uses GSUB `vert`/`vrt2` lookup and
  `FT_LOAD_VERTICAL_LAYOUT` where the existing path selects vertical glyphs.
- Color loading is gated by the profile and `FT_HAS_COLOR`, then passed through
  the fork's private bitmap conversion extension.

The build disables PNG and Zlib support and does not configure an SVG renderer.
Therefore bitmap/SVG color-font coverage must be reported by the exact table
format exercised; “color fonts supported” is too broad. Current Windows font
files, high-DPI output, CJK fallback quality, and visual colour behaviour stay
`UNKNOWN` until retained golden-image evidence exists.

## Sanitizer boundary

The repeatable ASan lane instruments the ownership, stream, cache-key, policy,
hook-lifecycle, and substitution modules with the correct x86 or x64 MSVC ASan
runtime. It is a module-level memory-safety gate.

The complete injected core is not an ASan artifact. Building only the core
with `/fsanitize=address` fails at link time because the stock static IniParser
and other C++ dependencies have incompatible STL annotation settings. Full-DLL
instrumentation is allowed only after every linked C++ dependency is rebuilt
with matching compiler, runtime, iterator-debug, and sanitizer settings. Until
then, documentation and releases must not describe the browser, service host,
or entire core as ASan-certified.

Application Verifier and UMDH remain a disposable-lab procedure. They are not
run on a developer machine or hosted runner merely to turn an evidence cell
green, because their configuration is machine-global and the complete tools
are not present in the current environment.

## Upgrade rule

A FreeType update is justified only by a reproduced defect, security fix, or
measured rendering benefit. The candidate must retain or deliberately replace
the private bitmap ABI, pass both architecture builds and focused runtime
tests, and supply golden images plus performance/memory comparisons for every
intended raster difference. In the absence of that evidence, keeping the
pinned 2.14.3 fork is the safer completed decision.

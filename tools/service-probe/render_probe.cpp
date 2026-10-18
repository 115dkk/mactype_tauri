#include "render_probe.h"

#include <sdkddkver.h>
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_NI

#include "win32_support.h"

#include <bcrypt.h>
#include <dwrite_3.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mactype::service_probe::internal {
namespace {

struct GdiObjectDeleter {
  void operator()(HGDIOBJ object) const noexcept {
    if (object != nullptr) {
      DeleteObject(object);
    }
  }
};

struct DcDeleter {
  void operator()(HDC dc) const noexcept {
    if (dc != nullptr) {
      DeleteDC(dc);
    }
  }
};

using UniqueGdiObject =
    std::unique_ptr<std::remove_pointer_t<HGDIOBJ>, GdiObjectDeleter>;
using UniqueDc = std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter>;

template <typename Interface>
struct ComReleaser {
  void operator()(Interface* value) const noexcept {
    if (value != nullptr) {
      value->Release();
    }
  }
};

template <typename Interface>
using UniqueCom = std::unique_ptr<Interface, ComReleaser<Interface>>;

class ScopedFontTable final {
 public:
  ScopedFontTable(IDWriteFontFace* face, void* context,
                  const bool acquired) noexcept
      : face_(face), context_(context), acquired_(acquired) {}

  ~ScopedFontTable() noexcept {
    if (face_ != nullptr && acquired_) {
      face_->ReleaseFontTable(context_);
    }
  }

  ScopedFontTable(const ScopedFontTable&) = delete;
  ScopedFontTable& operator=(const ScopedFontTable&) = delete;

 private:
  IDWriteFontFace* face_;
  void* context_;
  bool acquired_;
};

enum class FontTableComparison {
  unavailable,
  same_missing,
  same_present,
  different_present,
};

FontTableComparison CompareFontTables(IDWriteFontFace* expected,
                                      IDWriteFontFace* actual,
                                      const UINT32 table_tag) {
  if (expected == nullptr || actual == nullptr) {
    return FontTableComparison::unavailable;
  }
  const void* expected_data = nullptr;
  UINT32 expected_size = 0;
  void* expected_context = nullptr;
  BOOL expected_exists = FALSE;
  const HRESULT expected_result = expected->TryGetFontTable(
      table_tag, &expected_data, &expected_size, &expected_context,
      &expected_exists);
  if (FAILED(expected_result)) {
    return FontTableComparison::unavailable;
  }
  ScopedFontTable expected_table(
      expected, expected_context, expected_exists != FALSE);

  const void* actual_data = nullptr;
  UINT32 actual_size = 0;
  void* actual_context = nullptr;
  BOOL actual_exists = FALSE;
  const HRESULT actual_result = actual->TryGetFontTable(
      table_tag, &actual_data, &actual_size, &actual_context, &actual_exists);
  if (FAILED(actual_result)) {
    return FontTableComparison::unavailable;
  }
  ScopedFontTable actual_table(actual, actual_context, actual_exists != FALSE);
  if (expected_exists != actual_exists) {
    return FontTableComparison::unavailable;
  }
  if (expected_exists == FALSE) {
    return FontTableComparison::same_missing;
  }
  if (expected_data == nullptr || actual_data == nullptr) {
    return FontTableComparison::unavailable;
  }
  const auto* expected_bytes = static_cast<const std::byte*>(expected_data);
  const auto* actual_bytes = static_cast<const std::byte*>(actual_data);
  return expected_size == actual_size &&
                 std::equal(expected_bytes, expected_bytes + expected_size,
                            actual_bytes)
             ? FontTableComparison::same_present
             : FontTableComparison::different_present;
}

bool FontTablesEqual(IDWriteFontFace* expected, IDWriteFontFace* actual,
                     const UINT32 table_tag) {
  return CompareFontTables(expected, actual, table_tag) ==
         FontTableComparison::same_present;
}

bool FontMetricsEqual(const DWRITE_FONT_METRICS& expected,
                      const DWRITE_FONT_METRICS& actual) noexcept {
  return expected.designUnitsPerEm == actual.designUnitsPerEm &&
         expected.ascent == actual.ascent &&
         expected.descent == actual.descent &&
         expected.lineGap == actual.lineGap &&
         expected.capHeight == actual.capHeight &&
         expected.xHeight == actual.xHeight &&
         expected.underlinePosition == actual.underlinePosition &&
         expected.underlineThickness == actual.underlineThickness &&
         expected.strikethroughPosition == actual.strikethroughPosition &&
         expected.strikethroughThickness == actual.strikethroughThickness;
}

bool GlyphMetricsEqual(const DWRITE_GLYPH_METRICS& expected,
                       const DWRITE_GLYPH_METRICS& actual) noexcept {
  return expected.leftSideBearing == actual.leftSideBearing &&
         expected.advanceWidth == actual.advanceWidth &&
         expected.rightSideBearing == actual.rightSideBearing &&
         expected.topSideBearing == actual.topSideBearing &&
         expected.advanceHeight == actual.advanceHeight &&
         expected.bottomSideBearing == actual.bottomSideBearing &&
         expected.verticalOriginY == actual.verticalOriginY;
}

bool FontGeometryEqual(IDWriteFontFace* expected, IDWriteFontFace* actual) {
  if (expected == nullptr || actual == nullptr) {
    return false;
  }

  DWRITE_FONT_METRICS expected_metrics{};
  DWRITE_FONT_METRICS actual_metrics{};
  expected->GetMetrics(&expected_metrics);
  actual->GetMetrics(&actual_metrics);
  if (!FontMetricsEqual(expected_metrics, actual_metrics)) {
    return false;
  }

  constexpr std::array<UINT32, 12> code_points = {
      L'A', L'M', L'W', L'a', L'g', L'm', L'w', L'0', L'8', L'?',
      0x00e9, 0x03a9};
  std::array<UINT16, code_points.size()> expected_glyphs{};
  std::array<UINT16, code_points.size()> actual_glyphs{};
  if (FAILED(expected->GetGlyphIndices(
          code_points.data(), static_cast<UINT32>(code_points.size()),
          expected_glyphs.data())) ||
      FAILED(actual->GetGlyphIndices(
          code_points.data(), static_cast<UINT32>(code_points.size()),
          actual_glyphs.data())) ||
      expected_glyphs != actual_glyphs) {
    return false;
  }

  std::array<DWRITE_GLYPH_METRICS, code_points.size()>
      expected_glyph_metrics{};
  std::array<DWRITE_GLYPH_METRICS, code_points.size()> actual_glyph_metrics{};
  if (FAILED(expected->GetDesignGlyphMetrics(
          expected_glyphs.data(), static_cast<UINT32>(expected_glyphs.size()),
          expected_glyph_metrics.data(), FALSE)) ||
      FAILED(actual->GetDesignGlyphMetrics(
          actual_glyphs.data(), static_cast<UINT32>(actual_glyphs.size()),
          actual_glyph_metrics.data(), FALSE))) {
    return false;
  }
  for (std::size_t index = 0; index < expected_glyph_metrics.size(); ++index) {
    if (!GlyphMetricsEqual(expected_glyph_metrics[index],
                           actual_glyph_metrics[index])) {
      return false;
    }
  }

  constexpr std::array<UINT32, 4> required_tables = {
      DWRITE_MAKE_OPENTYPE_TAG('c', 'm', 'a', 'p'),
      DWRITE_MAKE_OPENTYPE_TAG('m', 'a', 'x', 'p'),
      DWRITE_MAKE_OPENTYPE_TAG('h', 'h', 'e', 'a'),
      DWRITE_MAKE_OPENTYPE_TAG('h', 'm', 't', 'x')};
  for (const UINT32 table : required_tables) {
    if (!FontTablesEqual(expected, actual, table)) {
      return false;
    }
  }

  const FontTableComparison glyf = CompareFontTables(
      expected, actual, DWRITE_MAKE_OPENTYPE_TAG('g', 'l', 'y', 'f'));
  if (glyf == FontTableComparison::unavailable ||
      glyf == FontTableComparison::different_present) {
    return false;
  }
  if (glyf == FontTableComparison::same_present &&
      !FontTablesEqual(expected, actual,
                       DWRITE_MAKE_OPENTYPE_TAG('l', 'o', 'c', 'a'))) {
    return false;
  }
  const FontTableComparison cff = CompareFontTables(
      expected, actual, DWRITE_MAKE_OPENTYPE_TAG('C', 'F', 'F', ' '));
  const FontTableComparison cff2 = CompareFontTables(
      expected, actual, DWRITE_MAKE_OPENTYPE_TAG('C', 'F', 'F', '2'));
  if (cff == FontTableComparison::unavailable ||
      cff == FontTableComparison::different_present ||
      cff2 == FontTableComparison::unavailable ||
      cff2 == FontTableComparison::different_present) {
    return false;
  }
  return glyf == FontTableComparison::same_present ||
         cff == FontTableComparison::same_present ||
         cff2 == FontTableComparison::same_present;
}

class FontSubstitutionSwitch final {
 public:
  FontSubstitutionSwitch() {
    SetLastError(ERROR_SUCCESS);
    const DWORD required =
        GetEnvironmentVariableW(name_, nullptr, 0);
    existed_ = required != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    if (required != 0) {
      previous_.resize(required);
      GetEnvironmentVariableW(name_, previous_.data(), required);
    }
    SetEnvironmentVariableW(name_, L"1");
  }

  ~FontSubstitutionSwitch() {
    SetEnvironmentVariableW(name_,
                            existed_ ? previous_.c_str() : nullptr);
  }

  FontSubstitutionSwitch(const FontSubstitutionSwitch&) = delete;
  FontSubstitutionSwitch& operator=(const FontSubstitutionSwitch&) = delete;

 private:
  static constexpr wchar_t name_[] = L"MACTYPE_FONTSUBSTITUTES_ENV";
  bool existed_ = false;
  std::wstring previous_;
};

bool HashSha256(const std::byte* bytes, const std::size_t size,
                std::string& result) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                  0) < 0) {
    return false;
  }
  DWORD object_size = 0;
  DWORD returned = 0;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&object_size),
                        sizeof(object_size), &returned, 0) < 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return false;
  }
  std::vector<UCHAR> object(object_size);
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0,
                       0) < 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return false;
  }
  const NTSTATUS update = BCryptHashData(
      hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes)),
      static_cast<ULONG>(size), 0);
  std::array<UCHAR, 32> digest{};
  const NTSTATUS finish = update < 0
                              ? update
                              : BCryptFinishHash(
                                    hash, digest.data(),
                                    static_cast<ULONG>(digest.size()), 0);
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (finish < 0) {
    return false;
  }
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (const UCHAR value : digest) {
    output << std::setw(2) << static_cast<unsigned int>(value);
  }
  result = output.str();
  return true;
}

enum class SystemCollectionMode {
  legacy,
  factory3,
  factory6_weight_stretch_style,
  factory6_typographic,
  factory6_typographic_local,
  factory7_weight_stretch_style,
  factory7_typographic,
  factory7_typographic_local,
  factory6_font_set,
  factory7_font_set,
};

bool SupportsSystemCollectionMode(IDWriteFactory* factory,
                                  const SystemCollectionMode mode) {
  if (factory == nullptr) {
    return false;
  }
  if (mode == SystemCollectionMode::legacy) {
    return true;
  }
  if (mode == SystemCollectionMode::factory3) {
    IDWriteFactory3* raw_factory3 = nullptr;
    const HRESULT result = factory->QueryInterface(&raw_factory3);
    UniqueCom<IDWriteFactory3> factory3(raw_factory3);
    return SUCCEEDED(result) && factory3 != nullptr;
  }
  if (mode == SystemCollectionMode::factory6_weight_stretch_style ||
      mode == SystemCollectionMode::factory6_typographic ||
      mode == SystemCollectionMode::factory6_typographic_local ||
      mode == SystemCollectionMode::factory6_font_set) {
    IDWriteFactory6* raw_factory6 = nullptr;
    const HRESULT result = factory->QueryInterface(&raw_factory6);
    UniqueCom<IDWriteFactory6> factory6(raw_factory6);
    return SUCCEEDED(result) && factory6 != nullptr;
  }
  IDWriteFactory7* raw_factory7 = nullptr;
  const HRESULT result = factory->QueryInterface(&raw_factory7);
  UniqueCom<IDWriteFactory7> factory7(raw_factory7);
  return SUCCEEDED(result) && factory7 != nullptr;
}

bool AcquireSystemFontCollection(
    IDWriteFactory* factory, const SystemCollectionMode mode,
    UniqueCom<IDWriteFontCollection>& collection, std::wstring& error) {
  collection.reset();
  if (mode == SystemCollectionMode::legacy) {
    IDWriteFontCollection* raw_collection = nullptr;
    const HRESULT result =
        factory->GetSystemFontCollection(&raw_collection);
    collection.reset(raw_collection);
    if (FAILED(result) || collection == nullptr) {
      error = L"IDWriteFactory::GetSystemFontCollection failed";
      return false;
    }
    return true;
  }
  if (mode == SystemCollectionMode::factory3) {
    IDWriteFactory3* raw_factory3 = nullptr;
    const HRESULT factory_result = factory->QueryInterface(&raw_factory3);
    UniqueCom<IDWriteFactory3> factory3(raw_factory3);
    IDWriteFontCollection1* raw_collection = nullptr;
    const HRESULT result =
        FAILED(factory_result) || factory3 == nullptr
            ? E_NOINTERFACE
            : factory3->GetSystemFontCollection(
                  TRUE, &raw_collection, FALSE);
    collection.reset(raw_collection);
    if (FAILED(result) || collection == nullptr) {
      error = L"IDWriteFactory3::GetSystemFontCollection failed";
      return false;
    }
    return true;
  }
  if (mode == SystemCollectionMode::factory6_weight_stretch_style ||
      mode == SystemCollectionMode::factory6_typographic ||
      mode == SystemCollectionMode::factory6_typographic_local ||
      mode == SystemCollectionMode::factory6_font_set) {
    IDWriteFactory6* raw_factory6 = nullptr;
    const HRESULT factory_result = factory->QueryInterface(&raw_factory6);
    UniqueCom<IDWriteFactory6> factory6(raw_factory6);
    if (FAILED(factory_result) || factory6 == nullptr) {
      error = L"IDWriteFactory6 is unavailable";
      return false;
    }
    if (mode == SystemCollectionMode::factory6_font_set) {
      IDWriteFontSet1* raw_set = nullptr;
      const HRESULT set_result = factory6->GetSystemFontSet(TRUE, &raw_set);
      UniqueCom<IDWriteFontSet1> set(raw_set);
      IDWriteFontCollection2* raw_collection = nullptr;
      const HRESULT collection_result =
          FAILED(set_result) || set == nullptr
              ? E_UNEXPECTED
              : factory6->CreateFontCollectionFromFontSet(
                    set.get(), DWRITE_FONT_FAMILY_MODEL_WEIGHT_STRETCH_STYLE,
                    &raw_collection);
      collection.reset(raw_collection);
      if (FAILED(collection_result) || collection == nullptr) {
        error = L"IDWriteFactory6 font-set collection failed";
        return false;
      }
      return true;
    }
    const BOOL include_downloadable_fonts =
        mode == SystemCollectionMode::factory6_typographic_local ? FALSE : TRUE;
    const DWRITE_FONT_FAMILY_MODEL family_model =
        mode == SystemCollectionMode::factory6_typographic ||
                mode == SystemCollectionMode::factory6_typographic_local
            ? DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC
            : DWRITE_FONT_FAMILY_MODEL_WEIGHT_STRETCH_STYLE;
    IDWriteFontCollection2* raw_collection = nullptr;
    const HRESULT collection_result = factory6->GetSystemFontCollection(
        include_downloadable_fonts, family_model, &raw_collection);
    UniqueCom<IDWriteFontCollection2> first(raw_collection);
    IDWriteFontCollection2* raw_repeated_collection = nullptr;
    const HRESULT repeated_result = factory6->GetSystemFontCollection(
        include_downloadable_fonts, family_model, &raw_repeated_collection);
    UniqueCom<IDWriteFontCollection2> repeated(raw_repeated_collection);
    if (FAILED(collection_result) || first == nullptr ||
        FAILED(repeated_result) || repeated == nullptr) {
      error = L"IDWriteFactory6::GetSystemFontCollection failed";
      return false;
    }
    if (first.get() != repeated.get()) {
      error = L"IDWriteFactory6 system collection pointer changed";
      return false;
    }
    collection.reset(first.release());
    return true;
  }

  IDWriteFactory7* raw_factory7 = nullptr;
  const HRESULT factory_result = factory->QueryInterface(&raw_factory7);
  UniqueCom<IDWriteFactory7> factory7(raw_factory7);
  if (FAILED(factory_result) || factory7 == nullptr) {
    error = L"IDWriteFactory7 is unavailable";
    return false;
  }
  if (mode == SystemCollectionMode::factory7_font_set) {
    IDWriteFontSet2* raw_set = nullptr;
    const HRESULT set_result = factory7->GetSystemFontSet(TRUE, &raw_set);
    UniqueCom<IDWriteFontSet2> set(raw_set);
    IDWriteFontCollection2* raw_collection = nullptr;
    const HRESULT collection_result =
        FAILED(set_result) || set == nullptr
            ? E_UNEXPECTED
            : factory7->CreateFontCollectionFromFontSet(
                  set.get(), DWRITE_FONT_FAMILY_MODEL_WEIGHT_STRETCH_STYLE,
                  &raw_collection);
    collection.reset(raw_collection);
    if (FAILED(collection_result) || collection == nullptr) {
      error = L"IDWriteFactory7 font-set collection failed";
      return false;
    }
    return true;
  }
  const BOOL include_downloadable_fonts =
      mode == SystemCollectionMode::factory7_typographic_local ? FALSE : TRUE;
  const DWRITE_FONT_FAMILY_MODEL family_model =
      mode == SystemCollectionMode::factory7_typographic ||
              mode == SystemCollectionMode::factory7_typographic_local
          ? DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC
          : DWRITE_FONT_FAMILY_MODEL_WEIGHT_STRETCH_STYLE;
  IDWriteFontCollection3* raw_collection = nullptr;
  const HRESULT collection_result = factory7->GetSystemFontCollection(
      include_downloadable_fonts, family_model, &raw_collection);
  collection.reset(raw_collection);
  if (FAILED(collection_result) || collection == nullptr) {
    error = L"IDWriteFactory7::GetSystemFontCollection failed";
    return false;
  }
  return true;
}

std::wstring ResolveDirectWriteFamily(
    const std::wstring_view family, void* directwrite_factory,
    const SystemCollectionMode collection_mode,
    UniqueCom<IDWriteFontFace>* retained_face, std::wstring& error) {
  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  IDWriteFactory* raw_factory = nullptr;
  UniqueCom<IDWriteFactory> owned_factory;
  if (factory == nullptr) {
    const HRESULT factory_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&raw_factory));
    owned_factory.reset(raw_factory);
    if (FAILED(factory_result) || owned_factory == nullptr) {
      error = L"DWriteCreateFactory failed";
      return {};
    }
    factory = owned_factory.get();
  }
  const std::wstring family_name(family);
  UniqueCom<IDWriteFontCollection> collection;
  if (!AcquireSystemFontCollection(
          factory, collection_mode, collection, error)) {
    return {};
  }
  IDWriteFontCollection1* raw_collection1 = nullptr;
  collection->QueryInterface(&raw_collection1);
  UniqueCom<IDWriteFontCollection1> collection1(raw_collection1);
  UINT32 family_index = 0;
  BOOL family_exists = FALSE;
  if (FAILED(collection->FindFamilyName(family_name.c_str(), &family_index,
                                        &family_exists)) ||
      family_exists == FALSE) {
    error = L"IDWriteFontCollection::FindFamilyName failed";
    return {};
  }
  IDWriteFontFamily* raw_family = nullptr;
  const HRESULT family_result =
      collection->GetFontFamily(family_index, &raw_family);
  UniqueCom<IDWriteFontFamily> font_family(raw_family);
  if (FAILED(family_result) || font_family == nullptr) {
    error = L"IDWriteFontCollection::GetFontFamily failed";
    return {};
  }
  IDWriteFont* raw_font = nullptr;
  const HRESULT font_result = font_family->GetFirstMatchingFont(
          DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, &raw_font);
  UniqueCom<IDWriteFont> font(raw_font);
  if (FAILED(font_result) || font == nullptr) {
    error = L"IDWriteFontFamily::GetFirstMatchingFont failed";
    return {};
  }
  IDWriteFontFace* raw_face = nullptr;
  const HRESULT face_result = font->CreateFontFace(&raw_face);
  UniqueCom<IDWriteFontFace> face(raw_face);
  if (FAILED(face_result) || face == nullptr) {
    error = L"IDWriteFont::CreateFontFace failed";
    return {};
  }
  IDWriteGdiInterop* raw_interop = nullptr;
  const HRESULT interop_result = factory->GetGdiInterop(&raw_interop);
  UniqueCom<IDWriteGdiInterop> interop(raw_interop);
  if (FAILED(interop_result) || interop == nullptr) {
    error = L"IDWriteFactory::GetGdiInterop failed";
    return {};
  }
  LOGFONTW resolved{};
  if (FAILED(interop->ConvertFontFaceToLOGFONT(face.get(), &resolved))) {
    error = L"IDWriteGdiInterop::ConvertFontFaceToLOGFONT failed";
    return {};
  }
  if (retained_face != nullptr) {
    retained_face->reset(face.release());
  }
  return resolved.lfFaceName;
}

struct GlyphRunAxisObservation final {
  std::wstring family;
  std::wstring face;
  FLOAT weight = 0.0F;
  bool has_weight = false;
  bool has_variations = false;
};

bool ReadLocalizedValue(IDWriteLocalizedStrings* values, const UINT32 index,
                        std::wstring& value) {
  if (values == nullptr || index >= values->GetCount()) {
    return false;
  }
  UINT32 length = 0;
  if (FAILED(values->GetStringLength(index, &length))) {
    return false;
  }
  value.assign(static_cast<std::size_t>(length) + 1U, L'\0');
  if (FAILED(values->GetString(index, value.data(), length + 1U))) {
    value.clear();
    return false;
  }
  value.resize(length);
  return true;
}

bool ReadPreferredLocalizedValue(IDWriteLocalizedStrings* values,
                                 std::wstring& value) {
  if (values == nullptr || values->GetCount() == 0) {
    return false;
  }
  UINT32 index = 0;
  BOOL locale_exists = FALSE;
  if (FAILED(values->FindLocaleName(L"en-us", &index, &locale_exists))) {
    return false;
  }
  return ReadLocalizedValue(values, locale_exists == FALSE ? 0 : index, value) &&
         !value.empty();
}

bool ReadFaceAxisObservation(IDWriteFontFace* face,
                             GlyphRunAxisObservation& observation) {
  IDWriteFontFace5* raw_face5 = nullptr;
  const HRESULT face_result =
      face == nullptr ? E_INVALIDARG : face->QueryInterface(&raw_face5);
  UniqueCom<IDWriteFontFace5> face5(raw_face5);
  if (FAILED(face_result) || face5 == nullptr) {
    return false;
  }

  IDWriteLocalizedStrings* raw_names = nullptr;
  const HRESULT names_result = face5->GetFamilyNames(&raw_names);
  UniqueCom<IDWriteLocalizedStrings> names(raw_names);
  if (FAILED(names_result) || names == nullptr || names->GetCount() == 0) {
    return false;
  }
  if (!ReadPreferredLocalizedValue(names.get(), observation.family)) {
    return false;
  }
  IDWriteLocalizedStrings* raw_face_names = nullptr;
  const HRESULT face_names_result = face5->GetFaceNames(&raw_face_names);
  UniqueCom<IDWriteLocalizedStrings> face_names(raw_face_names);
  if (FAILED(face_names_result) ||
      !ReadPreferredLocalizedValue(face_names.get(), observation.face)) {
    return false;
  }

  const UINT32 axis_count = face5->GetFontAxisValueCount();
  std::vector<DWRITE_FONT_AXIS_VALUE> axes(axis_count);
  if (axis_count != 0 &&
      FAILED(face5->GetFontAxisValues(axes.data(), axis_count))) {
    return false;
  }
  for (const DWRITE_FONT_AXIS_VALUE& axis : axes) {
    if (axis.axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT) {
      observation.weight = axis.value;
      observation.has_weight = true;
      break;
    }
  }
  observation.has_variations = face5->HasVariations() != FALSE;
  return observation.has_weight;
}

class GlyphRunAxisRecorder final : public IDWriteTextRenderer {
 public:
  IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (iid == __uuidof(IUnknown) ||
        iid == __uuidof(IDWritePixelSnapping) ||
        iid == __uuidof(IDWriteTextRenderer)) {
      *object = static_cast<IDWriteTextRenderer*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  IFACEMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }

  IFACEMETHODIMP_(ULONG) Release() override {
    const LONG remaining = InterlockedDecrement(&references_);
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* is_disabled) override {
    if (is_disabled == nullptr) {
      return E_POINTER;
    }
    *is_disabled = FALSE;
    return S_OK;
  }

  IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
    if (transform == nullptr) {
      return E_POINTER;
    }
    *transform = DWRITE_MATRIX{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
    return S_OK;
  }

  IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* pixels_per_dip) override {
    if (pixels_per_dip == nullptr) {
      return E_POINTER;
    }
    *pixels_per_dip = 1.0F;
    return S_OK;
  }

  IFACEMETHODIMP DrawGlyphRun(
      void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE,
      const DWRITE_GLYPH_RUN* glyph_run,
      const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
    GlyphRunAxisObservation observation;
    if (glyph_run == nullptr ||
        !ReadFaceAxisObservation(glyph_run->fontFace, observation)) {
      return E_FAIL;
    }
    observations_.push_back(std::move(observation));
    return S_OK;
  }

  IFACEMETHODIMP DrawUnderline(void*, FLOAT, FLOAT,
                               const DWRITE_UNDERLINE*, IUnknown*) override {
    return S_OK;
  }

  IFACEMETHODIMP DrawStrikethrough(
      void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
    return S_OK;
  }

  IFACEMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*,
                                  BOOL, BOOL, IUnknown*) override {
    return S_OK;
  }

  const std::vector<GlyphRunAxisObservation>& observations() const noexcept {
    return observations_;
  }

 private:
  ~GlyphRunAxisRecorder() = default;

  LONG references_ = 1;
  std::vector<GlyphRunAxisObservation> observations_;
};

bool ObserveLayoutAxis(IDWriteFactory* factory,
                       const SystemCollectionMode collection_mode,
                       const std::wstring_view source_family,
                       const FLOAT requested_weight,
                       GlyphRunAxisObservation& observation,
                       std::wstring& error) {
  UniqueCom<IDWriteFontCollection> collection;
  if (!AcquireSystemFontCollection(factory, collection_mode, collection,
                                   error)) {
    return false;
  }

  const std::wstring family_name(source_family);
  IDWriteTextFormat* raw_format = nullptr;
  const HRESULT format_result = factory->CreateTextFormat(
      family_name.c_str(), collection.get(), DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0F, L"en-us",
      &raw_format);
  UniqueCom<IDWriteTextFormat> format(raw_format);
  if (FAILED(format_result) || format == nullptr) {
    error = L"CreateTextFormat failed for the axis-pin probe";
    return false;
  }

  constexpr std::wstring_view text = L"A";
  IDWriteTextLayout* raw_layout = nullptr;
  const HRESULT layout_result = factory->CreateTextLayout(
      text.data(), static_cast<UINT32>(text.size()), format.get(), 128.0F,
      64.0F, &raw_layout);
  UniqueCom<IDWriteTextLayout> layout(raw_layout);
  if (FAILED(layout_result) || layout == nullptr) {
    error = L"CreateTextLayout failed for the axis-pin probe";
    return false;
  }

  IDWriteTextLayout4* raw_layout4 = nullptr;
  const HRESULT layout4_result = layout->QueryInterface(&raw_layout4);
  UniqueCom<IDWriteTextLayout4> layout4(raw_layout4);
  if (FAILED(layout4_result) || layout4 == nullptr) {
    error = L"IDWriteTextLayout4 is unavailable for the axis-pin probe";
    return false;
  }
  const DWRITE_FONT_AXIS_VALUE axis_value = {
      DWRITE_FONT_AXIS_TAG_WEIGHT, requested_weight};
  const DWRITE_TEXT_RANGE range = {0, static_cast<UINT32>(text.size())};
  if (FAILED(layout4->SetFontAxisValues(&axis_value, 1, range))) {
    error = L"IDWriteTextLayout4::SetFontAxisValues failed";
    return false;
  }

  UniqueCom<GlyphRunAxisRecorder> recorder(new GlyphRunAxisRecorder());
  const HRESULT draw_result = layout->Draw(nullptr, recorder.get(), 0.0F, 0.0F);
  if (FAILED(draw_result) || recorder->observations().size() != 1) {
    error = L"IDWriteTextLayout::Draw did not produce one axis observation";
    return false;
  }
  observation = recorder->observations().front();
  return true;
}

void PrintLayoutAxisObservation(const std::wstring_view model,
                                const FLOAT requested_weight,
                                const GlyphRunAxisObservation& observation) {
  std::wostringstream output;
  output << L"axis-pin model=" << model << L" requested-wght="
         << std::fixed << std::setprecision(1) << requested_weight
         << L" face-family=\"" << observation.family << L"\" face-name=\""
         << observation.face << L"\" face-wght=" << observation.weight << L'\n';
  std::wcout << output.str();
}

bool ResolveFontSetNamedInstance(IDWriteFactory* factory,
                                 const std::wstring_view family,
                                 GlyphRunAxisObservation& observation) {
  IDWriteFactory3* raw_factory3 = nullptr;
  const HRESULT factory_result = factory->QueryInterface(&raw_factory3);
  UniqueCom<IDWriteFactory3> factory3(raw_factory3);
  if (FAILED(factory_result) || factory3 == nullptr) {
    return false;
  }
  IDWriteFontSet* raw_system_set = nullptr;
  const HRESULT set_result = factory3->GetSystemFontSet(&raw_system_set);
  UniqueCom<IDWriteFontSet> system_set(raw_system_set);
  if (FAILED(set_result) || system_set == nullptr) {
    return false;
  }

  const std::wstring family_name(family);
  constexpr std::array<DWRITE_FONT_PROPERTY_ID, 3> family_properties = {
      DWRITE_FONT_PROPERTY_ID_WEIGHT_STRETCH_STYLE_FAMILY_NAME,
      DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME,
      DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME,
  };
  for (const DWRITE_FONT_PROPERTY_ID property_id : family_properties) {
    const DWRITE_FONT_PROPERTY property = {
        property_id, family_name.c_str(), nullptr};
    IDWriteFontSet* raw_matches = nullptr;
    const HRESULT match_result =
        system_set->GetMatchingFonts(&property, 1, &raw_matches);
    UniqueCom<IDWriteFontSet> matches(raw_matches);
    if (FAILED(match_result) || matches == nullptr ||
        matches->GetFontCount() == 0) {
      continue;
    }
    IDWriteFontFaceReference* raw_reference = nullptr;
    const HRESULT reference_result =
        matches->GetFontFaceReference(0, &raw_reference);
    UniqueCom<IDWriteFontFaceReference> reference(raw_reference);
    IDWriteFontFace3* raw_face = nullptr;
    const HRESULT face_result =
        FAILED(reference_result) || reference == nullptr
            ? E_UNEXPECTED
            : reference->CreateFontFace(&raw_face);
    UniqueCom<IDWriteFontFace3> face(raw_face);
    return SUCCEEDED(face_result) && face != nullptr &&
           ReadFaceAxisObservation(face.get(), observation);
  }
  return false;
}

bool ObservePinnedAxisLayouts(const std::wstring_view source_family,
                              const std::wstring_view replacement_family,
                              void* directwrite_factory,
                              std::wstring& error) {
  constexpr std::wstring_view fixture_source = L"Arial";
  constexpr std::wstring_view fixture_replacement = L"Bahnschrift SemiBold";
  if (_wcsicmp(std::wstring(source_family).c_str(), fixture_source.data()) != 0 ||
      _wcsicmp(std::wstring(replacement_family).c_str(),
               fixture_replacement.data()) != 0) {
    return true;
  }

  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  IDWriteFactory* raw_factory = nullptr;
  UniqueCom<IDWriteFactory> owned_factory;
  if (factory == nullptr) {
    const HRESULT factory_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&raw_factory));
    owned_factory.reset(raw_factory);
    if (FAILED(factory_result) || owned_factory == nullptr) {
      error = L"DWriteCreateFactory failed for the axis-pin probe";
      return false;
    }
    factory = owned_factory.get();
  }

  GlyphRunAxisObservation named_instance;
  {
    FontSubstitutionSwitch disabled;
    if (!ResolveFontSetNamedInstance(factory, replacement_family,
                                     named_instance) ||
        !named_instance.has_variations || named_instance.weight != 600.0F) {
      error = L"Bahnschrift SemiBold is not a variable wght=600 named instance";
      return false;
    }
  }

  struct CollectionProbe final {
    SystemCollectionMode mode;
    std::wstring_view name;
    bool must_pin;
  };
  constexpr std::array<CollectionProbe, 5> collection_probes = {{
      {SystemCollectionMode::legacy,
       L"weight/stretch/style with explicit axis values, not pinned (known "
       L"limitation)",
       false},
      {SystemCollectionMode::factory6_typographic_local,
       L"typographic-slot-51-local", true},
      {SystemCollectionMode::factory6_typographic,
       L"typographic-slot-51-downloadable", true},
      {SystemCollectionMode::factory7_typographic_local,
       L"typographic-slot-56-local", true},
      {SystemCollectionMode::factory7_typographic,
       L"typographic-slot-56-downloadable", true},
  }};
  constexpr std::array<FLOAT, 2> requested_weights = {400.0F, 700.0F};
  const std::wstring source_name(source_family);
  for (const CollectionProbe& collection_probe : collection_probes) {
    if ((collection_probe.mode == SystemCollectionMode::factory7_typographic ||
         collection_probe.mode ==
             SystemCollectionMode::factory7_typographic_local) &&
        !SupportsSystemCollectionMode(factory, collection_probe.mode)) {
      continue;
    }
    for (const FLOAT requested_weight : requested_weights) {
      GlyphRunAxisObservation observation;
      if (!ObserveLayoutAxis(factory, collection_probe.mode, source_family,
                             requested_weight, observation, error)) {
        return false;
      }
      PrintLayoutAxisObservation(collection_probe.name, requested_weight,
                                 observation);
      if (collection_probe.must_pin &&
          (_wcsicmp(observation.family.c_str(), source_name.c_str()) != 0 ||
           observation.weight != named_instance.weight)) {
        std::wostringstream message;
        message << L"Axis pin changed for " << collection_probe.name
                << L" at requested wght " << requested_weight;
        error = message.str();
        return false;
      }
    }
  }
  return true;
}

class AnalysisSource final : public IDWriteTextAnalysisSource {
 public:
  explicit AnalysisSource(std::wstring text) : text_(std::move(text)) {}

  IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    if (iid == __uuidof(IUnknown) ||
        iid == __uuidof(IDWriteTextAnalysisSource)) {
      *object = static_cast<IDWriteTextAnalysisSource*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  IFACEMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&references_));
  }

  IFACEMETHODIMP_(ULONG) Release() override {
    const LONG remaining = InterlockedDecrement(&references_);
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  IFACEMETHODIMP GetTextAtPosition(UINT32 position, const wchar_t** text,
                                   UINT32* length) override {
    if (text == nullptr || length == nullptr) {
      return E_POINTER;
    }
    if (position >= text_.size()) {
      *text = nullptr;
      *length = 0;
      return S_OK;
    }
    *text = text_.c_str() + position;
    *length = static_cast<UINT32>(text_.size() - position);
    return S_OK;
  }

  IFACEMETHODIMP GetTextBeforePosition(UINT32 position, const wchar_t** text,
                                       UINT32* length) override {
    if (text == nullptr || length == nullptr) {
      return E_POINTER;
    }
    const UINT32 available = static_cast<UINT32>(
        (std::min)(static_cast<std::size_t>(position), text_.size()));
    *text = available == 0 ? nullptr : text_.c_str();
    *length = available;
    return S_OK;
  }

  IFACEMETHODIMP_(DWRITE_READING_DIRECTION)
  GetParagraphReadingDirection() override {
    return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
  }

  IFACEMETHODIMP GetLocaleName(UINT32 position, UINT32* length,
                               const wchar_t** locale) override {
    if (length == nullptr || locale == nullptr) {
      return E_POINTER;
    }
    *length = position >= text_.size()
                  ? 0
                  : static_cast<UINT32>(text_.size() - position);
    *locale = L"en-us";
    return S_OK;
  }

  IFACEMETHODIMP GetNumberSubstitution(
      UINT32 position, UINT32* length,
      IDWriteNumberSubstitution** substitution) override {
    if (length == nullptr || substitution == nullptr) {
      return E_POINTER;
    }
    *length = position >= text_.size()
                  ? 0
                  : static_cast<UINT32>(text_.size() - position);
    *substitution = nullptr;
    return S_OK;
  }

 private:
  ~AnalysisSource() = default;

  LONG references_ = 1;
  std::wstring text_;
};

std::wstring ResolveTypographicFallbackFamily(
    const std::wstring_view family, void* directwrite_factory,
    UniqueCom<IDWriteFontFace>* retained_face, std::wstring& error) {
  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  if (factory == nullptr) {
    error = L"DirectWrite factory is unavailable";
    return {};
  }
  UniqueCom<IDWriteFontCollection> collection;
  if (!AcquireSystemFontCollection(
          factory, SystemCollectionMode::factory6_typographic,
          collection, error)) {
    return {};
  }
  IDWriteFactory2* raw_factory2 = nullptr;
  const HRESULT factory_result = factory->QueryInterface(&raw_factory2);
  UniqueCom<IDWriteFactory2> factory2(raw_factory2);
  IDWriteFontFallback* raw_fallback = nullptr;
  const HRESULT fallback_result =
      FAILED(factory_result) || factory2 == nullptr
          ? E_NOINTERFACE
          : factory2->GetSystemFontFallback(&raw_fallback);
  UniqueCom<IDWriteFontFallback> fallback(raw_fallback);
  if (FAILED(fallback_result) || fallback == nullptr) {
    error = L"IDWriteFactory2::GetSystemFontFallback failed";
    return {};
  }

  const std::wstring text = L"A";
  UniqueCom<AnalysisSource> source(new AnalysisSource(text));
  IDWriteFont* raw_font = nullptr;
  UINT32 mapped_length = 0;
  FLOAT scale = 0.0F;
  const std::wstring family_name(family);
  const HRESULT map_result = fallback->MapCharacters(
      source.get(), 0, static_cast<UINT32>(text.size()), collection.get(),
      family_name.c_str(), DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
      &mapped_length, &raw_font, &scale);
  UniqueCom<IDWriteFont> font(raw_font);
  if (FAILED(map_result) || font == nullptr || mapped_length != text.size()) {
    error = L"IDWriteFontFallback::MapCharacters failed";
    return {};
  }
  IDWriteFontFace* raw_face = nullptr;
  const HRESULT face_result = font->CreateFontFace(&raw_face);
  UniqueCom<IDWriteFontFace> face(raw_face);
  if (FAILED(face_result) || face == nullptr) {
    error = L"IDWriteFont::CreateFontFace failed";
    return {};
  }
  IDWriteGdiInterop* raw_interop = nullptr;
  const HRESULT interop_result = factory->GetGdiInterop(&raw_interop);
  UniqueCom<IDWriteGdiInterop> interop(raw_interop);
  LOGFONTW resolved{};
  if (FAILED(interop_result) || interop == nullptr ||
      FAILED(interop->ConvertFontFaceToLOGFONT(face.get(), &resolved))) {
    error = L"IDWriteGdiInterop::ConvertFontFaceToLOGFONT failed";
    return {};
  }
  if (retained_face != nullptr) {
    retained_face->reset(face.release());
  }
  return resolved.lfFaceName;
}

std::wstring ResolveDirectWriteFontObject(
    IDWriteFont* font, void* directwrite_factory, std::wstring& error) {
  if (font == nullptr) {
    error = L"DirectWrite font object is unavailable";
    return {};
  }
  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  IDWriteFactory* raw_factory = nullptr;
  UniqueCom<IDWriteFactory> owned_factory;
  if (factory == nullptr) {
    const HRESULT factory_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&raw_factory));
    owned_factory.reset(raw_factory);
    if (FAILED(factory_result) || owned_factory == nullptr) {
      error = L"DWriteCreateFactory failed for indexed collection";
      return {};
    }
    factory = owned_factory.get();
  }

  IDWriteGdiInterop* raw_interop = nullptr;
  const HRESULT interop_result = factory->GetGdiInterop(&raw_interop);
  UniqueCom<IDWriteGdiInterop> interop(raw_interop);
  LOGFONTW resolved{};
  BOOL is_system_font = FALSE;
  if (FAILED(interop_result) || interop == nullptr ||
      FAILED(interop->ConvertFontToLOGFONT(font, &resolved,
                                          &is_system_font)) ||
      is_system_font == FALSE) {
    error = L"ConvertFontToLOGFONT failed for indexed collection";
    return {};
  }
  return resolved.lfFaceName;
}

std::wstring ResolveDirectWriteFontDescriptor(
    IDWriteFontFace* face, void* directwrite_factory, std::wstring& error) {
  if (face == nullptr) {
    error = L"DirectWrite font face is unavailable";
    return {};
  }
  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  IDWriteFactory* raw_factory = nullptr;
  UniqueCom<IDWriteFactory> owned_factory;
  if (factory == nullptr) {
    const HRESULT factory_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&raw_factory));
    owned_factory.reset(raw_factory);
    if (FAILED(factory_result) || owned_factory == nullptr) {
      error = L"DWriteCreateFactory failed for retained face descriptor";
      return {};
    }
    factory = owned_factory.get();
  }

  UINT32 file_count = 0;
  if (FAILED(face->GetFiles(&file_count, nullptr)) || file_count == 0) {
    error = L"GetFiles count failed for retained face descriptor";
    return {};
  }
  std::vector<IDWriteFontFile*> files(file_count, nullptr);
  if (FAILED(face->GetFiles(&file_count, files.data()))) {
    for (IDWriteFontFile* file : files) {
      if (file != nullptr) {
        file->Release();
      }
    }
    error = L"GetFiles failed for retained face descriptor";
    return {};
  }
  std::vector<UniqueCom<IDWriteFontFile>> owned_files;
  owned_files.reserve(files.size());
  for (IDWriteFontFile* file : files) {
    owned_files.emplace_back(file);
  }

  if (file_count != 1) {
    error = L"Retained face descriptor must contain exactly one font file";
    return {};
  }
  BOOL is_supported = FALSE;
  DWRITE_FONT_FILE_TYPE file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
  DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
  UINT32 face_count = 0;
  if (FAILED(files.front()->Analyze(&is_supported, &file_type, &face_type,
                                    &face_count)) ||
      is_supported == FALSE || file_type == DWRITE_FONT_FILE_TYPE_UNKNOWN ||
      face_count == 0) {
    error = L"Analyze failed for retained face descriptor";
    return {};
  }
  const UINT32 face_index = face->GetIndex();
  const DWRITE_FONT_SIMULATIONS simulations = face->GetSimulations();
  IDWriteFontFace* raw_descriptor_face = nullptr;
  const HRESULT face_result = factory->CreateFontFace(
      face_type, file_count, files.data(), face_index, simulations,
      &raw_descriptor_face);
  UniqueCom<IDWriteFontFace> descriptor_face(raw_descriptor_face);
  if (FAILED(face_result) || descriptor_face == nullptr) {
    std::wostringstream message;
    message << L"CreateFontFace failed for retained face descriptor: HRESULT 0x"
            << std::hex << static_cast<unsigned long>(face_result)
            << L", type " << static_cast<unsigned int>(face_type)
            << L", files " << std::dec << file_count << L", index "
            << face_index << L", simulations "
            << static_cast<unsigned int>(simulations);
    error = message.str();
    return {};
  }

  IDWriteGdiInterop* raw_interop = nullptr;
  const HRESULT interop_result = factory->GetGdiInterop(&raw_interop);
  UniqueCom<IDWriteGdiInterop> interop(raw_interop);
  LOGFONTW resolved{};
  if (FAILED(interop_result) || interop == nullptr ||
      FAILED(interop->ConvertFontFaceToLOGFONT(descriptor_face.get(),
                                              &resolved))) {
    error = L"ConvertFontFaceToLOGFONT failed for retained face descriptor";
    return {};
  }
  return resolved.lfFaceName;
}

std::wstring ResolveAdvancedDirectWriteFontDescriptor(
    IDWriteFontFace* face, void* directwrite_factory,
    bool& name_table_stable, bool& geometry_stable, std::wstring& error) {
  name_table_stable = false;
  geometry_stable = false;
  if (face == nullptr) {
    error = L"DirectWrite font face is unavailable for advanced descriptor";
    return {};
  }

  IDWriteFontFace5* raw_face5 = nullptr;
  const HRESULT face5_result = face->QueryInterface(&raw_face5);
  UniqueCom<IDWriteFontFace5> face5(raw_face5);
  if (FAILED(face5_result) || face5 == nullptr) {
    error = L"IDWriteFontFace5 is unavailable for advanced descriptor";
    return {};
  }

  IDWriteFontResource* raw_resource = nullptr;
  const HRESULT resource_result = face5->GetFontResource(&raw_resource);
  UniqueCom<IDWriteFontResource> resource(raw_resource);
  if (FAILED(resource_result) || resource == nullptr) {
    error = L"IDWriteFontFace5::GetFontResource failed";
    return {};
  }

  const UINT32 axis_count = face5->GetFontAxisValueCount();
  std::vector<DWRITE_FONT_AXIS_VALUE> axis_values(axis_count);
  if (axis_count != 0 &&
      FAILED(face5->GetFontAxisValues(axis_values.data(), axis_count))) {
    error = L"IDWriteFontFace5::GetFontAxisValues failed";
    return {};
  }

  IDWriteFontFace5* raw_recreated_face = nullptr;
  const HRESULT recreate_result = resource->CreateFontFace(
      face5->GetSimulations(),
      axis_count == 0 ? nullptr : axis_values.data(), axis_count,
      &raw_recreated_face);
  UniqueCom<IDWriteFontFace5> recreated_face(raw_recreated_face);
  if (FAILED(recreate_result) || recreated_face == nullptr) {
    error = L"IDWriteFontResource::CreateFontFace failed";
    return {};
  }

  name_table_stable = FontTablesEqual(
      face, recreated_face.get(),
      DWRITE_MAKE_OPENTYPE_TAG('n', 'a', 'm', 'e'));
  geometry_stable = FontGeometryEqual(face, recreated_face.get());

  return ResolveDirectWriteFontDescriptor(
      recreated_face.get(), directwrite_factory, error);
}

std::wstring ResolveFontSetDirectWriteFamily(
    const std::wstring_view family, void* directwrite_factory,
    UniqueCom<IDWriteFontFace>* retained_face,
    std::wstring& error) {
  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  IDWriteFactory* raw_factory = nullptr;
  UniqueCom<IDWriteFactory> owned_factory;
  if (factory == nullptr) {
    const HRESULT factory_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&raw_factory));
    owned_factory.reset(raw_factory);
    if (FAILED(factory_result) || owned_factory == nullptr) {
      error = L"DWriteCreateFactory failed for font-set lookup";
      return {};
    }
    factory = owned_factory.get();
  }
  IDWriteFactory3* raw_factory3 = nullptr;
  const HRESULT factory3_result = factory->QueryInterface(&raw_factory3);
  UniqueCom<IDWriteFactory3> factory3(raw_factory3);
  if (FAILED(factory3_result) || factory3 == nullptr) {
    error = L"IDWriteFactory3 is unavailable for font-set lookup";
    return {};
  }
  IDWriteFontSet* raw_font_set = nullptr;
  const HRESULT font_set_result = factory3->GetSystemFontSet(&raw_font_set);
  UniqueCom<IDWriteFontSet> font_set(raw_font_set);
  if (FAILED(font_set_result) || font_set == nullptr) {
    error = L"IDWriteFactory3::GetSystemFontSet failed for font-set lookup";
    return {};
  }
  IDWriteFontCollection1* raw_font_set_collection = nullptr;
  const HRESULT create_result = factory3->CreateFontCollectionFromFontSet(
      font_set.get(), &raw_font_set_collection);
  UniqueCom<IDWriteFontCollection1> font_set_collection(
      raw_font_set_collection);
  if (FAILED(create_result) || font_set_collection == nullptr) {
    error = L"CreateFontCollectionFromFontSet failed";
    return {};
  }
  UINT32 family_index = 0;
  BOOL family_exists = FALSE;
  const std::wstring family_name(family);
  if (FAILED(font_set_collection->FindFamilyName(
          family_name.c_str(), &family_index, &family_exists)) ||
      family_exists == FALSE) {
    error = L"FindFamilyName failed for font-set collection";
    return {};
  }
  IDWriteFontFamily* raw_font_family = nullptr;
  const HRESULT family_result = font_set_collection->GetFontFamily(
      family_index, &raw_font_family);
  UniqueCom<IDWriteFontFamily> font_family(raw_font_family);
  if (FAILED(family_result) || font_family == nullptr) {
    error = L"GetFontFamily failed for font-set collection";
    return {};
  }
  IDWriteFont* raw_font = nullptr;
  const HRESULT font_result = font_family->GetFont(0, &raw_font);
  UniqueCom<IDWriteFont> font(raw_font);
  if (FAILED(font_result) || font == nullptr) {
    error = L"GetFont failed for font-set collection";
    return {};
  }
  std::wstring const resolved = ResolveDirectWriteFontObject(
      font.get(), factory, error);
  if (!resolved.empty() && retained_face != nullptr) {
    IDWriteFontFace* raw_face = nullptr;
    const HRESULT face_result = font->CreateFontFace(&raw_face);
    retained_face->reset(raw_face);
    if (FAILED(face_result) || *retained_face == nullptr) {
      error = L"CreateFontFace failed for font-set collection";
      return {};
    }
  }
  return resolved;
}

std::wstring ResolveIndexedDirectWriteFamily(
    const std::wstring_view family, void* directwrite_factory,
    std::wstring& postscript_name, UniqueCom<IDWriteFont>* retained_font,
    UniqueCom<IDWriteFontFace>* retained_face, std::wstring& error) {
  IDWriteFactory* factory = static_cast<IDWriteFactory*>(directwrite_factory);
  IDWriteFactory* raw_factory = nullptr;
  UniqueCom<IDWriteFactory> owned_factory;
  if (factory == nullptr) {
    const HRESULT factory_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&raw_factory));
    owned_factory.reset(raw_factory);
    if (FAILED(factory_result) || owned_factory == nullptr) {
      error = L"DWriteCreateFactory failed for indexed collection";
      return {};
    }
    factory = owned_factory.get();
  }

  IDWriteFontCollection* raw_collection = nullptr;
  const HRESULT collection_result =
      factory->GetSystemFontCollection(&raw_collection);
  UniqueCom<IDWriteFontCollection> collection(raw_collection);
  if (FAILED(collection_result) || collection == nullptr) {
    error = L"GetSystemFontCollection failed for indexed collection";
    return {};
  }
  // Family indexes belong to one immutable collection generation. Resolve the
  // index again after the substitution switch publishes another generation.
  UINT32 family_index = 0;
  BOOL exists = FALSE;
  const std::wstring family_name(family);
  if (FAILED(collection->FindFamilyName(family_name.c_str(), &family_index,
                                        &exists)) ||
      exists == FALSE) {
    error = L"FindFamilyName failed for indexed collection";
    return {};
  }

  IDWriteFontFamily* raw_family = nullptr;
  const HRESULT family_result =
      collection->GetFontFamily(family_index, &raw_family);
  UniqueCom<IDWriteFontFamily> font_family(raw_family);
  if (FAILED(family_result) || font_family == nullptr) {
    error = L"GetFontFamily failed for indexed collection";
    return {};
  }
  IDWriteFont* raw_font = nullptr;
  const HRESULT font_result = font_family->GetFont(0, &raw_font);
  UniqueCom<IDWriteFont> font(raw_font);
  if (FAILED(font_result) || font == nullptr) {
    error = L"GetFont failed for indexed collection";
    return {};
  }

  auto read_postscript_name = [&](IDWriteFont* target,
                                  std::wstring& value) -> bool {
    BOOL exists = FALSE;
    IDWriteLocalizedStrings* raw_names = nullptr;
    const HRESULT names_result = target->GetInformationalStrings(
        DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &raw_names, &exists);
    UniqueCom<IDWriteLocalizedStrings> names(raw_names);
    if (FAILED(names_result) || exists == FALSE || names == nullptr ||
        names->GetCount() == 0) {
      error = L"GetInformationalStrings failed for indexed collection";
      return false;
    }
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(0, &length))) {
      error = L"GetStringLength failed for indexed collection";
      return false;
    }
    value.assign(static_cast<std::size_t>(length) + 1U, L'\0');
    if (FAILED(names->GetString(0, value.data(), length + 1U))) {
      error = L"GetString failed for indexed collection";
      return false;
    }
    value.resize(length);
    return true;
  };
  if (!read_postscript_name(font.get(), postscript_name)) {
    return {};
  }

  if (retained_face != nullptr) {
    IDWriteFontFace* raw_face = nullptr;
    const HRESULT face_result = font->CreateFontFace(&raw_face);
    retained_face->reset(raw_face);
    if (FAILED(face_result) || *retained_face == nullptr) {
      error = L"CreateFontFace failed for retained indexed font";
      return {};
    }
  }

  std::wstring resolved = ResolveDirectWriteFontObject(
      font.get(), factory, error);
  if (!resolved.empty() && retained_font != nullptr) {
    retained_font->reset(font.release());
  }
  return resolved;
}

}  // namespace

void* CreateDirectWriteFactory(const bool isolated, std::wstring& error) {
  IDWriteFactory* factory = nullptr;
  const HRESULT result = DWriteCreateFactory(
      isolated ? DWRITE_FACTORY_TYPE_ISOLATED : DWRITE_FACTORY_TYPE_SHARED,
      __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(&factory));
  if (FAILED(result) || factory == nullptr) {
    error = L"DWriteCreateFactory failed before MacType preload";
    return nullptr;
  }
  return factory;
}

void ReleaseDirectWriteFactory(void* factory) noexcept {
  if (factory != nullptr) {
    static_cast<IDWriteFactory*>(factory)->Release();
  }
}

std::string RenderFingerprint(const std::wstring_view family,
                              std::wstring& error) {
  constexpr int width = 320;
  constexpr int height = 96;
  constexpr std::wstring_view text = L"MacType probe 0123456789 Aa 中 あ";

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void* pixels = nullptr;
  UniqueGdiObject bitmap(
      CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0));
  UniqueDc dc(CreateCompatibleDC(nullptr));
  if (bitmap == nullptr || dc == nullptr || pixels == nullptr) {
    error = L"CreateDIBSection/CreateCompatibleDC failed: " +
            Win32ErrorMessage(GetLastError());
    return {};
  }
  const HGDIOBJ old_bitmap = SelectObject(dc.get(), bitmap.get());
  if (old_bitmap == nullptr || old_bitmap == HGDI_ERROR) {
    error =
        L"SelectObject(bitmap) failed: " + Win32ErrorMessage(GetLastError());
    return {};
  }
  RECT rectangle{0, 0, width, height};
  FillRect(dc.get(), &rectangle,
           static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  SetTextColor(dc.get(), RGB(16, 24, 32));
  SetBkColor(dc.get(), RGB(255, 255, 255));
  SetBkMode(dc.get(), OPAQUE);
  const std::wstring family_name(family);
  UniqueGdiObject font(CreateFontW(
      -24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, family_name.c_str()));
  if (font == nullptr) {
    error = L"CreateFontW failed: " + Win32ErrorMessage(GetLastError());
    SelectObject(dc.get(), old_bitmap);
    return {};
  }
  const HGDIOBJ old_font =
      font == nullptr ? nullptr : SelectObject(dc.get(), font.get());
  RECT text_rectangle{12, 12, width - 12, height - 12};
  DrawTextW(dc.get(), text.data(), static_cast<int>(text.size()),
            &text_rectangle, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
  GdiFlush();

  std::string digest;
  const auto pixel_size = static_cast<std::size_t>(width) *
                          static_cast<std::size_t>(height) * 4U;
  if (!HashSha256(static_cast<const std::byte*>(pixels), pixel_size, digest)) {
    error = L"BCrypt SHA-256 calculation failed";
  }
  if (old_font != nullptr && old_font != HGDI_ERROR) {
    SelectObject(dc.get(), old_font);
  }
  SelectObject(dc.get(), old_bitmap);
  return digest;
}

FontSubstitutionObservation ObserveFontSubstitution(
    const std::wstring_view source_family,
    const std::wstring_view replacement_family, void* directwrite_factory,
    std::wstring& error) {
  FontSubstitutionObservation result;
  std::string repeated_disabled_source;
  std::string repeated_disabled_replacement;
  {
    FontSubstitutionSwitch disabled;
    if (RenderFingerprint(source_family, error).empty()) {
      return result;
    }
    result.disabled_source_fingerprint =
        RenderFingerprint(source_family, error);
    if (result.disabled_source_fingerprint.empty()) {
      return result;
    }
    result.disabled_replacement_fingerprint =
        RenderFingerprint(replacement_family, error);
    if (result.disabled_replacement_fingerprint.empty()) {
      return result;
    }
    repeated_disabled_source = RenderFingerprint(source_family, error);
    repeated_disabled_replacement =
        RenderFingerprint(replacement_family, error);
  }
  if (RenderFingerprint(source_family, error).empty()) {
    return result;
  }
  result.active_source_fingerprint = RenderFingerprint(source_family, error);
  const std::string repeated_active_source =
      RenderFingerprint(source_family, error);
  result.controls_stable =
      result.disabled_source_fingerprint == repeated_disabled_source &&
      result.disabled_replacement_fingerprint ==
          repeated_disabled_replacement &&
      result.active_source_fingerprint == repeated_active_source;
  result.replacement_observed =
      result.controls_stable && result.disabled_source_fingerprint !=
          result.disabled_replacement_fingerprint &&
      result.active_source_fingerprint ==
          result.disabled_replacement_fingerprint;

  const auto observe_direct_write = [&](const SystemCollectionMode mode) {
    DirectWriteSubstitutionObservation observation;
    std::wstring repeated_disabled_source_family;
    std::wstring repeated_disabled_replacement_family;
    UniqueCom<IDWriteFontFace> disabled_replacement_face;
    {
      FontSubstitutionSwitch disabled;
      static_cast<void>(ResolveDirectWriteFamily(
          source_family, directwrite_factory, mode, nullptr,
          error));
      observation.disabled_source_family = ResolveDirectWriteFamily(
          source_family, directwrite_factory, mode, nullptr,
          error);
      observation.disabled_replacement_family = ResolveDirectWriteFamily(
          replacement_family, directwrite_factory, mode,
          &disabled_replacement_face, error);
      repeated_disabled_source_family = ResolveDirectWriteFamily(
          source_family, directwrite_factory, mode, nullptr,
          error);
      repeated_disabled_replacement_family = ResolveDirectWriteFamily(
          replacement_family, directwrite_factory, mode,
          nullptr, error);
    }
    static_cast<void>(ResolveDirectWriteFamily(
        source_family, directwrite_factory, mode, nullptr,
        error));
    UniqueCom<IDWriteFontFace> active_source_face;
    observation.active_source_family = ResolveDirectWriteFamily(
        source_family, directwrite_factory, mode,
        &active_source_face, error);
    const std::wstring repeated_active_source_family = ResolveDirectWriteFamily(
        source_family, directwrite_factory, mode, nullptr,
        error);
    if (observation.disabled_source_family.empty() ||
        observation.disabled_replacement_family.empty() ||
        observation.active_source_family.empty()) {
      return observation;
    }
    observation.controls_stable =
        _wcsicmp(observation.disabled_source_family.c_str(),
                 repeated_disabled_source_family.c_str()) == 0 &&
        _wcsicmp(observation.disabled_replacement_family.c_str(),
                 repeated_disabled_replacement_family.c_str()) == 0 &&
        _wcsicmp(observation.active_source_family.c_str(),
                 repeated_active_source_family.c_str()) == 0;
    observation.replacement_geometry_coherent = FontGeometryEqual(
        disabled_replacement_face.get(), active_source_face.get());
    observation.replacement_observed =
        observation.controls_stable && observation.replacement_geometry_coherent &&
        _wcsicmp(observation.active_source_family.c_str(),
                 observation.disabled_source_family.c_str()) == 0;
    return observation;
  };
  const auto observe_typographic_fallback = [&]() {
    DirectWriteSubstitutionObservation observation;
    std::wstring repeated_disabled_source_family;
    std::wstring repeated_disabled_replacement_family;
    UniqueCom<IDWriteFontFace> disabled_replacement_face;
    {
      FontSubstitutionSwitch disabled;
      observation.disabled_source_family = ResolveTypographicFallbackFamily(
          source_family, directwrite_factory, nullptr, error);
      observation.disabled_replacement_family =
          ResolveTypographicFallbackFamily(
              replacement_family, directwrite_factory,
              &disabled_replacement_face, error);
      repeated_disabled_source_family = ResolveTypographicFallbackFamily(
          source_family, directwrite_factory, nullptr, error);
      repeated_disabled_replacement_family =
          ResolveTypographicFallbackFamily(
              replacement_family, directwrite_factory, nullptr, error);
    }
    UniqueCom<IDWriteFontFace> active_source_face;
    observation.active_source_family = ResolveTypographicFallbackFamily(
        source_family, directwrite_factory, &active_source_face, error);
    const std::wstring repeated_active_source_family =
        ResolveTypographicFallbackFamily(
            source_family, directwrite_factory, nullptr, error);
    if (observation.disabled_source_family.empty() ||
        observation.disabled_replacement_family.empty() ||
        observation.active_source_family.empty()) {
      return observation;
    }
    observation.controls_stable =
        _wcsicmp(observation.disabled_source_family.c_str(),
                 repeated_disabled_source_family.c_str()) == 0 &&
        _wcsicmp(observation.disabled_replacement_family.c_str(),
                 repeated_disabled_replacement_family.c_str()) == 0 &&
        _wcsicmp(observation.active_source_family.c_str(),
                 repeated_active_source_family.c_str()) == 0;
    observation.replacement_geometry_coherent = FontGeometryEqual(
        disabled_replacement_face.get(), active_source_face.get());
    observation.replacement_observed =
        observation.controls_stable &&
        observation.replacement_geometry_coherent &&
        _wcsicmp(observation.active_source_family.c_str(),
                 observation.disabled_source_family.c_str()) == 0;
    return observation;
  };
  DirectWriteSubstitutionObservation& indexed =
      result.direct_write_indexed_collection;
  std::wstring repeated_disabled_indexed_source;
  std::wstring repeated_disabled_indexed_replacement;
  std::wstring ignored_repeated_postscript_name;
  UniqueCom<IDWriteFont> pinned_disabled_source;
  UniqueCom<IDWriteFontFace> pinned_disabled_source_face;
  UniqueCom<IDWriteFontFace> pinned_disabled_replacement_face;
  {
    FontSubstitutionSwitch disabled;
    indexed.disabled_source_family =
        ResolveIndexedDirectWriteFamily(source_family, directwrite_factory,
            indexed.disabled_source_postscript_name,
            &pinned_disabled_source, &pinned_disabled_source_face, error);
    indexed.disabled_replacement_family =
        ResolveIndexedDirectWriteFamily(replacement_family, directwrite_factory,
            indexed.disabled_replacement_postscript_name, nullptr,
            &pinned_disabled_replacement_face, error);
    repeated_disabled_indexed_source = ResolveIndexedDirectWriteFamily(
        source_family, directwrite_factory,
        ignored_repeated_postscript_name, nullptr, nullptr, error);
    repeated_disabled_indexed_replacement = ResolveIndexedDirectWriteFamily(
        replacement_family, directwrite_factory,
        ignored_repeated_postscript_name, nullptr, nullptr, error);
  }
  UniqueCom<IDWriteFontFace> active_alias_face;
  indexed.active_source_family =
      ResolveIndexedDirectWriteFamily(source_family, directwrite_factory,
          indexed.active_source_postscript_name,
          nullptr, &active_alias_face, error);
  const std::wstring repeated_active_indexed_source =
      ResolveIndexedDirectWriteFamily(source_family, directwrite_factory,
          ignored_repeated_postscript_name,
          nullptr, nullptr, error);
  indexed.retained_generation_family = ResolveDirectWriteFontObject(
      pinned_disabled_source.get(), directwrite_factory, error);
  indexed.retained_generation_descriptor_family =
      ResolveDirectWriteFontDescriptor(
          pinned_disabled_source_face.get(), directwrite_factory, error);
  bool resource_name_table_stable = false;
  bool resource_geometry_stable = false;
  indexed.active_alias_descriptor_family =
      ResolveAdvancedDirectWriteFontDescriptor(
          active_alias_face.get(), directwrite_factory,
          resource_name_table_stable, resource_geometry_stable, error);
  indexed.controls_stable =
      !indexed.disabled_source_family.empty() &&
      !indexed.disabled_replacement_family.empty() &&
      _wcsicmp(indexed.disabled_source_family.c_str(),
          repeated_disabled_indexed_source.c_str()) == 0 &&
      _wcsicmp(indexed.disabled_replacement_family.c_str(),
          repeated_disabled_indexed_replacement.c_str()) == 0 &&
      _wcsicmp(indexed.active_source_family.c_str(),
          repeated_active_indexed_source.c_str()) == 0;
  indexed.retained_generation_object_stable =
      _wcsicmp(indexed.retained_generation_family.c_str(),
               indexed.disabled_source_family.c_str()) == 0;
  indexed.retained_generation_descriptor_stable =
      _wcsicmp(indexed.retained_generation_descriptor_family.c_str(),
               indexed.disabled_source_family.c_str()) == 0;
  indexed.active_identity_coherent =
      !indexed.active_source_postscript_name.empty() &&
      _wcsicmp(indexed.active_source_family.c_str(),
               indexed.disabled_source_family.c_str()) == 0 &&
      _wcsicmp(indexed.active_alias_descriptor_family.c_str(),
               indexed.active_source_family.c_str()) == 0;
  indexed.virtual_name_table_coherent =
      resource_name_table_stable &&
      CompareFontTables(
          pinned_disabled_replacement_face.get(), active_alias_face.get(),
          DWRITE_MAKE_OPENTYPE_TAG('n', 'a', 'm', 'e')) ==
          FontTableComparison::different_present &&
      CompareFontTables(
          pinned_disabled_source_face.get(), active_alias_face.get(),
          DWRITE_MAKE_OPENTYPE_TAG('n', 'a', 'm', 'e')) ==
          FontTableComparison::different_present;
  indexed.replacement_geometry_coherent = FontGeometryEqual(
      pinned_disabled_replacement_face.get(), active_alias_face.get());
  indexed.resource_round_trip_coherent =
      resource_name_table_stable && resource_geometry_stable &&
      _wcsicmp(indexed.active_alias_descriptor_family.c_str(),
               indexed.active_source_family.c_str()) == 0;
  indexed.replacement_observed =
      indexed.controls_stable &&
      _wcsicmp(indexed.active_source_family.c_str(),
               indexed.disabled_source_family.c_str()) == 0 &&
      indexed.retained_generation_object_stable &&
      indexed.retained_generation_descriptor_stable &&
      indexed.active_identity_coherent &&
      indexed.virtual_name_table_coherent &&
      indexed.resource_round_trip_coherent &&
      indexed.replacement_geometry_coherent;
  result.direct_write =
      observe_direct_write(SystemCollectionMode::legacy);
  DirectWriteSubstitutionObservation& font_set =
      result.direct_write_font_set_collection;
  std::wstring repeated_disabled_font_set_source;
  std::wstring repeated_disabled_font_set_replacement;
  UniqueCom<IDWriteFontFace> disabled_font_set_replacement_face;
  {
    FontSubstitutionSwitch disabled;
    font_set.disabled_source_family = ResolveFontSetDirectWriteFamily(
        source_family, directwrite_factory, nullptr, error);
    font_set.disabled_replacement_family = ResolveFontSetDirectWriteFamily(
        replacement_family, directwrite_factory,
        &disabled_font_set_replacement_face, error);
    repeated_disabled_font_set_source = ResolveFontSetDirectWriteFamily(
        source_family, directwrite_factory, nullptr, error);
    repeated_disabled_font_set_replacement = ResolveFontSetDirectWriteFamily(
        replacement_family, directwrite_factory, nullptr, error);
  }
  UniqueCom<IDWriteFontFace> active_font_set_source_face;
  font_set.active_source_family = ResolveFontSetDirectWriteFamily(
      source_family, directwrite_factory, &active_font_set_source_face, error);
  const std::wstring repeated_active_font_set_source =
      ResolveFontSetDirectWriteFamily(
          source_family, directwrite_factory, nullptr, error);
  font_set.controls_stable =
      !font_set.disabled_source_family.empty() &&
      !font_set.disabled_replacement_family.empty() &&
      _wcsicmp(font_set.disabled_source_family.c_str(),
               repeated_disabled_font_set_source.c_str()) == 0 &&
      _wcsicmp(font_set.disabled_replacement_family.c_str(),
               repeated_disabled_font_set_replacement.c_str()) == 0 &&
      _wcsicmp(font_set.active_source_family.c_str(),
               repeated_active_font_set_source.c_str()) == 0;
  font_set.replacement_geometry_coherent = FontGeometryEqual(
      disabled_font_set_replacement_face.get(),
      active_font_set_source_face.get());
  font_set.replacement_observed =
      font_set.controls_stable && font_set.replacement_geometry_coherent &&
      _wcsicmp(font_set.active_source_family.c_str(),
               font_set.disabled_source_family.c_str()) == 0;
  DirectWriteSubstitutionObservation& modern =
      result.direct_write_modern_collection;
  modern = observe_direct_write(SystemCollectionMode::factory3);
  const auto merge_modern_observation =
      [&modern](const DirectWriteSubstitutionObservation& observation) {
        modern.controls_stable =
            modern.controls_stable && observation.controls_stable;
        modern.replacement_geometry_coherent =
            modern.replacement_geometry_coherent &&
            observation.replacement_geometry_coherent;
        modern.replacement_observed =
            modern.replacement_observed && observation.replacement_observed;
        if (observation.active_source_family.empty()) {
          modern.active_source_family.clear();
        }
      };
  IDWriteFactory* const factory =
      static_cast<IDWriteFactory*>(directwrite_factory);
  constexpr std::array<SystemCollectionMode, 6> modern_modes = {
      SystemCollectionMode::factory6_weight_stretch_style,
      SystemCollectionMode::factory6_typographic,
      SystemCollectionMode::factory6_font_set,
      SystemCollectionMode::factory7_weight_stretch_style,
      SystemCollectionMode::factory7_typographic,
      SystemCollectionMode::factory7_font_set,
  };
  for (const SystemCollectionMode mode : modern_modes) {
    if (SupportsSystemCollectionMode(factory, mode)) {
      merge_modern_observation(observe_direct_write(mode));
    }
  }
  if (SupportsSystemCollectionMode(
          factory, SystemCollectionMode::factory6_typographic)) {
    merge_modern_observation(observe_typographic_fallback());
  }
  if (!ObservePinnedAxisLayouts(source_family, replacement_family,
                                directwrite_factory, error)) {
    result.active_source_fingerprint.clear();
    return result;
  }
  if (result.direct_write.active_source_family.empty() ||
      indexed.active_source_family.empty() ||
      indexed.retained_generation_descriptor_family.empty() ||
      indexed.active_alias_descriptor_family.empty() ||
      font_set.active_source_family.empty() ||
      result.direct_write_modern_collection.active_source_family.empty()) {
    result.active_source_fingerprint.clear();
  }
  return result;
}

}  // namespace mactype::service_probe::internal

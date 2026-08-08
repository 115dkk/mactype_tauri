#pragma once

#include <string>
#include <string_view>

namespace mactype::service_probe::internal {

struct DirectWriteSubstitutionObservation final {
  std::wstring disabled_source_family;
  std::wstring active_source_family;
  std::wstring disabled_replacement_family;
  std::wstring disabled_source_postscript_name;
  std::wstring active_source_postscript_name;
  std::wstring disabled_replacement_postscript_name;
  std::wstring active_replacement_postscript_name;
  std::wstring retained_generation_family;
  std::wstring retained_generation_descriptor_family;
  std::wstring active_replacement_descriptor_family;
  bool controls_stable = false;
  bool replacement_metadata_coherent = false;
  bool replacement_name_table_coherent = false;
  bool retained_generation_object_stable = false;
  bool retained_generation_descriptor_stable = false;
  bool replacement_descriptor_coherent = false;
  bool replacement_observed = false;
};

struct FontSubstitutionObservation final {
  std::string disabled_source_fingerprint;
  std::string active_source_fingerprint;
  std::string disabled_replacement_fingerprint;
  bool controls_stable = false;
  bool replacement_observed = false;
  DirectWriteSubstitutionObservation direct_write;
  DirectWriteSubstitutionObservation direct_write_indexed_collection;
  DirectWriteSubstitutionObservation direct_write_font_set_collection;
  DirectWriteSubstitutionObservation direct_write_modern_collection;
};

void* CreateDirectWriteFactory(bool isolated, std::wstring& error);
void ReleaseDirectWriteFactory(void* factory) noexcept;
FontSubstitutionObservation ObserveFontSubstitution(
    std::wstring_view source_family, std::wstring_view replacement_family,
    void* directwrite_factory, std::wstring& error);

}  // namespace mactype::service_probe::internal

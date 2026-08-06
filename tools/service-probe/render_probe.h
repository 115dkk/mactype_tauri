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
  std::wstring active_retained_source_postscript_name;
  std::wstring active_pinned_source_family;
  std::wstring active_pinned_source_descriptor_family;
  bool controls_stable = false;
  bool retained_metadata_stable = false;
  bool retained_object_replacement_observed = false;
  bool retained_descriptor_replacement_observed = false;
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
  DirectWriteSubstitutionObservation direct_write_custom_collection;
};

void* CreateDirectWriteFactory(bool isolated, std::wstring& error);
void ReleaseDirectWriteFactory(void* factory) noexcept;
FontSubstitutionObservation ObserveFontSubstitution(
    std::wstring_view source_family, std::wstring_view replacement_family,
    void* directwrite_factory, std::wstring& error);

}  // namespace mactype::service_probe::internal

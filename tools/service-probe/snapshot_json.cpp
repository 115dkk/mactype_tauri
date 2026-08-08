#include "snapshot_json.h"

#include <sstream>

namespace mactype::service_probe::internal {
namespace {

std::string OptionalJsonString(const std::wstring& value) {
  return value.empty() ? "null" : "\"" + EscapeJson(value) + "\"";
}

const ModuleObservation* SelectPrimaryModule(const ObservedModules& observed,
                                              const std::string& architecture) {
  const std::wstring preferred_name =
      architecture == "x64" ? L"MacType64.dll" : L"MacType.dll";
  for (const auto& [path, module] : observed) {
    static_cast<void>(path);
    if (_wcsicmp(module.name.c_str(), preferred_name.c_str()) == 0) {
      return &module;
    }
  }
  return observed.empty() ? nullptr : &observed.begin()->second;
}

}  // namespace

std::string BuildSnapshotJson(const ProbeOptions& options,
                              const ObservedModules& observed,
                              const FontSubstitutionObservation& font_substitution,
                              const std::string& observed_at) {
  const ProcessObservation process = CaptureProcessObservation();
  const ModuleObservation* primary =
      SelectPrimaryModule(observed, process.architecture);
  OSVERSIONINFOW reported_windows_version{};
  reported_windows_version.dwOSVersionInfoSize =
      sizeof(reported_windows_version);
#pragma warning(suppress : 4996)
  GetVersionExW(&reported_windows_version);

  std::ostringstream json;
  json << "{\n"
       << "  \"schemaVersion\": 1,\n"
       << "  \"probeKind\": \"" << EscapeJson(options.probe_kind) << "\",\n"
       << "  \"role\": \"" << EscapeJson(options.role) << "\",\n"
       << "  \"treeLevel\": " << options.tree_level << ",\n"
       << "  \"pid\": " << process.pid << ",\n"
       << "  \"parentPid\": " << process.parent_pid << ",\n"
       << "  \"sessionId\": " << process.session_id << ",\n"
       << "  \"architecture\": \"" << process.architecture << "\",\n"
       << "  \"processMachine\": \"" << process.process_machine << "\",\n"
       << "  \"nativeMachine\": \"" << process.native_machine << "\",\n"
       << "  \"integrity\": \"" << EscapeJson(process.integrity) << "\",\n"
       << "  \"reportedWindowsMajorVersion\": "
       << reported_windows_version.dwMajorVersion << ",\n"
       << "  \"reportedWindowsMinorVersion\": "
       << reported_windows_version.dwMinorVersion << ",\n"
       << "  \"startedAt\": \"" << process.started_at << "\",\n"
       << "  \"observedAt\": \"" << observed_at << "\",\n"
       << "  \"waitMilliseconds\": " << options.wait_milliseconds << ",\n"
       << "  \"directWriteFactoryPrecreated\": "
       << (options.precreate_directwrite_factory ? "true" : "false") << ",\n"
       << "  \"directWriteFactoryType\": "
       << (options.precreate_directwrite_factory
               ? (options.precreate_isolated_directwrite_factory
                      ? "\"isolated\""
                      : "\"shared\"")
               : "null")
       << ",\n"
       << "  \"mactypeModuleLoaded\": "
       << (!observed.empty() ? "true" : "false") << ",\n"
       << "  \"mactypeModulePath\": "
       << (primary == nullptr ? "null" : OptionalJsonString(primary->path))
       << ",\n"
       << "  \"mactypeVersion\": "
       << (primary == nullptr ? "null" : OptionalJsonString(primary->version))
       << ",\n"
       << "  \"versionSource\": "
       << (primary == nullptr ? "null"
                              : OptionalJsonString(primary->version_source))
       << ",\n"
       << "  \"loadObservedAt\": "
       << (primary == nullptr ? "null"
                              : "\"" + primary->first_observed_at + "\"")
       << ",\n"
       << "  \"renderFingerprint\": \""
       << font_substitution.active_source_fingerprint << "\",\n"
       << "  \"render\": {\n"
       << "    \"width\": 320,\n"
       << "    \"height\": 96,\n"
       << "    \"pixelFormat\": \"BGRA8-top-down\",\n"
       << "    \"font\": \"" << EscapeJson(options.font_source) << "\",\n"
       << "    \"text\": \"MacType probe 0123456789 Aa 中 あ\"\n"
       << "  },\n"
       << "  \"fontSubstitution\": {\n"
       << "    \"sourceFamily\": \"" << EscapeJson(options.font_source)
       << "\",\n"
       << "    \"replacementFamily\": \""
       << EscapeJson(options.font_replacement) << "\",\n"
       << "    \"gdi\": {\n"
       << "      \"disabledSourceFingerprint\": \""
       << font_substitution.disabled_source_fingerprint << "\",\n"
       << "      \"activeSourceFingerprint\": \""
       << font_substitution.active_source_fingerprint << "\",\n"
       << "      \"disabledReplacementFingerprint\": \""
       << font_substitution.disabled_replacement_fingerprint << "\",\n"
       << "      \"controlsStable\": "
       << (font_substitution.controls_stable ? "true" : "false") << ",\n"
       << "      \"replacementObserved\": "
       << (font_substitution.replacement_observed ? "true" : "false") << "\n"
       << "    },\n"
       << "    \"directWrite\": {\n"
       << "      \"disabledSourceFamily\": \""
       << EscapeJson(font_substitution.direct_write.disabled_source_family)
       << "\",\n"
       << "      \"activeSourceFamily\": \""
       << EscapeJson(font_substitution.direct_write.active_source_family)
       << "\",\n"
       << "      \"disabledReplacementFamily\": \""
       << EscapeJson(
              font_substitution.direct_write.disabled_replacement_family)
       << "\",\n"
       << "      \"controlsStable\": "
       << (font_substitution.direct_write.controls_stable ? "true" : "false")
       << ",\n"
       << "      \"replacementObserved\": "
       << (font_substitution.direct_write.replacement_observed ? "true"
                                                               : "false")
       << "\n"
       << "    },\n"
       << "    \"directWriteIndexedCollection\": {\n"
       << "      \"disabledSourceFamily\": \""
       << EscapeJson(font_substitution.direct_write_indexed_collection
                         .disabled_source_family)
       << "\",\n"
       << "      \"activeSourceFamily\": \""
       << EscapeJson(font_substitution.direct_write_indexed_collection
                         .active_source_family)
       << "\",\n"
       << "      \"disabledReplacementFamily\": \""
       << EscapeJson(font_substitution.direct_write_indexed_collection
                         .disabled_replacement_family)
       << "\",\n"
       << "      \"disabledSourcePostScriptName\": \""
       << EscapeJson(font_substitution.direct_write_indexed_collection
                         .disabled_source_postscript_name)
       << "\",\n"
       << "      \"activeSourcePostScriptName\": \""
       << EscapeJson(font_substitution.direct_write_indexed_collection
                         .active_source_postscript_name)
       << "\",\n"
       << "      \"disabledReplacementPostScriptName\": \""
       << EscapeJson(font_substitution.direct_write_indexed_collection
                         .disabled_replacement_postscript_name)
       << "\",\n"
       << "      \"activeReplacementPostScriptName\": \""
       << EscapeJson(font_substitution.direct_write_indexed_collection
                         .active_replacement_postscript_name)
       << "\",\n"
       << "      \"retainedGenerationFamily\": \""
       << EscapeJson(font_substitution.direct_write_indexed_collection
                          .retained_generation_family)
       << "\",\n"
       << "      \"retainedGenerationDescriptorFamily\": \""
       << EscapeJson(font_substitution.direct_write_indexed_collection
                          .retained_generation_descriptor_family)
       << "\",\n"
       << "      \"activeReplacementDescriptorFamily\": \""
       << EscapeJson(font_substitution.direct_write_indexed_collection
                          .active_replacement_descriptor_family)
       << "\",\n"
       << "      \"controlsStable\": "
       << (font_substitution.direct_write_indexed_collection.controls_stable
               ? "true"
               : "false")
       << ",\n"
       << "      \"replacementMetadataCoherent\": "
       << (font_substitution.direct_write_indexed_collection
                    .replacement_metadata_coherent
               ? "true"
               : "false")
       << ",\n"
       << "      \"replacementNameTableCoherent\": "
       << (font_substitution.direct_write_indexed_collection
                    .replacement_name_table_coherent
               ? "true"
               : "false")
       << ",\n"
       << "      \"retainedGenerationObjectStable\": "
       << (font_substitution.direct_write_indexed_collection
                    .retained_generation_object_stable
               ? "true"
               : "false")
       << ",\n"
       << "      \"retainedGenerationDescriptorStable\": "
       << (font_substitution.direct_write_indexed_collection
                    .retained_generation_descriptor_stable
               ? "true"
               : "false")
       << ",\n"
       << "      \"replacementDescriptorCoherent\": "
       << (font_substitution.direct_write_indexed_collection
                    .replacement_descriptor_coherent
               ? "true"
               : "false")
       << ",\n"
       << "      \"replacementObserved\": "
       << (font_substitution.direct_write_indexed_collection
                   .replacement_observed
               ? "true"
               : "false")
       << "\n"
       << "    },\n"
       << "    \"directWriteFontSetCollection\": {\n"
       << "      \"disabledSourceFamily\": \""
       << EscapeJson(font_substitution.direct_write_font_set_collection
                         .disabled_source_family)
       << "\",\n"
       << "      \"activeSourceFamily\": \""
       << EscapeJson(font_substitution.direct_write_font_set_collection
                         .active_source_family)
       << "\",\n"
       << "      \"disabledReplacementFamily\": \""
       << EscapeJson(font_substitution.direct_write_font_set_collection
                         .disabled_replacement_family)
       << "\",\n"
       << "      \"controlsStable\": "
       << (font_substitution.direct_write_font_set_collection.controls_stable
               ? "true"
               : "false")
       << ",\n"
       << "      \"replacementObserved\": "
       << (font_substitution.direct_write_font_set_collection
                   .replacement_observed
               ? "true"
               : "false")
       << "\n"
       << "    },\n"
       << "    \"directWriteModernCollection\": {\n"
       << "      \"disabledSourceFamily\": \""
       << EscapeJson(font_substitution.direct_write_modern_collection
                         .disabled_source_family)
       << "\",\n"
       << "      \"activeSourceFamily\": \""
       << EscapeJson(font_substitution.direct_write_modern_collection
                         .active_source_family)
       << "\",\n"
       << "      \"disabledReplacementFamily\": \""
       << EscapeJson(font_substitution.direct_write_modern_collection
                         .disabled_replacement_family)
       << "\",\n"
       << "      \"controlsStable\": "
       << (font_substitution.direct_write_modern_collection.controls_stable
               ? "true"
               : "false")
       << ",\n"
       << "      \"replacementObserved\": "
       << (font_substitution.direct_write_modern_collection
                   .replacement_observed
               ? "true"
               : "false")
       << "\n"
       << "    }\n"
       << "  },\n"
       << "  \"modules\": [";

  bool first = true;
  for (const auto& [path, module] : observed) {
    static_cast<void>(path);
    if (!first) {
      json << ',';
    }
    first = false;
    json << "\n    {\"name\": \"" << EscapeJson(module.name)
         << "\", \"path\": \"" << EscapeJson(module.path)
         << "\", \"version\": " << OptionalJsonString(module.version)
         << ", \"versionSource\": "
         << OptionalJsonString(module.version_source)
         << ", \"firstObservedAt\": \"" << module.first_observed_at << "\"}";
  }
  if (!observed.empty()) {
    json << '\n';
  }
  json << "  ]\n}\n";
  return json.str();
}

}  // namespace mactype::service_probe::internal

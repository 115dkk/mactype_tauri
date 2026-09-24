cmake_minimum_required(VERSION 3.24)

project(mactype_service_probe LANGUAGES CXX)

option(MACTYPE_ENABLE_ASAN "Instrument renderer runtime tests with AddressSanitizer" OFF)

if(NOT WIN32)
  message(FATAL_ERROR "The MacType service probes only support Windows.")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(PROBE_ARCH_SUFFIX 64)
else()
  set(PROBE_ARCH_SUFFIX 32)
endif()

add_library(service-probe-common STATIC
  child_process.cpp
  child_process.h
  process_observation.cpp
  process_observation.h
  probe_common.cpp
  probe_common.h
  probe_io.cpp
  render_probe.cpp
  render_probe.h
  snapshot_json.cpp
  snapshot_json.h
  win32_support.cpp
  win32_support.h
)
target_compile_features(service-probe-common PUBLIC cxx_std_20)
target_compile_definitions(service-probe-common PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(service-probe-common PUBLIC advapi32 bcrypt dwrite gdi32 shell32 user32 version)

add_executable(probe-console probe_main.cpp)
target_link_libraries(probe-console PRIVATE service-probe-common)
set_target_properties(probe-console PROPERTIES OUTPUT_NAME "probe-console${PROBE_ARCH_SUFFIX}")

add_executable(probe-window WIN32 probe_main.cpp)
target_compile_definitions(probe-window PRIVATE SERVICE_PROBE_WINDOW=1)
target_link_libraries(probe-window PRIVATE service-probe-common)
set_target_properties(probe-window PROPERTIES OUTPUT_NAME "probe-window${PROBE_ARCH_SUFFIX}")

add_executable(probe-spawn-tree spawn_tree.cpp)
target_link_libraries(probe-spawn-tree PRIVATE service-probe-common)
set_target_properties(probe-spawn-tree PROPERTIES OUTPUT_NAME "probe-spawn-tree${PROBE_ARCH_SUFFIX}")

add_executable(probe-timeout-fixture EXCLUDE_FROM_ALL
  tests/fixtures/timeout_tree.cpp
)
target_compile_features(probe-timeout-fixture PRIVATE cxx_std_20)
target_compile_definitions(probe-timeout-fixture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(probe-timeout-fixture PROPERTIES OUTPUT_NAME "probe-timeout-fixture${PROBE_ARCH_SUFFIX}")

add_executable(renderer-raii-tests tests/renderer_raii_tests.cpp)
target_compile_features(renderer-raii-tests PRIVATE cxx_std_20)
target_compile_definitions(renderer-raii-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(hook-lifecycle-tests
  tests/hook_lifecycle_tests.cpp
  ../../renderer/hook_lifecycle.cpp
)
target_compile_features(hook-lifecycle-tests PRIVATE cxx_std_20)
target_compile_definitions(hook-lifecycle-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(activation-repaint-tests
  tests/activation_repaint_tests.cpp
  ../../renderer/activation_repaint.cpp
  ../../renderer/arrival_evidence.cpp
)
target_compile_features(activation-repaint-tests PRIVATE cxx_std_20)
target_compile_definitions(activation-repaint-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(activation-repaint-tests PRIVATE user32)

add_executable(freetype-runtime-tests tests/freetype_runtime_tests.cpp)
target_compile_features(freetype-runtime-tests PRIVATE cxx_std_20)
target_compile_definitions(freetype-runtime-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(private-freetype-policy-tests
  tests/private_freetype_policy_tests.cpp
  ../../renderer/private_freetype_policy.cpp
)
target_compile_features(private-freetype-policy-tests PRIVATE cxx_std_20)
target_compile_definitions(private-freetype-policy-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(image-subsystem-tests
  tests/image_subsystem_tests.cpp
  ../../renderer/image_subsystem.cpp
)
target_compile_features(image-subsystem-tests PRIVATE cxx_std_20)
target_compile_definitions(image-subsystem-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(module-name-tests
  tests/module_name_tests.cpp
  ../../renderer/module_name.cpp
)
target_compile_features(module-name-tests PRIVATE cxx_std_20)
target_compile_definitions(module-name-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(directwrite-font-cache-tests
  tests/directwrite_font_cache_tests.cpp
  ../../renderer/virtual_font_cache.cpp
)
target_compile_features(directwrite-font-cache-tests PRIVATE cxx_std_20)
target_compile_definitions(directwrite-font-cache-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(directwrite-font-cache-tests PRIVATE bcrypt advapi32)

add_executable(startup-debug-probe tests/startup_debug_probe.cpp)
target_compile_features(startup-debug-probe PRIVATE cxx_std_20)
target_compile_definitions(startup-debug-probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(startup-debug-probe PRIVATE /W4 /WX /permissive- /utf-8 /sdl)
target_link_libraries(startup-debug-probe PRIVATE dbghelp user32 gdi32)

add_executable(startup-stack-fixture tests/startup_stack_fixture.cpp)
target_compile_features(startup-stack-fixture PRIVATE cxx_std_20)
target_compile_definitions(startup-stack-fixture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(startup-stack-fixture PRIVATE /W4 /WX /permissive- /utf-8 /sdl)

add_executable(pe-export-view-tests
  tests/pe_export_view_tests.cpp
  ../../renderer/dll.cpp
)
target_compile_features(pe-export-view-tests PRIVATE cxx_std_20)
target_compile_definitions(pe-export-view-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(unload-lifecycle-tests
  tests/unload_lifecycle_tests.cpp
  ../../renderer/unload_lifecycle.cpp
)
target_compile_features(unload-lifecycle-tests PRIVATE cxx_std_20)
target_compile_definitions(unload-lifecycle-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(renderer-policy-tests
  tests/renderer_policy_tests.cpp
  ../../renderer/renderer_policy.cpp
  ../../renderer/profile_runtime.cpp
  ../../renderer/font_substitution.cpp
)
target_compile_features(renderer-policy-tests PRIVATE cxx_std_20)
target_compile_definitions(renderer-policy-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(renderer-activation-tests
  tests/renderer_activation_tests.cpp
  ../../renderer/renderer_activation.cpp
  ../../renderer/hook_lifecycle.cpp
  ../../renderer/renderer_policy.cpp
  ../../renderer/profile_runtime.cpp
  ../../renderer/font_substitution.cpp
)
target_compile_features(renderer-activation-tests PRIVATE cxx_std_20)
target_compile_definitions(renderer-activation-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(font-substitution-tests
  tests/font_substitution_tests.cpp
  ../../renderer/font_substitution.cpp
)
target_compile_features(font-substitution-tests PRIVATE cxx_std_20)
target_compile_definitions(font-substitution-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(bold-face-selection-tests
  tests/bold_face_selection_tests.cpp
  ../../renderer/bold_face_selection.cpp
)
target_compile_features(bold-face-selection-tests PRIVATE cxx_std_20)
target_compile_definitions(bold-face-selection-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(directwrite-virtual-font-tests
  tests/directwrite_virtual_font_tests.cpp
  ../../renderer/directwrite_virtual_font.cpp
  ../../renderer/sfnt_names.cpp
  ../../renderer/virtual_font_cache.cpp
)
target_compile_features(directwrite-virtual-font-tests PRIVATE cxx_std_20)
target_compile_definitions(directwrite-virtual-font-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(directwrite-virtual-font-tests PRIVATE dwrite bcrypt advapi32)

add_executable(directwrite-alias-policy-tests
  tests/directwrite_alias_policy_tests.cpp
  ../../renderer/directwrite_alias_policy.cpp
  ../../renderer/font_substitution.cpp
)
target_compile_features(directwrite-alias-policy-tests PRIVATE cxx_std_20)
target_compile_definitions(directwrite-alias-policy-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(arrival-evidence-tests
  tests/arrival_evidence_tests.cpp
  ../../renderer/arrival_evidence.cpp
)
target_compile_features(arrival-evidence-tests PRIVATE cxx_std_20)
target_compile_definitions(arrival-evidence-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(arrival-evidence-tests PRIVATE user32 gdi32)

add_executable(gdi-font-identity-tests
  tests/gdi_font_identity_tests.cpp
  ../../renderer/gdi_font_identity.cpp
  ../../renderer/arrival_evidence.cpp
)
target_compile_features(gdi-font-identity-tests PRIVATE cxx_std_20)
target_compile_definitions(gdi-font-identity-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(gdi-font-identity-tests PRIVATE user32 gdi32)

add_executable(unity-font-hook-tests
  tests/unity_font_hook_tests.cpp
  ../../renderer/unity_font_catalog.cpp
  ../../renderer/unity_font_evidence.cpp
  ../../renderer/unity_font_hook_core.cpp
  ../../renderer/unity_font_selection_context.cpp
  ../../renderer/renderer_policy.cpp
  ../../renderer/font_substitution.cpp
)
target_compile_features(unity-font-hook-tests PRIVATE cxx_std_20)
target_compile_definitions(unity-font-hook-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(unity-font-catalog-probe
  tests/unity_font_catalog_probe.cpp
  ../../renderer/unity_font_catalog.cpp
  ../../renderer/unity_font_evidence.cpp
  ../../renderer/unity_font_hook_core.cpp
  ../../renderer/font_substitution.cpp
)
target_compile_features(unity-font-catalog-probe PRIVATE cxx_std_20)
target_compile_definitions(unity-font-catalog-probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(renderer-profile-probe tests/renderer_profile_probe.cpp)
target_compile_features(renderer-profile-probe PRIVATE cxx_std_20)
target_compile_definitions(renderer-profile-probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(renderer-profile-probe PROPERTIES
  OUTPUT_NAME "renderer-profile-probe${PROBE_ARCH_SUFFIX}"
)

add_executable(bold-substitution-probe tests/bold_substitution_probe.cpp)
target_link_libraries(bold-substitution-probe PRIVATE service-probe-common)
set_target_properties(bold-substitution-probe PROPERTIES
  OUTPUT_NAME "bold-substitution-probe${PROBE_ARCH_SUFFIX}"
)

add_executable(browser-launch-gate browser_launch_gate.cpp)
target_link_libraries(browser-launch-gate PRIVATE service-probe-common)
set_target_properties(browser-launch-gate PROPERTIES OUTPUT_NAME "browser-launch-gate${PROBE_ARCH_SUFFIX}")

add_library(dwritecore-proxy SHARED
  tests/fixtures/dwritecore_proxy.cpp
  tests/fixtures/dwritecore_proxy.def
)
target_compile_features(dwritecore-proxy PRIVATE cxx_std_20)
target_compile_definitions(dwritecore-proxy PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dwritecore-proxy PRIVATE dwrite)
set_target_properties(dwritecore-proxy PROPERTIES PREFIX "" OUTPUT_NAME "DWriteCore")

add_executable(dwritecore-contract-probe tests/dwritecore_contract_probe.cpp)
target_compile_features(dwritecore-contract-probe PRIVATE cxx_std_20)
target_compile_definitions(dwritecore-contract-probe PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
set_target_properties(dwritecore-contract-probe PROPERTIES
  OUTPUT_NAME "dwritecore-contract-probe${PROBE_ARCH_SUFFIX}"
)

add_executable(relay-policy-probe tests/relay_policy_probe.cpp)
target_compile_features(relay-policy-probe PRIVATE cxx_std_20)
target_compile_definitions(relay-policy-probe PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
set_target_properties(relay-policy-probe PROPERTIES
  OUTPUT_NAME "relay-policy-probe${PROBE_ARCH_SUFFIX}"
)

if(MSVC)
  foreach(target service-probe-common probe-console probe-window probe-spawn-tree probe-timeout-fixture renderer-raii-tests hook-lifecycle-tests activation-repaint-tests freetype-runtime-tests private-freetype-policy-tests image-subsystem-tests module-name-tests directwrite-font-cache-tests pe-export-view-tests unload-lifecycle-tests renderer-policy-tests renderer-activation-tests font-substitution-tests bold-face-selection-tests directwrite-virtual-font-tests directwrite-alias-policy-tests arrival-evidence-tests gdi-font-identity-tests unity-font-hook-tests unity-font-catalog-probe renderer-profile-probe bold-substitution-probe browser-launch-gate dwritecore-proxy dwritecore-contract-probe relay-policy-probe)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /sdl)
  endforeach()
  foreach(target probe-console probe-window probe-spawn-tree probe-timeout-fixture renderer-profile-probe bold-substitution-probe browser-launch-gate dwritecore-contract-probe relay-policy-probe)
    target_link_options(${target} PRIVATE
      /MANIFEST:EMBED
      "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/probe.manifest"
    )
  endforeach()
  if(MACTYPE_ENABLE_ASAN)
    foreach(target renderer-raii-tests hook-lifecycle-tests activation-repaint-tests freetype-runtime-tests private-freetype-policy-tests image-subsystem-tests module-name-tests directwrite-font-cache-tests pe-export-view-tests unload-lifecycle-tests renderer-policy-tests font-substitution-tests bold-face-selection-tests directwrite-virtual-font-tests directwrite-alias-policy-tests unity-font-hook-tests)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    endforeach()
  endif()
endif()

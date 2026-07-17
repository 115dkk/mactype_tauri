#include "module_inventory.h"

#include <psapi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mactype::injector {
namespace {

constexpr std::size_t kMaxModules = 4'096U;
constexpr std::size_t kMaxModulePathCharacters = 32'768U;

[[nodiscard]] bool starts_with_case_insensitive(const std::wstring_view text,
                                                const std::wstring_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           _wcsnicmp(text.data(), prefix.data(), prefix.size()) == 0;
}

[[nodiscard]] std::optional<std::wstring> normalized_module_path(
    const std::wstring_view input) noexcept {
    if (input.empty() || input.find(L'\0') != std::wstring_view::npos) {
        return std::nullopt;
    }
    try {
        std::wstring normalized;
        if (starts_with_case_insensitive(input, LR"(\\?\UNC\)") ||
            starts_with_case_insensitive(input, LR"(\??\UNC\)")) {
            normalized = LR"(\\)";
            normalized.append(input.substr(8U));
        } else if (starts_with_case_insensitive(input, LR"(\\?\)") ||
                   starts_with_case_insensitive(input, LR"(\??\)")) {
            normalized.assign(input.substr(4U));
        } else {
            normalized.assign(input);
        }
        const std::filesystem::path path{normalized};
        if (!path.is_absolute()) {
            return std::nullopt;
        }
        return path.lexically_normal().native();
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::vector<HMODULE>> enumerate_modules(
    HANDLE process) noexcept {
    try {
        std::vector<HMODULE> modules(kMaxModules);
        DWORD needed = 0U;
        const auto capacity = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
        if (!K32EnumProcessModulesEx(process, modules.data(), capacity, &needed,
                                     LIST_MODULES_ALL) ||
            needed > capacity || needed % sizeof(HMODULE) != 0U) {
            return std::nullopt;
        }
        modules.resize(needed / sizeof(HMODULE));
        return modules;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::wstring_view> module_path(
    HANDLE process, const HMODULE module, std::vector<wchar_t>& path) noexcept {
    const DWORD length = K32GetModuleFileNameExW(
        process, module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size()) {
        return std::nullopt;
    }
    return std::wstring_view{path.data(), length};
}

[[nodiscard]] std::optional<std::uintptr_t> remote_module_base(
    HANDLE process, const std::wstring_view module_name) noexcept {
    const auto inventory = enumerate_modules(process);
    if (!inventory) {
        return std::nullopt;
    }
    try {
        std::vector<wchar_t> path(kMaxModulePathCharacters);
        for (const HMODULE module : *inventory) {
            const auto current_path = module_path(process, module, path);
            if (!current_path) {
                return std::nullopt;
            }
            const std::filesystem::path parsed{*current_path};
            if (_wcsicmp(parsed.filename().c_str(), module_name.data()) == 0) {
                return reinterpret_cast<std::uintptr_t>(module);
            }
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

bool module_paths_equal(const std::wstring_view left,
                        const std::wstring_view right) noexcept {
    const auto normalized_left = normalized_module_path(left);
    const auto normalized_right = normalized_module_path(right);
    return normalized_left && normalized_right &&
           _wcsicmp(normalized_left->c_str(), normalized_right->c_str()) == 0;
}

std::optional<bool> fixed_module_is_loaded(
    HANDLE process, const std::filesystem::path& expected_path) noexcept {
    const auto inventory = enumerate_modules(process);
    if (!inventory) {
        return std::nullopt;
    }
    try {
        std::vector<wchar_t> path(kMaxModulePathCharacters);
        for (const HMODULE module : *inventory) {
            const auto current_path = module_path(process, module, path);
            if (!current_path) {
                return std::nullopt;
            }
            if (module_paths_equal(*current_path, expected_path.native())) {
                return true;
            }
        }
    } catch (...) {
        return std::nullopt;
    }
    return false;
}

std::optional<LPTHREAD_START_ROUTINE> remote_load_library(HANDLE process) noexcept {
    const auto local_kernel = GetModuleHandleW(L"kernel32.dll");
    if (local_kernel == nullptr) {
        return std::nullopt;
    }
    const auto local_load_library = GetProcAddress(local_kernel, "LoadLibraryW");
    if (local_load_library == nullptr) {
        return std::nullopt;
    }

    MEMORY_BASIC_INFORMATION implementation{};
    if (VirtualQuery(reinterpret_cast<const void*>(local_load_library), &implementation,
                     sizeof(implementation)) != sizeof(implementation) ||
        implementation.AllocationBase == nullptr) {
        return std::nullopt;
    }
    const auto implementation_module = static_cast<HMODULE>(implementation.AllocationBase);
    std::array<wchar_t, MAX_PATH> implementation_path{};
    const DWORD length = GetModuleFileNameW(implementation_module, implementation_path.data(),
                                            static_cast<DWORD>(implementation_path.size()));
    if (length == 0U || length >= implementation_path.size()) {
        return std::nullopt;
    }
    const wchar_t* filename = std::wcsrchr(implementation_path.data(), L'\\');
    filename = filename == nullptr ? implementation_path.data() : filename + 1;
    const auto remote_implementation = remote_module_base(process, filename);
    if (!remote_implementation) {
        return std::nullopt;
    }
    const auto offset = reinterpret_cast<std::uintptr_t>(local_load_library) -
                        reinterpret_cast<std::uintptr_t>(implementation.AllocationBase);
    return reinterpret_cast<LPTHREAD_START_ROUTINE>(*remote_implementation + offset);
}

}  // namespace mactype::injector

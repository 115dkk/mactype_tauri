#pragma once

#include <windows.h>

#include <algorithm>

namespace mactype::injector {

class OperationDeadline final {
public:
    static constexpr DWORD kHelperBudgetMs = 19'000U;

    OperationDeadline() noexcept
        : expires_at_{GetTickCount64() + kHelperBudgetMs} {}

    [[nodiscard]] DWORD remaining(DWORD maximum) const noexcept {
        const ULONGLONG now = GetTickCount64();
        if (now >= expires_at_) {
            return 0U;
        }
        const ULONGLONG remaining = expires_at_ - now;
        return static_cast<DWORD>(
            std::min<ULONGLONG>(remaining, static_cast<ULONGLONG>(maximum)));
    }

    [[nodiscard]] DWORD remaining_before_reserve(
        DWORD maximum, DWORD reserve) const noexcept {
        const DWORD available = remaining(MAXDWORD);
        if (available <= reserve) {
            return 0U;
        }
        return std::min(maximum, available - reserve);
    }

private:
    ULONGLONG expires_at_{};
};

}  // namespace mactype::injector

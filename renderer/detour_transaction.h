#pragma once

#include <Windows.h>
#include <mutex>

#include "detours.h"

namespace renderer_raii {

namespace detail {

inline std::mutex& ProcessDetoursTransactionMutex() noexcept
{
	static std::mutex mutex;
	return mutex;
}

struct DetoursApi
{
	static LONG Begin() noexcept { return ::DetourTransactionBegin(); }
	static LONG Abort() noexcept { return ::DetourTransactionAbort(); }
	static LONG Commit() noexcept { return ::DetourTransactionCommit(); }
	static LONG UpdateThread(HANDLE thread) noexcept { return ::DetourUpdateThread(thread); }
	static LONG Attach(PVOID* target, PVOID hook) noexcept { return ::DetourAttach(target, hook); }
	static LONG Detach(PVOID* target, PVOID hook) noexcept { return ::DetourDetach(target, hook); }
};

} // namespace detail

// Detours exposes a process-global C transaction.  This owner guarantees that
// every successfully begun transaction is either committed explicitly or
// aborted during stack unwinding and early returns.
template <typename Api = detail::DetoursApi>
class BasicDetourTransaction
{
public:
	BasicDetourTransaction()
		: transactionLock_(detail::ProcessDetoursTransactionMutex()),
		  active_(false), status_(Api::Begin())
	{
		if (status_ == NOERROR) {
			active_ = true;
			status_ = Api::UpdateThread(::GetCurrentThread());
		}
	}

	~BasicDetourTransaction() noexcept
	{
		if (active_) {
			Api::Abort();
		}
	}

	BasicDetourTransaction(const BasicDetourTransaction&) = delete;
	BasicDetourTransaction& operator=(const BasicDetourTransaction&) = delete;
	BasicDetourTransaction(BasicDetourTransaction&&) = delete;
	BasicDetourTransaction& operator=(BasicDetourTransaction&&) = delete;

	LONG Attach(PVOID* target, PVOID hook) noexcept
	{
		if (status_ == NOERROR) {
			status_ = Api::Attach(target, hook);
		}
		return status_;
	}

	LONG Detach(PVOID* target, PVOID hook) noexcept
	{
		if (status_ == NOERROR) {
			status_ = Api::Detach(target, hook);
		}
		return status_;
	}

	LONG Commit() noexcept
	{
		if (!active_) {
			return status_;
		}
		if (status_ != NOERROR) {
			Api::Abort();
			active_ = false;
			return status_;
		}
		status_ = Api::Commit();
		active_ = false;
		return status_;
	}

	LONG status() const noexcept { return status_; }
	bool active() const noexcept { return active_; }

private:
	// Detours exposes one transaction per process. Keep ownership ahead of the
	// transaction state so the lock is released only after a pending abort.
	std::unique_lock<std::mutex> transactionLock_;
	bool active_;
	LONG status_;
};

using DetourTransaction = BasicDetourTransaction<>;

} // namespace renderer_raii

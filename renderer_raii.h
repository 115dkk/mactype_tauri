#pragma once

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

namespace renderer_raii {

// The production aliases below deliberately use std::unique_ptr.  The custom
// code is limited to adapting non-C++ release rules and contextual leases to
// that standard owner.
namespace detail {

struct Win32ResourceApi
{
	static void CloseKernelHandle(HANDLE value) noexcept { ::CloseHandle(value); }
	static void CloseFindHandle(HANDLE value) noexcept { ::FindClose(value); }
	static void FreeLoadedModule(HMODULE value) noexcept { ::FreeLibrary(value); }
	static void DeleteDeviceContext(HDC value) noexcept { ::DeleteDC(value); }
	static void ReleaseDeviceContext(HWND window, HDC value) noexcept { ::ReleaseDC(window, value); }
	static void DeleteGdiObject(HGDIOBJ value) noexcept { ::DeleteObject(value); }
	static HGDIOBJ SelectGdiObject(HDC dc, HGDIOBJ value) noexcept { return ::SelectObject(dc, value); }
	static void CloseRegistryKey(HKEY value) noexcept { ::RegCloseKey(value); }
	static void FreeLocalMemory(HLOCAL value) noexcept { ::LocalFree(value); }
	static void FreeGlobalMemory(HGLOBAL value) noexcept { ::GlobalFree(value); }
	static void FreeSidMemory(PSID value) noexcept { ::FreeSid(value); }
	static void FreeEnvironmentBlock(LPWSTR value) noexcept { ::FreeEnvironmentStringsW(value); }
	static void UnmapView(void* value) noexcept { ::UnmapViewOfFile(value); }
	static void FreeVirtualMemory(void* value) noexcept { ::VirtualFree(value, 0, MEM_RELEASE); }
	static void FreeRemoteVirtualMemory(HANDLE process, void* value) noexcept
	{
		::VirtualFreeEx(process, value, 0, MEM_RELEASE);
	}
	static void FreeHeapMemory(HANDLE heap, void* value) noexcept { ::HeapFree(heap, 0, value); }
	static bool LockPages(void* value, SIZE_T size) noexcept { return ::VirtualLock(value, size) != FALSE; }
	static void UnlockPages(void* value, SIZE_T size) noexcept { ::VirtualUnlock(value, size); }
	static DWORD LastError() noexcept { return ::GetLastError(); }
};

template <typename Api>
struct KernelHandleCloser
{
	using pointer = HANDLE;
	void operator()(HANDLE value) const noexcept { Api::CloseKernelHandle(value); }
};

template <typename Api>
struct FindHandleCloser
{
	using pointer = HANDLE;
	void operator()(HANDLE value) const noexcept { Api::CloseFindHandle(value); }
};

template <typename Api>
struct LoadedModuleReleaser
{
	using pointer = HMODULE;
	void operator()(HMODULE value) const noexcept { Api::FreeLoadedModule(value); }
};

template <typename Api>
struct DeviceContextDeleter
{
	using pointer = HDC;
	void operator()(HDC value) const noexcept { Api::DeleteDeviceContext(value); }
};

template <typename Api>
struct DeviceContextReleaser
{
	using pointer = HDC;
	HWND window = nullptr;

	void operator()(HDC value) const noexcept { Api::ReleaseDeviceContext(window, value); }
};

template <typename Handle, typename Api>
struct GdiObjectDeleter
{
	using pointer = Handle;
	void operator()(Handle value) const noexcept
	{
		Api::DeleteGdiObject(reinterpret_cast<HGDIOBJ>(value));
	}
};

template <typename Handle, typename Api>
struct SelectedObjectRestorer
{
	using pointer = Handle;
	HDC dc = nullptr;

	void operator()(Handle previous) const noexcept
	{
		Api::SelectGdiObject(dc, reinterpret_cast<HGDIOBJ>(previous));
	}
};

template <typename Api>
struct RegistryKeyCloser
{
	using pointer = HKEY;
	void operator()(HKEY value) const noexcept { Api::CloseRegistryKey(value); }
};

template <typename T, typename Api>
struct LocalMemoryDeleter
{
	using pointer = T*;
	void operator()(T* value) const noexcept { Api::FreeLocalMemory(static_cast<HLOCAL>(value)); }
};

template <typename T, typename Api>
struct GlobalMemoryDeleter
{
	using pointer = T*;
	void operator()(T* value) const noexcept { Api::FreeGlobalMemory(static_cast<HGLOBAL>(value)); }
};

template <typename Api>
struct SidDeleter
{
	using pointer = PSID;
	void operator()(PSID value) const noexcept { Api::FreeSidMemory(value); }
};

template <typename Api>
struct EnvironmentBlockDeleter
{
	using pointer = LPWSTR;
	void operator()(LPWSTR value) const noexcept { Api::FreeEnvironmentBlock(value); }
};

template <typename Api>
struct MappedViewDeleter
{
	using pointer = void*;
	void operator()(void* value) const noexcept { Api::UnmapView(value); }
};

template <typename Api>
struct VirtualMemoryDeleter
{
	using pointer = void*;
	void operator()(void* value) const noexcept { Api::FreeVirtualMemory(value); }
};

template <typename Api>
struct RemoteVirtualMemoryDeleter
{
	using pointer = void*;
	HANDLE process = nullptr;
	void operator()(void* value) const noexcept { Api::FreeRemoteVirtualMemory(process, value); }
};

template <typename T, typename Api>
struct HeapMemoryDeleter
{
	using pointer = T*;
	HANDLE heap = nullptr;

	void operator()(T* value) const noexcept { Api::FreeHeapMemory(heap, value); }
};

template <typename T>
struct MallocDeleter
{
	void operator()(T* value) const noexcept { std::free(value); }
};

struct FileCloser
{
	void operator()(FILE* value) const noexcept { std::fclose(value); }
};

} // namespace detail

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueHandle = std::unique_ptr<void, detail::KernelHandleCloser<Api>>;

using UniqueHandle = BasicUniqueHandle<>;

template <typename Api = detail::Win32ResourceApi>
BasicUniqueHandle<Api> AdoptHandle(HANDLE value) noexcept
{
	return BasicUniqueHandle<Api>(value == INVALID_HANDLE_VALUE ? nullptr : value);
}

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueFindHandle = std::unique_ptr<void, detail::FindHandleCloser<Api>>;

using UniqueFindHandle = BasicUniqueFindHandle<>;

template <typename Api = detail::Win32ResourceApi>
BasicUniqueFindHandle<Api> AdoptFindHandle(HANDLE value) noexcept
{
	return BasicUniqueFindHandle<Api>(value == INVALID_HANDLE_VALUE ? nullptr : value);
}

// GetModuleHandle returns a borrowed view.  LoadLibrary and successful
// GetModuleHandleEx calls return references represented by UniqueModuleReference.
class BorrowedModule
{
public:
	explicit BorrowedModule(HMODULE value = nullptr) noexcept : value_(value) {}
	HMODULE get() const noexcept { return value_; }
	explicit operator bool() const noexcept { return value_ != nullptr; }

private:
	HMODULE value_;
};

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueModuleReference = std::unique_ptr<void, detail::LoadedModuleReleaser<Api>>;

using UniqueModuleReference = BasicUniqueModuleReference<>;

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueDeviceContext = std::unique_ptr<void, detail::DeviceContextDeleter<Api>>;

using UniqueDeviceContext = BasicUniqueDeviceContext<>;

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueWindowDeviceContext = std::unique_ptr<void, detail::DeviceContextReleaser<Api>>;

using UniqueWindowDeviceContext = BasicUniqueWindowDeviceContext<>;

template <typename Api = detail::Win32ResourceApi>
BasicUniqueWindowDeviceContext<Api> AdoptWindowDeviceContext(HWND window, HDC dc) noexcept
{
	return BasicUniqueWindowDeviceContext<Api>(dc, detail::DeviceContextReleaser<Api>{window});
}

template <typename Handle, typename Api = detail::Win32ResourceApi>
using BasicUniqueGdiObject = std::unique_ptr<void, detail::GdiObjectDeleter<Handle, Api>>;

using UniqueBitmap = BasicUniqueGdiObject<HBITMAP>;
using UniqueBrush = BasicUniqueGdiObject<HBRUSH>;
using UniqueFont = BasicUniqueGdiObject<HFONT>;
using UniquePen = BasicUniqueGdiObject<HPEN>;
using UniqueRegion = BasicUniqueGdiObject<HRGN>;

template <typename Handle, typename Api = detail::Win32ResourceApi>
using BasicSelectedObject = std::unique_ptr<void, detail::SelectedObjectRestorer<Handle, Api>>;

template <typename Handle, typename Api = detail::Win32ResourceApi>
BasicSelectedObject<Handle, Api> SelectObject(HDC dc, Handle value) noexcept
{
	HGDIOBJ previous = Api::SelectGdiObject(dc, reinterpret_cast<HGDIOBJ>(value));
	if (previous == nullptr || previous == HGDI_ERROR) {
		previous = nullptr;
	}
	return BasicSelectedObject<Handle, Api>(
		reinterpret_cast<Handle>(previous), detail::SelectedObjectRestorer<Handle, Api>{dc});
}

using SelectedBitmap = BasicSelectedObject<HBITMAP>;
using SelectedBrush = BasicSelectedObject<HBRUSH>;
using SelectedFont = BasicSelectedObject<HFONT>;
using SelectedPen = BasicSelectedObject<HPEN>;
using SelectedRegion = BasicSelectedObject<HRGN>;

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueRegistryKey = std::unique_ptr<void, detail::RegistryKeyCloser<Api>>;

using UniqueRegistryKey = BasicUniqueRegistryKey<>;

template <typename T, typename Api = detail::Win32ResourceApi>
using BasicUniqueLocalMemory = std::unique_ptr<T, detail::LocalMemoryDeleter<T, Api>>;

template <typename T>
using UniqueLocalMemory = BasicUniqueLocalMemory<T>;

template <typename T, typename Api = detail::Win32ResourceApi>
using BasicUniqueGlobalMemory = std::unique_ptr<T, detail::GlobalMemoryDeleter<T, Api>>;

template <typename T>
using UniqueGlobalMemory = BasicUniqueGlobalMemory<T>;

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueSid = std::unique_ptr<void, detail::SidDeleter<Api>>;

using UniqueSid = BasicUniqueSid<>;

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueEnvironmentBlock = std::unique_ptr<void, detail::EnvironmentBlockDeleter<Api>>;

using UniqueEnvironmentBlock = BasicUniqueEnvironmentBlock<>;

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueMappedView = std::unique_ptr<void, detail::MappedViewDeleter<Api>>;

using UniqueMappedView = BasicUniqueMappedView<>;

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueVirtualMemory = std::unique_ptr<void, detail::VirtualMemoryDeleter<Api>>;

using UniqueVirtualMemory = BasicUniqueVirtualMemory<>;

template <typename Api = detail::Win32ResourceApi>
using BasicUniqueRemoteVirtualMemory = std::unique_ptr<void, detail::RemoteVirtualMemoryDeleter<Api>>;

using UniqueRemoteVirtualMemory = BasicUniqueRemoteVirtualMemory<>;

template <typename Api = detail::Win32ResourceApi>
BasicUniqueRemoteVirtualMemory<Api> AdoptRemoteVirtualMemory(HANDLE process, void* value) noexcept
{
	return BasicUniqueRemoteVirtualMemory<Api>(
		value, detail::RemoteVirtualMemoryDeleter<Api>{process});
}

template <typename T, typename Api = detail::Win32ResourceApi>
using BasicUniqueHeapMemory = std::unique_ptr<T, detail::HeapMemoryDeleter<T, Api>>;

template <typename T>
BasicUniqueHeapMemory<T> AdoptHeapMemory(HANDLE heap, T* value) noexcept
{
	return BasicUniqueHeapMemory<T>(value, detail::HeapMemoryDeleter<T, detail::Win32ResourceApi>{heap});
}

template <typename T>
using UniqueMallocMemory = std::unique_ptr<T, detail::MallocDeleter<T>>;

using UniqueFile = std::unique_ptr<FILE, detail::FileCloser>;

// A locked page range is a lease, not an allocation owner.  Failure is kept on
// the object so callers that require non-pageable memory cannot accidentally
// treat a failed VirtualLock as success.
template <typename Api = detail::Win32ResourceApi>
class BasicPageLock
{
public:
	BasicPageLock() noexcept : address_(nullptr), size_(0), error_(ERROR_SUCCESS) {}

	static BasicPageLock TryLock(void* address, SIZE_T size) noexcept
	{
		BasicPageLock result;
		if (address == nullptr || size == 0) {
			result.error_ = ERROR_INVALID_PARAMETER;
			return result;
		}
		if (!Api::LockPages(address, size)) {
			result.error_ = Api::LastError();
			return result;
		}
		result.address_ = address;
		result.size_ = size;
		return result;
	}

	BasicPageLock(BasicPageLock&& other) noexcept
		: address_(other.address_), size_(other.size_), error_(other.error_)
	{
		other.address_ = nullptr;
		other.size_ = 0;
		other.error_ = ERROR_SUCCESS;
	}

	BasicPageLock& operator=(BasicPageLock&& other) noexcept
	{
		if (this != &other) {
			reset();
			address_ = other.address_;
			size_ = other.size_;
			error_ = other.error_;
			other.address_ = nullptr;
			other.size_ = 0;
			other.error_ = ERROR_SUCCESS;
		}
		return *this;
	}

	~BasicPageLock() noexcept { reset(); }

	BasicPageLock(const BasicPageLock&) = delete;
	BasicPageLock& operator=(const BasicPageLock&) = delete;

	void reset() noexcept
	{
		if (address_ != nullptr) {
			Api::UnlockPages(address_, size_);
			address_ = nullptr;
			size_ = 0;
		}
	}

	bool locked() const noexcept { return address_ != nullptr; }
	explicit operator bool() const noexcept { return locked(); }
	void* data() const noexcept { return address_; }
	SIZE_T size() const noexcept { return size_; }
	DWORD error() const noexcept { return error_; }

private:
	void* address_;
	SIZE_T size_;
	DWORD error_;
};

using PageLock = BasicPageLock<>;

// Some DLL resources must be initialized and torn down at an explicit runtime
// boundary.  This owner still provides a destructor backstop for partial init
// and early-return paths while allowing a deliberate reset before unload.
class CriticalSection
{
public:
	CriticalSection() noexcept : initialized_(false) {}
	~CriticalSection() noexcept { reset(); }

	CriticalSection(const CriticalSection&) = delete;
	CriticalSection& operator=(const CriticalSection&) = delete;
	CriticalSection(CriticalSection&&) = delete;
	CriticalSection& operator=(CriticalSection&&) = delete;

	void initialize()
	{
		if (!initialized_) {
			::InitializeCriticalSection(&value_);
			initialized_ = true;
		}
	}

	void reset() noexcept
	{
		if (initialized_) {
			::DeleteCriticalSection(&value_);
			initialized_ = false;
		}
	}

	CRITICAL_SECTION* get() noexcept { return &value_; }
	const CRITICAL_SECTION* get() const noexcept { return &value_; }
	bool initialized() const noexcept { return initialized_; }

private:
	CRITICAL_SECTION value_{};
	bool initialized_;
};

} // namespace renderer_raii

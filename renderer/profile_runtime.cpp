#if defined(_MSC_VER) && defined(_MSVC_LANG) && _MSVC_LANG >= 202002L
#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
#endif

#include "profile_runtime.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace renderer {
namespace {

struct AlgorithmProviderCloser
{
	void operator()(void* value) const noexcept
	{
		BCryptCloseAlgorithmProvider(
			static_cast<BCRYPT_ALG_HANDLE>(value), 0);
	}
};

struct HashCloser
{
	void operator()(void* value) const noexcept
	{
		BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(value));
	}
};

using UniqueAlgorithmProvider =
	std::unique_ptr<void, AlgorithmProviderCloser>;
using UniqueHash = std::unique_ptr<void, HashCloser>;

bool IsCompleteCandidate(const RendererPolicyCandidate& candidate) noexcept
{
	if (!candidate.valid ||
		!IsCanonicalProfileDigest(candidate.profileDigest) ||
		candidate.freeType.cacheMaxFaces <= 0 ||
		candidate.freeType.cacheMaxSizes < 0 ||
		candidate.freeType.cacheMaxBytes < 0 ||
		!std::isfinite(candidate.raster.gamma) || candidate.raster.gamma <= 0.0f ||
		!std::isfinite(candidate.directWrite.gamma) ||
		candidate.directWrite.gamma < 0.0f ||
		!std::isfinite(candidate.directWrite.contrast) ||
		candidate.directWrite.contrast <= 0.0f ||
		!std::isfinite(candidate.directWrite.clearTypeLevel) ||
		candidate.directWrite.clearTypeLevel < 0.0f ||
		candidate.directWrite.clearTypeLevel > 1.0f)
		return false;
	for (const FontIndividualPolicy& individual : candidate.individualFonts)
	{
		if (individual.family.empty())
			return false;
	}
	return true;
}

} // namespace

bool IsCanonicalProfileDigest(const std::string& digest) noexcept
{
	if (digest.size() != 71 || digest.compare(0, 7, "sha256:") != 0)
		return false;
	for (std::size_t index = 7; index < digest.size(); ++index)
	{
		const char value = digest[index];
		if (!((value >= '0' && value <= '9') ||
			(value >= 'a' && value <= 'f')))
			return false;
	}
	return true;
}

bool Sha256ProfileDigest(
	const unsigned char* bytes,
	std::size_t size,
	std::string& digest) noexcept
{
	digest.clear();
	if ((bytes == nullptr && size != 0) ||
		size > static_cast<std::size_t>(
			(std::numeric_limits<ULONG>::max)()))
		return false;
	try
	{
		BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
		NTSTATUS status = BCryptOpenAlgorithmProvider(
			&rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
		if (!BCRYPT_SUCCESS(status))
			return false;
		UniqueAlgorithmProvider algorithm(rawAlgorithm);

		DWORD objectSize = 0;
		DWORD copied = 0;
		status = BCryptGetProperty(
			algorithm.get(), BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &copied, 0);
		if (!BCRYPT_SUCCESS(status) || copied != sizeof(objectSize))
			return false;
		std::vector<unsigned char> hashObject(objectSize);
		BCRYPT_HASH_HANDLE rawHash = nullptr;
		status = BCryptCreateHash(
			algorithm.get(), &rawHash,
			hashObject.empty() ? nullptr : hashObject.data(), objectSize,
			nullptr, 0, 0);
		if (!BCRYPT_SUCCESS(status))
			return false;
		UniqueHash hash(rawHash);
		status = BCryptHashData(
			hash.get(), const_cast<PUCHAR>(bytes),
			static_cast<ULONG>(size), 0);
		if (!BCRYPT_SUCCESS(status))
			return false;

		std::array<unsigned char, 32> output{};
		status = BCryptFinishHash(
			hash.get(), output.data(), static_cast<ULONG>(output.size()), 0);
		if (!BCRYPT_SUCCESS(status))
			return false;

		constexpr char digits[] = "0123456789abcdef";
		digest.assign("sha256:");
		digest.reserve(71);
		for (unsigned char value : output)
		{
			digest.push_back(digits[value >> 4]);
			digest.push_back(digits[value & 0x0f]);
		}
		return true;
	}
	catch (...)
	{
		digest.clear();
		return false;
	}
}

ProfilePublication ProfileRuntime::Publish(
	RendererPolicyCandidate candidate) noexcept
{
	std::lock_guard<std::mutex> lock(publishMutex_);
	ProfilePublication publication;
	publication.generation = generation_;
	publication.revision = revision_;
	publication.snapshot = std::atomic_load_explicit(
		&current_, std::memory_order_acquire);
	if (!IsCompleteCandidate(candidate))
		return publication;

	try
	{
		const std::uint64_t generation = generation_ + 1;
		const std::uint64_t revision = revision_ + 1;
		std::shared_ptr<const font_substitution::Snapshot> substitutions =
			font_substitution::Snapshot::Build(
				std::move(candidate.substitutionRules), revision);
		RendererPolicyRef snapshot(new RendererPolicySnapshot(
			std::move(candidate), generation, revision, substitutions));

		font_substitution::ProcessRegistry().Publish(substitutions);
		std::atomic_store_explicit(
			&current_, snapshot, std::memory_order_release);
		generation_ = generation;
		revision_ = revision;

		publication.status = ProfilePublicationStatus::published;
		publication.generation = generation;
		publication.revision = revision;
		publication.snapshot = std::move(snapshot);
	}
	catch (...)
	{
	}
	return publication;
}

RendererPolicyRef ProfileRuntime::Load() const noexcept
{
	return std::atomic_load_explicit(&current_, std::memory_order_acquire);
}

bool ProfileRuntime::MatchesProfileDigest(
	const std::string& expected) const noexcept
{
	if (!IsCanonicalProfileDigest(expected))
		return false;
	RendererPolicyRef const current = Load();
	return current && current->profile_digest() == expected;
}

void ProfileRuntime::ClearForQuietUnload() noexcept
{
	std::lock_guard<std::mutex> lock(publishMutex_);
	std::atomic_store_explicit(
		&current_, RendererPolicyRef{}, std::memory_order_release);
	generation_ = 0;
	revision_ = 0;
}

ProfileRuntime& ProcessProfileRuntime()
{
	// Explicit renderer teardown owns live resources. Policy values contain no
	// handles, so retaining this tiny registry avoids shared_ptr destruction
	// while Windows holds the loader lock at process exit.
	static ProfileRuntime* runtime = new ProfileRuntime;
	return *runtime;
}

RendererPolicyRef CurrentRendererPolicy() noexcept
{
	return ProcessProfileRuntime().Load();
}

void ClearProcessProfileRuntimeForQuietUnload() noexcept
{
	ProcessProfileRuntime().ClearForQuietUnload();
}

} // namespace renderer

#pragma once

#include "renderer_policy.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace renderer {

bool IsCanonicalProfileDigest(const std::string& digest) noexcept;
bool Sha256ProfileDigest(
	const unsigned char* bytes,
	std::size_t size,
	std::string& digest) noexcept;

enum class ProfilePublicationStatus : unsigned char
{
	rejected,
	published,
};

struct ProfilePublication final
{
	ProfilePublicationStatus status = ProfilePublicationStatus::rejected;
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
	RendererPolicyRef snapshot;

	[[nodiscard]] bool published() const noexcept
	{
		return status == ProfilePublicationStatus::published;
	}
};

class ProfileRuntime final
{
public:
	ProfileRuntime() = default;
	ProfileRuntime(const ProfileRuntime&) = delete;
	ProfileRuntime& operator=(const ProfileRuntime&) = delete;

	[[nodiscard]] ProfilePublication Publish(
		RendererPolicyCandidate candidate) noexcept;
	[[nodiscard]] RendererPolicyRef Load() const noexcept;
	[[nodiscard]] bool MatchesProfileDigest(
		const std::string& expected) const noexcept;
	void ClearForQuietUnload() noexcept;

private:
	mutable std::mutex publishMutex_;
	RendererPolicyRef current_;
	std::uint64_t generation_ = 0;
	std::uint64_t revision_ = 0;
};

ProfileRuntime& ProcessProfileRuntime();
RendererPolicyRef CurrentRendererPolicy() noexcept;
void ClearProcessProfileRuntimeForQuietUnload() noexcept;

} // namespace renderer

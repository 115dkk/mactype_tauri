#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace renderer {
namespace font_substitution {

enum class ResolutionStatus : unsigned char
{
	noMatch,
	applied,
	cycle,
	depthExceeded,
};

struct Rule
{
	std::wstring sourceFamily;
	std::wstring replacementFamily;
	bool charsetSpecific;
	unsigned char charset;
	std::uint64_t id;

	Rule(
		std::wstring source = {},
		std::wstring replacement = {},
		bool specific = false,
		unsigned char ruleCharset = 1,
		std::uint64_t ruleId = 0)
		: sourceFamily(std::move(source)),
		  replacementFamily(std::move(replacement)),
		  charsetSpecific(specific), charset(ruleCharset), id(ruleId)
	{
	}
};

struct Request
{
	std::wstring family;
	unsigned char charset;

	Request(std::wstring requestedFamily = {}, unsigned char requestedCharset = 1)
		: family(std::move(requestedFamily)), charset(requestedCharset)
	{
	}
};

enum class BoldMode : unsigned char
{
	ignoreWeight = 0,
	synthetic = 1,
	sameFamily = 2,
	pairs = 3,
};

struct BoldPair
{
	std::wstring family;
	std::wstring boldFamily;
};

// What an adapter does with a bold-class request for a resolved replacement.
// sameFamilyFace falls back to keepWeight when no heavier face exists;
// pairedFamily falls back to dropWeight when the paired family has no face.
enum class BoldAction : unsigned char
{
	unchanged,
	dropWeight,
	keepWeight,
	sameFamilyFace,
	pairedFamily,
};

struct BoldPlan
{
	BoldAction action = BoldAction::unchanged;
	std::wstring pairedFamily;
};

// Parses one `Replacement=BoldFamily` profile line. Both sides are trimmed and
// lose a trailing `,<charset>` suffix; malformed and identity lines are
// rejected without affecting other lines.
bool ParseBoldPairLine(const std::wstring& line, BoldPair& pair);

bool BoldModeFromProfileValue(int value, BoldMode& mode) noexcept;

struct Resolution
{
	ResolutionStatus status = ResolutionStatus::noMatch;
	std::wstring family;
	bool matched = false;
	std::size_t hops = 0;
	std::uint64_t ruleId = 0;
	std::uint64_t generation = 0;
	std::uint64_t snapshotDigest = 0;
};

class Snapshot
{
public:
	static std::shared_ptr<const Snapshot> Build(
		std::vector<Rule> rules,
		std::uint64_t generation);
	static std::shared_ptr<const Snapshot> Build(
		std::vector<Rule> rules,
		BoldMode boldMode,
		std::vector<BoldPair> boldPairs,
		std::uint64_t generation);

	Resolution Resolve(const Request& request, std::size_t maxHops = 16) const;
	const std::vector<Rule>& rules() const noexcept { return rules_; }
	BoldMode bold_mode() const noexcept { return boldMode_; }
	const std::vector<BoldPair>& bold_pairs() const noexcept { return boldPairs_; }
	const BoldPair* FindBoldPair(const std::wstring& family) const noexcept;
	// resolvedFamily is Resolution::family without a vertical '@' prefix.
	BoldPlan PlanBold(const std::wstring& resolvedFamily, int requestedWeight) const;
	std::uint64_t generation() const noexcept { return generation_; }
	std::uint64_t digest() const noexcept { return digest_; }

private:
	Snapshot(
		std::vector<Rule> rules,
		BoldMode boldMode,
		std::vector<BoldPair> boldPairs,
		std::uint64_t generation,
		std::uint64_t digest)
		: rules_(std::move(rules)), boldPairs_(std::move(boldPairs)),
		  generation_(generation), digest_(digest), boldMode_(boldMode)
	{
	}

	const Rule* FindRule(const std::wstring& family, unsigned char charset) const noexcept;

	std::vector<Rule> rules_;
	std::vector<BoldPair> boldPairs_;
	std::uint64_t generation_;
	std::uint64_t digest_;
	BoldMode boldMode_;
};

class Registry
{
public:
	Registry();
	void Publish(std::shared_ptr<const Snapshot> snapshot) noexcept;
	std::shared_ptr<const Snapshot> Load() const noexcept;
	void ClearForQuietUnload() noexcept;

private:
	mutable std::mutex mutex_;
	std::shared_ptr<const Snapshot> snapshot_;
};

Registry& ProcessRegistry();
void ClearProcessRegistryForQuietUnload() noexcept;

} // namespace font_substitution
} // namespace renderer

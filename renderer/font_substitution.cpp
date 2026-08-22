#include "font_substitution.h"

#include <algorithm>

namespace renderer {
namespace font_substitution {
namespace {

wchar_t Fold(wchar_t value) noexcept
{
	return value >= L'A' && value <= L'Z' ? value + (L'a' - L'A') : value;
}

bool EqualFamily(const std::wstring& left, const std::wstring& right) noexcept
{
	if (left.size() != right.size())
		return false;
	for (std::size_t index = 0; index < left.size(); ++index)
	{
		if (Fold(left[index]) != Fold(right[index]))
			return false;
	}
	return true;
}

std::uint64_t HashByte(std::uint64_t hash, unsigned char value) noexcept
{
	return (hash ^ value) * 1099511628211ULL;
}

std::uint64_t HashFamily(std::uint64_t hash, const std::wstring& family) noexcept
{
	for (wchar_t value : family)
	{
		std::uint32_t const folded = static_cast<std::uint32_t>(Fold(value));
		for (unsigned int shift = 0; shift != 32; shift += 8)
			hash = HashByte(hash, static_cast<unsigned char>(folded >> shift));
	}
	return HashByte(hash, 0xff);
}

std::uint64_t StableRuleId(const Rule& rule) noexcept
{
	std::uint64_t hash = HashFamily(1469598103934665603ULL, rule.sourceFamily);
	hash = HashFamily(hash, rule.replacementFamily);
	hash = HashByte(hash, rule.charsetSpecific ? 1 : 0);
	hash = HashByte(hash, rule.charset);
	return hash == 0 ? 1 : hash;
}

std::uint64_t SnapshotDigest(const std::vector<Rule>& rules) noexcept
{
	std::uint64_t hash = 1469598103934665603ULL;
	for (const Rule& rule : rules)
	{
		for (unsigned int shift = 0; shift != 64; shift += 8)
			hash = HashByte(hash, static_cast<unsigned char>(rule.id >> shift));
		hash = HashByte(hash, 0xfe);
	}
	return hash == 0 ? 1 : hash;
}

bool SameRuleKey(const Rule& left, const Rule& right) noexcept
{
	return left.charsetSpecific == right.charsetSpecific &&
		(!left.charsetSpecific || left.charset == right.charset) &&
		EqualFamily(left.sourceFamily, right.sourceFamily);
}

bool ContainsFamily(
	const std::vector<std::wstring>& families,
	const std::wstring& family) noexcept
{
	return std::any_of(
		families.begin(), families.end(), [&](const std::wstring& candidate) {
			return EqualFamily(candidate, family);
		});
}

} // namespace

std::shared_ptr<const Snapshot> Snapshot::Build(
	std::vector<Rule> rules,
	std::uint64_t generation)
{
	std::vector<Rule> accepted;
	accepted.reserve(rules.size());
	for (Rule& rule : rules)
	{
		if (rule.sourceFamily.empty() || rule.replacementFamily.empty() ||
			EqualFamily(rule.sourceFamily, rule.replacementFamily))
			continue;
		if (std::any_of(
				accepted.begin(), accepted.end(), [&](const Rule& existing) {
					return SameRuleKey(existing, rule);
				}))
			continue;
		if (rule.id == 0)
			rule.id = StableRuleId(rule);
		accepted.push_back(std::move(rule));
	}
	std::uint64_t const digest = SnapshotDigest(accepted);
	return std::shared_ptr<const Snapshot>(
		new Snapshot(std::move(accepted), generation, digest));
}

const Rule* Snapshot::FindRule(
	const std::wstring& family,
	unsigned char charset) const noexcept
{
	const Rule* generic = nullptr;
	for (const Rule& rule : rules_)
	{
		if (!EqualFamily(rule.sourceFamily, family))
			continue;
		if (rule.charsetSpecific)
		{
			if (rule.charset == charset)
				return &rule;
		}
		else if (generic == nullptr)
		{
			generic = &rule;
		}
	}
	return generic;
}

Resolution Snapshot::Resolve(const Request& request, std::size_t maxHops) const
{
	Resolution result;
	result.family = request.family;
	result.generation = generation_;
	result.snapshotDigest = digest_;
	if (request.family.empty())
		return result;

	std::wstring current = request.family;
	std::vector<std::wstring> visited{current};
	for (std::size_t hop = 0; hop < maxHops; ++hop)
	{
		const Rule* rule = FindRule(current, request.charset);
		if (rule == nullptr)
		{
			if (result.hops != 0)
			{
				result.status = ResolutionStatus::applied;
				result.family = current;
				result.matched = true;
			}
			return result;
		}
		if (result.ruleId == 0)
			result.ruleId = rule->id;
		++result.hops;
		if (ContainsFamily(visited, rule->replacementFamily))
		{
			result.status = ResolutionStatus::cycle;
			result.family = request.family;
			result.matched = false;
			return result;
		}
		current = rule->replacementFamily;
		visited.push_back(current);
	}

	if (FindRule(current, request.charset) != nullptr)
	{
		result.status = ResolutionStatus::depthExceeded;
		result.family = request.family;
		result.matched = false;
		return result;
	}
	if (result.hops != 0)
	{
		result.status = ResolutionStatus::applied;
		result.family = current;
		result.matched = true;
	}
	return result;
}

Registry::Registry()
	: snapshot_(Snapshot::Build({}, 0))
{
}

void Registry::Publish(std::shared_ptr<const Snapshot> snapshot) noexcept
{
	if (snapshot)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		snapshot_ = std::move(snapshot);
	}
}

std::shared_ptr<const Snapshot> Registry::Load() const noexcept
{
	std::lock_guard<std::mutex> lock(mutex_);
	return snapshot_;
}

Registry& ProcessRegistry()
{
	static Registry* registry = new Registry;
	return *registry;
}

} // namespace font_substitution
} // namespace renderer

#include "font_substitution.h"

#include "bold_face_selection.h"

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

std::uint64_t BoldDigest(
	std::uint64_t hash,
	BoldMode boldMode,
	const std::vector<BoldPair>& pairs) noexcept
{
	hash = HashByte(hash, 0xfd);
	hash = HashByte(hash, static_cast<unsigned char>(boldMode));
	for (const BoldPair& pair : pairs)
	{
		hash = HashFamily(hash, pair.family);
		hash = HashFamily(hash, pair.boldFamily);
	}
	return hash == 0 ? 1 : hash;
}

bool IsPairSpace(wchar_t value) noexcept
{
	return value == L' ' || value == L'\t' || value == L'\r' ||
		value == L'\n' || value == 0x3000;
}

std::wstring Trim(const std::wstring& value)
{
	std::size_t first = 0;
	std::size_t last = value.size();
	while (first < last && IsPairSpace(value[first]))
		++first;
	while (last > first && IsPairSpace(value[last - 1]))
		--last;
	return value.substr(first, last - first);
}

std::wstring PairSide(const std::wstring& value)
{
	std::wstring side = Trim(value);
	std::size_t const comma = side.rfind(L',');
	if (comma != std::wstring::npos)
	{
		std::wstring const suffix = Trim(side.substr(comma + 1));
		bool digits = !suffix.empty();
		for (wchar_t character : suffix)
			digits = digits && character >= L'0' && character <= L'9';
		if (digits)
			side = Trim(side.substr(0, comma));
	}
	return side;
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

bool ParseBoldPairLine(const std::wstring& line, BoldPair& pair)
{
	pair = {};
	std::size_t const separator = line.find(L'=');
	if (separator == std::wstring::npos)
		return false;
	std::wstring family = PairSide(line.substr(0, separator));
	std::wstring boldFamily = PairSide(line.substr(separator + 1));
	if (family.empty() || boldFamily.empty() || EqualFamily(family, boldFamily))
		return false;
	pair.family = std::move(family);
	pair.boldFamily = std::move(boldFamily);
	return true;
}

bool BoldModeFromProfileValue(int value, BoldMode& mode) noexcept
{
	switch (value)
	{
	case 0:
		mode = BoldMode::ignoreWeight;
		return true;
	case 1:
		mode = BoldMode::synthetic;
		return true;
	case 2:
		mode = BoldMode::sameFamily;
		return true;
	case 3:
		mode = BoldMode::pairs;
		return true;
	default:
		mode = BoldMode::sameFamily;
		return false;
	}
}

std::shared_ptr<const Snapshot> Snapshot::Build(
	std::vector<Rule> rules,
	std::uint64_t generation)
{
	return Build(std::move(rules), BoldMode::sameFamily, {}, generation);
}

std::shared_ptr<const Snapshot> Snapshot::Build(
	std::vector<Rule> rules,
	BoldMode boldMode,
	std::vector<BoldPair> boldPairs,
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
	std::vector<BoldPair> acceptedPairs;
	acceptedPairs.reserve(boldPairs.size());
	for (BoldPair& pair : boldPairs)
	{
		if (pair.family.empty() || pair.boldFamily.empty() ||
			EqualFamily(pair.family, pair.boldFamily))
			continue;
		std::vector<BoldPair>::iterator const existing = std::find_if(
			acceptedPairs.begin(), acceptedPairs.end(),
			[&](const BoldPair& candidate) {
				return EqualFamily(candidate.family, pair.family);
			});
		if (existing != acceptedPairs.end())
			*existing = std::move(pair);
		else
			acceptedPairs.push_back(std::move(pair));
	}
	std::uint64_t const digest = BoldDigest(
		SnapshotDigest(accepted), boldMode, acceptedPairs);
	return std::shared_ptr<const Snapshot>(new Snapshot(
		std::move(accepted), boldMode, std::move(acceptedPairs),
		generation, digest));
}

const BoldPair* Snapshot::FindBoldPair(const std::wstring& family) const noexcept
{
	for (const BoldPair& pair : boldPairs_)
	{
		if (EqualFamily(pair.family, family))
			return &pair;
	}
	return nullptr;
}

BoldPlan Snapshot::PlanBold(
	const std::wstring& resolvedFamily,
	int requestedWeight) const
{
	BoldPlan plan;
	if (!bold_face_selection::IsBoldClassWeight(requestedWeight))
		return plan;
	switch (boldMode_)
	{
	case BoldMode::ignoreWeight:
		plan.action = BoldAction::dropWeight;
		break;
	case BoldMode::synthetic:
		plan.action = BoldAction::keepWeight;
		break;
	case BoldMode::sameFamily:
		plan.action = BoldAction::sameFamilyFace;
		break;
	case BoldMode::pairs:
	{
		const BoldPair* const pair = FindBoldPair(resolvedFamily);
		if (pair == nullptr)
		{
			plan.action = BoldAction::dropWeight;
			break;
		}
		plan.action = BoldAction::pairedFamily;
		plan.pairedFamily = pair->boldFamily;
		break;
	}
	}
	return plan;
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

void Registry::ClearForQuietUnload() noexcept
{
	std::lock_guard<std::mutex> lock(mutex_);
	snapshot_.reset();
}

Registry& ProcessRegistry()
{
	static Registry* registry = new Registry;
	return *registry;
}

void ClearProcessRegistryForQuietUnload() noexcept
{
	ProcessRegistry().ClearForQuietUnload();
}

} // namespace font_substitution
} // namespace renderer

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

	Resolution Resolve(const Request& request, std::size_t maxHops = 16) const;
	const std::vector<Rule>& rules() const noexcept { return rules_; }
	std::uint64_t generation() const noexcept { return generation_; }
	std::uint64_t digest() const noexcept { return digest_; }

private:
	Snapshot(
		std::vector<Rule> rules,
		std::uint64_t generation,
		std::uint64_t digest)
		: rules_(std::move(rules)), generation_(generation), digest_(digest)
	{
	}

	const Rule* FindRule(const std::wstring& family, unsigned char charset) const noexcept;

	std::vector<Rule> rules_;
	std::uint64_t generation_;
	std::uint64_t digest_;
};

class Registry
{
public:
	Registry();
	void Publish(std::shared_ptr<const Snapshot> snapshot) noexcept;
	std::shared_ptr<const Snapshot> Load() const noexcept;

private:
	mutable std::mutex mutex_;
	std::shared_ptr<const Snapshot> snapshot_;
};

Registry& ProcessRegistry();

} // namespace font_substitution
} // namespace renderer

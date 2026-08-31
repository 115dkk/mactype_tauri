#include "renderer_policy.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <cmath>
#include <cwctype>

const signed char CFontSettings::bounds_[MAX_FONT_SETTINGS][2] = {
	{0, 2},
	{-1, 6},
	{-64, 64},
	{-32, 32},
	{-32, 32},
	{0, 1},
};

int CFontSettings::GetParam(int index) const noexcept
{
	assert(index >= 0 && index < MAX_FONT_SETTINGS);
	return index >= 0 && index < MAX_FONT_SETTINGS ? settings_[index] : 0;
}

void CFontSettings::SetParam(int index, int value) noexcept
{
	assert(index >= 0 && index < MAX_FONT_SETTINGS);
	if (index < 0 || index >= MAX_FONT_SETTINGS)
		return;
	const int minimum = bounds_[index][0];
	const int maximum = bounds_[index][1];
	settings_[index] = value < minimum ? minimum :
		value > maximum ? maximum : value;
}

void CFontSettings::Clear() noexcept
{
	std::memset(settings_, 0, sizeof(settings_));
}

void CFontSettings::SetSettings(const int* values, int count) noexcept
{
	if (values == nullptr || count <= 0)
		return;
	const int bounded = count < MAX_FONT_SETTINGS ? count : MAX_FONT_SETTINGS;
	std::memcpy(settings_, values, static_cast<std::size_t>(bounded) * sizeof(int));
}

namespace renderer {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void HashBytes(std::uint64_t& digest, const void* data, std::size_t size) noexcept
{
	const unsigned char* bytes = static_cast<const unsigned char*>(data);
	for (std::size_t index = 0; index < size; ++index)
	{
		digest ^= bytes[index];
		digest *= kFnvPrime;
	}
}

template <typename Value>
void HashValue(std::uint64_t& digest, const Value& value) noexcept
{
	HashBytes(digest, &value, sizeof(value));
}

std::wstring CanonicalFamily(const wchar_t* family)
{
	if (family == nullptr)
		return {};
	if (*family == L'@')
		++family;
	std::wstring canonical(family);
	std::transform(
		canonical.begin(), canonical.end(), canonical.begin(),
		[](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
	return canonical;
}

std::uint64_t PolicyDigest(const RendererPolicySnapshot& snapshot) noexcept
{
	std::uint64_t digest = kFnvOffset;
	HashBytes(
		digest, snapshot.profile_digest().data(),
		snapshot.profile_digest().size());
	HashValue(digest, snapshot.hooks().childProcesses);
	HashValue(digest, snapshot.hooks().directWrite);
	HashValue(digest, snapshot.hooks().fontSubstitution);
	HashValue(digest, snapshot.hooks().skipPrivateFreeType);
	HashValue(digest, snapshot.hooks().unityFontMode);
	HashValue(digest, snapshot.hooks().unityFontEnabledForProcess);
	HashValue(digest, snapshot.free_type().cacheMaxFaces);
	HashValue(digest, snapshot.free_type().cacheMaxSizes);
	HashValue(digest, snapshot.free_type().cacheMaxBytes);
	const RasterPolicy& raster = snapshot.raster();
	HashValue(digest, raster.fontLoader);
	HashValue(digest, raster.fontLinkMode);
	HashValue(digest, raster.bitmapHeight);
	HashValue(digest, raster.bolderMode);
	HashValue(digest, raster.widthMode);
	HashValue(digest, raster.lcdFilter);
	HashValue(digest, raster.hintSmallFont);
	HashValue(digest, raster.harmonyLcd);
	HashValue(digest, raster.loadColorFont);
	HashValue(digest, raster.invertColor);
	HashValue(digest, raster.gamma);
	HashValue(digest, raster.renderWeight);
	HashValue(digest, raster.contrast);
	HashBytes(digest, raster.coverageTuning.data(), raster.coverageTuning.size());
	HashBytes(digest, raster.coverageTuningR.data(), raster.coverageTuningR.size());
	HashBytes(digest, raster.coverageTuningG.data(), raster.coverageTuningG.size());
	HashBytes(digest, raster.coverageTuningB.data(), raster.coverageTuningB.size());
	HashValue(digest, raster.shadowDarkColor);
	HashValue(digest, raster.shadowLightColor);
	const DirectWritePolicy& directWrite = snapshot.direct_write();
	HashValue(digest, directWrite.enabled);
	HashValue(digest, directWrite.gamma);
	HashValue(digest, directWrite.contrast);
	HashValue(digest, directWrite.clearTypeLevel);
	HashValue(digest, directWrite.renderingMode);
	HashValue(digest, directWrite.antiAliasMode);
	for (int index = 0; index < MAX_FONT_SETTINGS; ++index)
		HashValue(digest, snapshot.common_font_settings().GetParam(index));
	for (const FontIndividualPolicy& individual : snapshot.individual_fonts())
	{
		const wchar_t* family = individual.family.c_str();
		if (*family == L'@')
			++family;
		for (; *family != L'\0'; ++family)
		{
			const wchar_t canonical =
				static_cast<wchar_t>(std::towlower(*family));
			HashValue(digest, canonical);
		}
		for (int index = 0; index < MAX_FONT_SETTINGS; ++index)
			HashValue(digest, individual.settings.GetParam(index));
	}
	HashValue(digest, snapshot.substitutions_ready());
	if (snapshot.font_substitutions())
		HashValue(digest, snapshot.font_substitutions()->digest());
	return digest;
}

unsigned char CoverageValue(
	unsigned int input,
	const std::array<unsigned char, 256>& tuning,
	float renderWeight,
	float contrast) noexcept
{
	if (input >= tuning.size() || !std::isfinite(renderWeight) ||
		!std::isfinite(contrast) || renderWeight <= 0.0f || contrast <= 0.0f)
		return static_cast<unsigned char>((std::min)(input, 255u));
	double const tuned = static_cast<double>(tuning[input]) / 255.0;
	double const weighted = std::pow(tuned, 1.0 / renderWeight);
	double const adjusted = weighted < 0.5
		? std::pow(weighted * 2.0, contrast) / 2.0
		: 1.0 - std::pow((1.0 - weighted) * 2.0, contrast) / 2.0;
	double const bounded = (std::max)(0.0, (std::min)(1.0, adjusted));
	return static_cast<unsigned char>(bounded * 255.0 + 0.5);
}

UnityCoverageLut BuildUnityCoverageLut(
	const RendererPolicyCandidate& candidate) noexcept
{
	UnityCoverageLut result;
	const RasterPolicy& raster = candidate.raster;
	const bool bgr = candidate.commonFontSettings.GetAntiAliasMode() == 3 ||
		candidate.commonFontSettings.GetAntiAliasMode() == 5;
	const std::array<unsigned char, 256>* channels[3] = {
		bgr ? &raster.coverageTuningB : &raster.coverageTuningR,
		&raster.coverageTuningG,
		bgr ? &raster.coverageTuningR : &raster.coverageTuningB,
	};
	for (unsigned int value = 0; value < 256; ++value)
	{
		result.gray[value] = CoverageValue(
			value, raster.coverageTuning, raster.renderWeight, raster.contrast);
		for (unsigned int channel = 0; channel < 3; ++channel)
			result.rgb[channel][value] = CoverageValue(
				value, *channels[channel], raster.renderWeight, raster.contrast);
	}
	return result;
}

} // namespace

RendererPolicySnapshot::RendererPolicySnapshot(
	RendererPolicyCandidate candidate,
	std::uint64_t generation,
	std::uint64_t revision,
	std::shared_ptr<const font_substitution::Snapshot> substitutions)
	: generation_(generation), revision_(revision),
	  profileDigest_(std::move(candidate.profileDigest)), hooks_(candidate.hooks),
	  freeType_(candidate.freeType), raster_(candidate.raster),
	  directWrite_(candidate.directWrite),
	  unityCoverage_(BuildUnityCoverageLut(candidate)),
	  commonFontSettings_(candidate.commonFontSettings),
	  individualFonts_(std::move(candidate.individualFonts)),
	  substitutions_(std::move(substitutions)),
	  substitutionsReady_(candidate.substitutionsReady)
{
	raster_.generation = generation_;
	digest_ = PolicyDigest(*this);
}

CFontSettings RendererPolicySnapshot::font_settings_for(
	const wchar_t* family) const noexcept
{
	try
	{
		const std::wstring canonical = CanonicalFamily(family);
		for (const FontIndividualPolicy& individual : individualFonts_)
		{
			if (CanonicalFamily(individual.family.c_str()) == canonical)
				return individual.settings;
		}
	}
	catch (...)
	{
	}
	return commonFontSettings_;
}

} // namespace renderer

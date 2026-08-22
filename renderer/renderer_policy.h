#pragma once

#include "font_substitution.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifndef MAX_FONT_SETTINGS
#define MAX_FONT_SETTINGS 16
#endif

class CFontSettings
{
public:
	CFontSettings() noexcept { Clear(); }

	int GetHintingMode() const noexcept { return GetParam(0); }
	void SetHintingMode(int value) noexcept { SetParam(0, value); }
	int GetAntiAliasMode() const noexcept { return GetParam(1); }
	void SetAntiAliasMode(int value) noexcept { SetParam(1, value); }
	int GetNormalWeight() const noexcept { return GetParam(2); }
	void SetNormalWeight(int value) noexcept { SetParam(2, value); }
	int GetBoldWeight() const noexcept { return GetParam(3); }
	void SetBoldWeight(int value) noexcept { SetParam(3, value); }
	int GetItalicSlant() const noexcept { return GetParam(4); }
	void SetItalicSlant(int value) noexcept { SetParam(4, value); }
	int GetKerning() const noexcept { return GetParam(5); }
	void SetKerning(int value) noexcept { SetParam(5, value); }

	int GetParam(int index) const noexcept;
	void SetParam(int index, int value) noexcept;
	void Clear() noexcept;
	void SetSettings(const int* values, int count) noexcept;

private:
	int settings_[MAX_FONT_SETTINGS];
	static const signed char bounds_[MAX_FONT_SETTINGS][2];
};

namespace renderer {

struct HookPolicy final
{
	bool childProcesses = false;
	bool directWrite = false;
	bool fontSubstitution = false;
};

struct FreeTypeStartupPolicy final
{
	int cacheMaxFaces = 0;
	int cacheMaxSizes = 0;
	int cacheMaxBytes = 0;
};

struct RasterPolicy final
{
	std::uint64_t generation = 0;
	int fontLoader = 0;
	int fontLinkMode = 0;
	int bitmapHeight = 0;
	int bolderMode = 0;
	int widthMode = 0;
	int lcdFilter = 0;
	bool hintSmallFont = false;
	bool harmonyLcd = false;
	bool loadColorFont = false;
	bool invertColor = false;
	float gamma = 1.0f;
	std::uint32_t shadowDarkColor = 0;
	std::uint32_t shadowLightColor = 0;
};

struct DirectWritePolicy final
{
	bool enabled = false;
	float gamma = 0.0f;
	float contrast = 1.0f;
	float clearTypeLevel = 1.0f;
	int renderingMode = 0;
	int antiAliasMode = 0;
};

struct FontIndividualPolicy final
{
	std::wstring family;
	CFontSettings settings;
};

struct RendererPolicyCandidate final
{
	bool valid = false;
	std::string profileDigest;
	HookPolicy hooks;
	FreeTypeStartupPolicy freeType;
	RasterPolicy raster;
	DirectWritePolicy directWrite;
	CFontSettings commonFontSettings;
	std::vector<FontIndividualPolicy> individualFonts;
	std::vector<font_substitution::Rule> substitutionRules;
	bool substitutionsReady = false;
};

class ProfileRuntime;

class RendererPolicySnapshot final
{
public:
	[[nodiscard]] std::uint64_t generation() const noexcept
	{
		return generation_;
	}
	[[nodiscard]] std::uint64_t revision() const noexcept
	{
		return revision_;
	}
	[[nodiscard]] const std::string& profile_digest() const noexcept
	{
		return profileDigest_;
	}
	[[nodiscard]] std::uint64_t digest() const noexcept { return digest_; }
	[[nodiscard]] const HookPolicy& hooks() const noexcept { return hooks_; }
	[[nodiscard]] const FreeTypeStartupPolicy& free_type() const noexcept
	{
		return freeType_;
	}
	[[nodiscard]] const RasterPolicy& raster() const noexcept { return raster_; }
	[[nodiscard]] const DirectWritePolicy& direct_write() const noexcept
	{
		return directWrite_;
	}
	[[nodiscard]] bool substitutions_ready() const noexcept
	{
		return substitutionsReady_;
	}
	[[nodiscard]] const CFontSettings& common_font_settings() const noexcept
	{
		return commonFontSettings_;
	}
	[[nodiscard]] const std::vector<FontIndividualPolicy>&
	individual_fonts() const noexcept
	{
		return individualFonts_;
	}
	[[nodiscard]] const std::shared_ptr<const font_substitution::Snapshot>&
	font_substitutions() const noexcept
	{
		return substitutions_;
	}
	[[nodiscard]] CFontSettings font_settings_for(
		const wchar_t* family) const noexcept;

private:
	friend class ProfileRuntime;

	RendererPolicySnapshot(
		RendererPolicyCandidate candidate,
		std::uint64_t generation,
		std::uint64_t revision,
		std::shared_ptr<const font_substitution::Snapshot> substitutions);

	std::uint64_t generation_ = 0;
	std::uint64_t revision_ = 0;
	std::string profileDigest_;
	std::uint64_t digest_ = 0;
	HookPolicy hooks_;
	FreeTypeStartupPolicy freeType_;
	RasterPolicy raster_;
	DirectWritePolicy directWrite_;
	CFontSettings commonFontSettings_;
	std::vector<FontIndividualPolicy> individualFonts_;
	std::shared_ptr<const font_substitution::Snapshot> substitutions_;
	bool substitutionsReady_ = false;
};

using RendererPolicyRef = std::shared_ptr<const RendererPolicySnapshot>;

} // namespace renderer

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <dwrite_3.h>
#include <atlbase.h>
#include <atlcomcli.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace directwrite_virtual_font {

struct Identity
{
	std::wstring family;
	std::wstring subfamily;
	std::wstring fullName;
	std::wstring postScriptName;
};

// overrideWeight makes the alias advertise `weight` through OS/2
// usWeightClass, the OS/2 fsSelection BOLD bit and head.macStyle bold, so the
// alias family mirrors the source family's weight axis. A bold-class weight
// also names the subfamily Bold (or Bold Italic), so the alias has its own
// full and PostScript names. Without it the output bytes are exactly those of
// the plain alias.
struct AliasOptions
{
	bool overrideWeight = false;
	UINT16 weight = 400;
	std::uint64_t profileDigest = 0;
	std::vector<DWRITE_FONT_AXIS_VALUE> axisValues;
};

struct AliasedReference
{
	CComPtr<IDWriteFontFaceReference> reference;
	std::vector<DWRITE_FONT_AXIS_VALUE> axisValues;
	Identity identity;
};

// Produces one self-contained SFNT whose glyph and metric tables come from the
// replacement face while every OpenType naming record describes the alias.
// The returned reference is native DirectWrite state backed by an immutable,
// content-addressed local file. Standard DirectWrite file identity can therefore
// cross renderer-process boundaries without a MacType loader or proxy object.
[[nodiscard]] std::expected<AliasedReference, HRESULT> CreateAliasedReference(
	IDWriteFactory3* factory,
	IDWriteFontFaceReference* replacementReference,
	WCHAR const* aliasFamily,
	AliasOptions const& options = AliasOptions());

// The family name (ID 1) of a font whose unique identifier (ID 3) this module
// rewrote, or nothing for any other font; an alias of a replacement without an
// ID 3 record carries no marker and is reported as DirectWrite reports it. IDWriteGdiInterop::ConvertFontToLOGFONT reports
// an alias at a non-RIBBI weight as "<family> <subfamily>", a face GDI has
// never registered; the bare family is the name GDI substitution resolves to
// the same replacement. The Windows English record wins, then the first one.
[[nodiscard]] std::optional<std::wstring> AliasFamilyFromNameTable(
	std::vector<BYTE> const& nameTable);

// The SFNT rewrite behind CreateAliasedReference, exposed for tests.
HRESULT BuildAliasedSfnt(
	std::vector<BYTE> const& source,
	UINT32 faceIndex,
	std::wstring const& family,
	AliasOptions const& options,
	std::vector<BYTE>& output,
	Identity& identity);

} // namespace directwrite_virtual_font

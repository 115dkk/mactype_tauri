#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <dwrite_3.h>
#include <atlbase.h>
#include <atlcomcli.h>

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
// alias family mirrors the source family's weight axis. Without it the output
// bytes are exactly those of the plain alias.
struct AliasOptions
{
	bool overrideWeight = false;
	UINT16 weight = 400;
	bool addBoldSimulation = false;
};

// Produces one self-contained SFNT whose glyph and metric tables come from the
// replacement face while every OpenType naming record describes the alias.
// The returned reference is native DirectWrite state backed by an immutable,
// content-addressed local file. Standard DirectWrite file identity can therefore
// cross renderer-process boundaries without a MacType loader or proxy object.
HRESULT CreateAliasedReference(
	IDWriteFactory3* factory,
	IDWriteFontFaceReference* replacementReference,
	WCHAR const* aliasFamily,
	CComPtr<IDWriteFontFaceReference>& reference,
	Identity& identity,
	AliasOptions const& options = AliasOptions());

// The SFNT rewrite behind CreateAliasedReference, exposed for tests.
HRESULT BuildAliasedSfnt(
	std::vector<BYTE> const& source,
	UINT32 faceIndex,
	std::wstring const& family,
	AliasOptions const& options,
	std::vector<BYTE>& output,
	Identity& identity);

} // namespace directwrite_virtual_font

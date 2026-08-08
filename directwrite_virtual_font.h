#pragma once

#include "common.h"

#include <string>

namespace directwrite_virtual_font {

struct Identity
{
	std::wstring family;
	std::wstring subfamily;
	std::wstring fullName;
	std::wstring postScriptName;
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
	Identity& identity);

} // namespace directwrite_virtual_font

#include "settings.h"
#include "strtoken.h"
#include <math.h>	//pow
#include "supinfo.h"
#include "fteng.h"
#include "font_substitution.h"
#include "profile_runtime.h"
#include <stdlib.h>
#include <freetype/ftmodapi.h>
#ifdef INFINALITY
#include <freetype/ftenv.h>
#endif

CControlCenter* g_ControlCenter = nullptr;

namespace {

constexpr LONGLONG kMaximumProfileBytes = 4LL * 1024 * 1024;

bool SameFileIdentity(
	const BY_HANDLE_FILE_INFORMATION& left,
	const BY_HANDLE_FILE_INFORMATION& right) noexcept
{
	return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
		left.nFileIndexHigh == right.nFileIndexHigh &&
		left.nFileIndexLow == right.nFileIndexLow &&
		left.nFileSizeHigh == right.nFileSizeHigh &&
		left.nFileSizeLow == right.nFileSizeLow &&
		left.ftLastWriteTime.dwHighDateTime ==
			right.ftLastWriteTime.dwHighDateTime &&
		left.ftLastWriteTime.dwLowDateTime ==
			right.ftLastWriteTime.dwLowDateTime;
}

struct StableProfileDocument final
{
	renderer_raii::UniqueHandle file;
	BY_HANDLE_FILE_INFORMATION identity{};
	std::string digest;

	bool Unchanged() const noexcept
	{
		BY_HANDLE_FILE_INFORMATION current{};
		return file && GetFileInformationByHandle(file.get(), &current) &&
			SameFileIdentity(identity, current);
	}
};

bool LoadStableProfileDocument(
	LPCTSTR path,
	CParseIni& document,
	StableProfileDocument& stable)
{
	stable = {};
	if (path == nullptr || *path == _T('\0'))
		return false;
	stable.file = renderer_raii::AdoptHandle(CreateFile(
		path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
			FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr));
	if (!stable.file ||
		!GetFileInformationByHandle(stable.file.get(), &stable.identity) ||
		(stable.identity.dwFileAttributes &
			(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
		return false;

	ULARGE_INTEGER size{};
	size.HighPart = stable.identity.nFileSizeHigh;
	size.LowPart = stable.identity.nFileSizeLow;
	if (size.QuadPart == 0 || size.QuadPart > kMaximumProfileBytes)
		return false;
	std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
	size_t offset = 0;
	while (offset < bytes.size())
	{
		DWORD received = 0;
		const DWORD wanted = static_cast<DWORD>(bytes.size() - offset);
		if (!ReadFile(
				stable.file.get(), bytes.data() + offset, wanted, &received,
				nullptr) || received == 0)
			return false;
		offset += received;
	}
	if (!renderer::Sha256ProfileDigest(
			bytes.data(), bytes.size(), stable.digest))
		return false;

	document.Clear();
	return document.LoadFromFile(path) && stable.Unchanged();
}

bool BuildMainProfilePath(
	HINSTANCE module,
	TCHAR (&profilePath)[MAX_PATH]) noexcept
{
	TCHAR modulePath[MAX_PATH] = {};
	DWORD const length = GetModuleFileName(module, modulePath, MAX_PATH);
	if (length == 0 || length >= MAX_PATH || !PathRemoveFileSpec(modulePath))
		return false;
	return PathCombine(
		profilePath, modulePath, _T("MacType.ini")) != nullptr;
}

} // namespace

inline BOOL IsFolder(LPCTSTR pszPath) {
	return pszPath && *pszPath && *(pszPath + wcslen(pszPath) - 1) == '\\';
}

int _StrToInt(LPCTSTR pStr, int nDefault)
{
#define isspace(ch)		(ch == _T('\t') || ch == _T(' '))
#define isdigit(ch)		(static_cast<_TUCHAR>(ch - _T('0')) <= 9)

	int ret;
	bool neg = false;
	LPCTSTR pStart;

	for (; isspace(*pStr); pStr++);
	switch (*pStr) {
	case _T('-'):
		neg = true;
	case _T('+'):
		pStr++;
		break;
	}

	pStart = pStr;
	ret = 0;
	for (; isdigit(*pStr); pStr++) {
		ret = 10 * ret + (*pStr - _T('0'));
	}

	if (pStr == pStart) {
		return nDefault;
	}
	return neg ? -ret : ret;

#undef isspace
#undef isdigit
}

wstring LowerCase(wstring str) {
	transform(str.begin(), str.end(), str.begin(), ::tolower);
	return str;
}

// split a comma separated string into an int vector
vector<int> SplitString(LPCTSTR str) {
	CStringTokenizer token;
	vector<int> intList;
	int argc = 0;
	argc = token.Parse(str);

	for (int i = 0; i < 6; i++) {
		LPCTSTR arg = token.GetArgument(i);
		if (!arg)
			break;
		intList.push_back(_StrToInt(arg, 0));
	}
	return intList;
}

const wstring GetAppDir() {
	static wstring AppDir;
	if (AppDir.length()) {
		return AppDir;
	}
	WCHAR name[MAX_PATH] = { 0 };

	int nSize = GetModuleFileName(nullptr, name, MAX_PATH + 1);
	PathRemoveFileSpec(name);
	AppDir = wstring(name) + L"\\"; // path should always end with a "\"
	AppDir = LowerCase(AppDir);
	return AppDir;
}

CGdippSettings* CGdippSettings::s_pInstance;
CParseIni CGdippSettings::m_Config;
CHashedStringList FontNameCache;

static const TCHAR c_szGeneral[]  = _T("General");
static const TCHAR c_szFreeType[] = _T("FreeType");
static const TCHAR c_szDirectWrite[] = _T("DirectWrite");
#define HINTING_MIN			0
#define HINTING_MAX			2
#define AAMODE_MIN			-1
#define AAMODE_MAX			6
#define GAMMAVALUE_MIN		0.0625f
#define GAMMAVALUE_MAX		20.0f
#define CONTRAST_MIN		0.0625f
#define CONTRAST_MAX		10.0f
#define RENDERWEIGHT_MIN	0.0625f
#define RENDERWEIGHT_MAX	10.0f
#define NWEIGHT_MIN			-64
#define NWEIGHT_MAX			+64
#define BWEIGHT_MIN			-32
#define BWEIGHT_MAX			+32
#define SLANT_MIN			-32
#define SLANT_MAX			+32

CGdippSettings* CGdippSettings::CreateInstance()
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_SETTING);
	CGdippSettings* pSettings = new CGdippSettings;
	CGdippSettings* pOldSettings = reinterpret_cast<CGdippSettings*>(InterlockedExchangePointer(reinterpret_cast<void**>(&s_pInstance), pSettings));
	_ASSERTE(pOldSettings == nullptr);
	int nSize = GetModuleFileName(nullptr, pSettings->m_szexeName, MAX_PATH);
	for (int i = nSize; i > 0; --i) {
		if (pSettings->m_szexeName[i] == _T('\\')) {
			StringCchCopy(pSettings->m_szexeName, nSize - i, pSettings->m_szexeName + i + 1);
			break;
		}
	}
	return pSettings;
}

void CGdippSettings::DestroyInstance()
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_SETTING);

	CGdippSettings* pSettings = reinterpret_cast<CGdippSettings*>(InterlockedExchangePointer(reinterpret_cast<void**>(&s_pInstance), nullptr));
	if (pSettings) {
		delete pSettings;
	}
}

CGdippSettings* CGdippSettings::GetInstance()
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_SETTING);
	CGdippSettings* pSettings = s_pInstance;
	_ASSERTE(pSettings != nullptr);

	if (!pSettings->m_bDelayedInit) {
		pSettings->DelayedInit();
	}
	return pSettings;
}

const CGdippSettings* CGdippSettings::GetInstanceNoInit()
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_SETTING);
	CGdippSettings* pSettings = s_pInstance;
	_ASSERTE(pSettings != nullptr);
	return pSettings;
}

void CGdippSettings::DelayedInit()
{
	if (!g_pFTEngine) {
		return;
	}
	if (IsBadCodePtr(reinterpret_cast<FARPROC>(RegOpenKeyExW)) || *reinterpret_cast<const DWORD_PTR*>(RegOpenKeyExW) == 0)
		return;
	m_bDelayedInit = true;

	// Font-substitution rules are configuration data, not screen-DC state.
	// Restricted renderer processes can deny GetDC while still supporting
	// DirectWrite, so retain their exact configured family names before any
	// GDI-dependent tuning is attempted.
	CFontSubstitutesIniArray arrFontSubstitutes;
	wstring names = _T("FontSubstitutes@") + wstring(m_szexeName);
	if (_IsFreeTypeProfileSectionExists(names.c_str(), m_szFileName))
		AddListFromSection(names.c_str(), m_szFileName, arrFontSubstitutes);
	else
		AddListFromSection(_T("FontSubstitutes"), m_szFileName, arrFontSubstitutes);
	m_FontSubstitutesInfo.init(m_nFontSubstitutes, arrFontSubstitutes);
	if (!PublishRendererPolicySnapshot(true))
	{
		m_bDelayedInit = false;
		return;
	}

	auto hdcScreen = renderer_raii::AdoptWindowDeviceContext(nullptr, GetDC(nullptr));
	if (!hdcScreen) {
		return;
	}

	m_nScreenDpi = GetDeviceCaps(hdcScreen.get(), LOGPIXELSX);

	const int nTextTuning = _GetFreeTypeProfileInt(_T("TextTuning"), 0, nullptr),
		nTextTuningR = _GetFreeTypeProfileInt(_T("TextTuningR"), 0, nullptr),
		nTextTuningG = _GetFreeTypeProfileInt(_T("TextTuningG"), 0, nullptr),
		nTextTuningB = _GetFreeTypeProfileInt(_T("TextTuningB"), 0, nullptr);
	InitInitTuneTable();
	InitTuneTable(nTextTuning,  m_nTuneTable);
	InitTuneTable(nTextTuningR, m_nTuneTableR);
	InitTuneTable(nTextTuningG, m_nTuneTableG);
	InitTuneTable(nTextTuningB, m_nTuneTableB);
	RefreshAlphaTable();

	names = _T("Individual@") + wstring(m_szexeName);
	if (_IsFreeTypeProfileSectionExists(names.c_str(), nullptr))
		AddIndividualFromSection(names.c_str(), nullptr, m_arrIndividual);
	else
		AddIndividualFromSection(_T("Individual"), nullptr, m_arrIndividual);

	AddExcludeListFromSection(_T("Exclude"), nullptr, m_arrExcludeFont);
	// Both lists share parsing; inclusion semantics are applied during lookup.
	AddExcludeListFromSection(_T("Include"), nullptr, m_arrIncludeFont);

	if (FontLink()) {
		m_fontlinkinfo.init();
	}

	FT_LCDMode_Set(freetype_library, this->HarmonyLCD() ? 1 : 0);

	if (this->HarmonyLCD()) {
		FT_Library_SetLcdFilter(nullptr, FT_LCD_FILTER_NONE);
		// Harmony LCD rendering
		if (m_bUseCustomPixelLayout) {
			FT_Vector  sub[3] = { { m_arrPixelLayout[0], m_arrPixelLayout[1]},
									{m_arrPixelLayout[2], m_arrPixelLayout[3]},
									{m_arrPixelLayout[4], m_arrPixelLayout[5]}};	// custom layout
			FT_Library_SetLcdGeometry(freetype_library, sub);
		}
		else {
			switch (this->m_FontSettings.GetAntiAliasMode()) {
			case 0:
			case 1: {
				FT_Vector  sub[3] = { { 0, 0 }, { 0, 0 },	 { 0, 0 } };	// gray scale
				FT_Library_SetLcdGeometry(freetype_library, sub);
				break;
			}
			case 2: //RGB
			case 4: {
				FT_Vector  sub[3] = { { -21, 0 }, { 0, 0 },	 { 21, 0 } };
				FT_Library_SetLcdGeometry(freetype_library, sub);
				break;
			}
			case 3:	//BGR
			case 5: {
				FT_Vector  sub[3] = { { 21, 0 }, { 0, 0 },	 { -21, 0 } };
				FT_Library_SetLcdGeometry(freetype_library, sub);
				break;
			}
			case 6: {
				//Pentile
				FT_Vector  sub[3] = { {-11, 16}, {-11, -16}, {22, 0} };
				FT_Library_SetLcdGeometry(freetype_library, sub);
				break;
			}
			}
			if (m_FontSettings.GetAntiAliasMode() > 2)
				m_FontSettings.SetAntiAliasMode(2);	// all non-grayscale panel should use DrawLCD routine as its output.
		}
	}
	else {
		int nLcdFilter = LcdFilter();
	if (static_cast<int>(FT_LCD_FILTER_NONE) <= nLcdFilter && nLcdFilter < static_cast<int>(FT_LCD_FILTER_MAX)) {
			switch (GetFontSettings().GetAntiAliasMode()) {
			case 1:
			case 4:
			case 5:
				nLcdFilter = FT_LCD_FILTER_LIGHT;	// AA mode selects the light filter unless the profile defines one.
			}
			FT_Library_SetLcdFilter(freetype_library, static_cast<FT_LcdFilter>(nLcdFilter));
			if (UseCustomLcdFilter())
			{
				unsigned char buff[5];
				memcpy(buff, LcdFilterWeights(), sizeof(buff));
				FT_Library_SetLcdFilterWeights(freetype_library, buff);
			}
		}
	}

	PublishRendererPolicySnapshot(true);
}

bool CGdippSettings::PublishRendererPolicySnapshot(
	bool substitutionsReady) const
{
	renderer::RendererPolicyCandidate candidate;
	candidate.valid = true;
	candidate.profileDigest = m_profileDigest;
	candidate.hooks.childProcesses = m_bHookChildProcesses;
	candidate.hooks.directWrite = m_bDirectWrite != FALSE;
	candidate.hooks.fontSubstitution = m_nFontSubstitutes != 0;
	candidate.freeType.cacheMaxFaces = m_nCacheMaxFaces;
	candidate.freeType.cacheMaxSizes = m_nCacheMaxSizes;
	candidate.freeType.cacheMaxBytes = m_nCacheMaxBytes;
	candidate.raster.fontLoader = m_nFontLoader;
	candidate.raster.fontLinkMode = m_bFontLink;
	candidate.raster.bitmapHeight = m_nBitmapHeight;
	candidate.raster.bolderMode = m_nBolderMode;
	candidate.raster.widthMode = m_nWidthMode;
	candidate.raster.lcdFilter = m_nLcdFilter;
	candidate.raster.hintSmallFont = m_bHintSmallFont != FALSE;
	candidate.raster.harmonyLcd = HarmonyLCD();
	candidate.raster.loadColorFont = m_bColorFont;
	candidate.raster.invertColor = m_bInvertColor;
	candidate.raster.gamma = m_fGammaValue;
	candidate.raster.shadowDarkColor = m_nShadowDarkColor;
	candidate.raster.shadowLightColor = m_nShadowLightColor;
	candidate.directWrite.enabled = m_bDirectWrite != FALSE;
	candidate.directWrite.gamma = m_fGammaValueForDW;
	candidate.directWrite.contrast = m_fContrastForDW;
	candidate.directWrite.clearTypeLevel = m_fClearTypeLevelForDW;
	candidate.directWrite.renderingMode = m_nRenderingModeForDW;
	candidate.directWrite.antiAliasMode = m_nAntiAliasModeForDW;
	candidate.commonFontSettings = m_FontSettings;

	const CFontIndividual* individual = m_arrIndividual.Begin();
	const CFontIndividual* const individualEnd = m_arrIndividual.End();
	for (; individual != individualEnd; ++individual)
	{
		candidate.individualFonts.push_back({
			individual->GetName(), individual->GetIndividual()});
	}

	candidate.substitutionsReady = substitutionsReady;
	if (substitutionsReady)
	{
		candidate.substitutionRules.reserve(
			static_cast<size_t>(m_FontSubstitutesInfo.GetSize()));
		for (int index = 0; index < m_FontSubstitutesInfo.GetSize(); ++index)
		{
			LOGFONT source = {};
			LOGFONT replacement = {};
			bool charsetSpecific = false;
			if (!m_FontSubstitutesInfo.CopyRule(
					index, source, replacement, charsetSpecific))
				continue;
			candidate.substitutionRules.emplace_back(
				source.lfFaceName,
				replacement.lfFaceName,
				charsetSpecific,
				source.lfCharSet,
				0);
		}
	}
	return renderer::ProcessProfileRuntime().Publish(
		std::move(candidate)).published();
}

bool CGdippSettings::LoadSettings(HINSTANCE hModule)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_SETTING);
	if (!BuildMainProfilePath(hModule, m_szFileName)) {
		return false;
	}

	return LoadAppSettings(m_szFileName);
}

int CGdippSettings::_GetFreeTypeProfileIntFromSection(LPCTSTR lpszSection, LPCTSTR lpszKey, int nDefault, LPCTSTR lpszFile)
{
	wstring names = wstring(lpszSection) + _T("@") + wstring(m_szexeName);
	if (m_Config.IsPartExists(names.c_str()) && m_Config[names.c_str()].IsValueExists(lpszKey))
		return m_Config[names.c_str()][lpszKey].ToInt();
	else
	if (m_Config[lpszSection].IsValueExists(lpszKey))
		return m_Config[lpszSection][lpszKey].ToInt();
	else
		return nDefault;
}

bool CGdippSettings::_GetFreeTypeProfileBoolFromSection(LPCTSTR lpszSection, LPCTSTR lpszKey, bool nDefault, LPCTSTR lpszFile)
{
	wstring names = wstring(lpszSection) + _T("@") + wstring(m_szexeName);
	if (m_Config.IsPartExists(names.c_str()) && m_Config[names.c_str()].IsValueExists(lpszKey))
		return m_Config[names.c_str()][lpszKey].ToBool();
	else
	if (m_Config[lpszSection].IsValueExists(lpszKey))
		return m_Config[lpszSection][lpszKey].ToBool();
	else
		return nDefault;
}

wstring CGdippSettings::_GetFreeTypeProfileStrFromSection(LPCTSTR lpszSection, LPCTSTR lpszKey, const TCHAR* nDefault, LPCTSTR lpszFile)
{
	wstring names = wstring(lpszSection) + _T("@") + wstring(m_szexeName);
	if (m_Config.IsPartExists(names.c_str()) && m_Config[names.c_str()].IsValueExists(lpszKey))
		return m_Config[names.c_str()][lpszKey].ToString();
	else
	if (m_Config[lpszSection].IsValueExists(lpszKey))
		return m_Config[lpszSection][lpszKey].ToString();
	else
		return nDefault;
}

int CGdippSettings::_GetFreeTypeProfileInt(LPCTSTR lpszKey, int nDefault, LPCTSTR lpszFile)
{
	int ret = _GetFreeTypeProfileIntFromSection(c_szFreeType, lpszKey, nDefault, lpszFile);
	if (ret == nDefault)
		return _GetFreeTypeProfileIntFromSection(c_szGeneral, lpszKey, nDefault, lpszFile);
	else
		return ret;
}

int CGdippSettings::_GetFreeTypeProfileBoundInt(LPCTSTR lpszKey, int nDefault, int nMin, int nMax, LPCTSTR lpszFile)
{
	const int ret = _GetFreeTypeProfileInt(lpszKey, nDefault, lpszFile);
	return Bound(ret, nMin, nMax);
}

bool CGdippSettings::_IsFreeTypeProfileSectionExists(LPCTSTR lpszKey, LPCTSTR lpszFile)
{
	return m_Config.IsPartExists(lpszKey);
}

float CGdippSettings::FastGetProfileFloat(LPCTSTR lpszSection, LPCTSTR lpszKey, float fDefault)
{
	wstring names = wstring(lpszSection) + _T("@") + wstring(m_szexeName);
	if (m_Config.IsPartExists(names.c_str()) && m_Config[names.c_str()].IsValueExists(lpszKey))
		return m_Config[names.c_str()][lpszKey].ToDouble();
	else
	if (m_Config[lpszSection].IsValueExists(lpszKey))
		return m_Config[lpszSection][lpszKey].ToDouble();
	else
		return fDefault;
}

int CGdippSettings::FastGetProfileInt(LPCTSTR lpszSection, LPCTSTR lpszKey, int nDefault)
{
	wstring names = wstring(lpszSection) + _T("@") + wstring(m_szexeName);
	if (m_Config.IsPartExists(names.c_str()) && m_Config[names.c_str()].IsValueExists(lpszKey))
		return m_Config[names.c_str()][lpszKey].ToInt();
	else
	if (m_Config[lpszSection].IsValueExists(lpszKey))
		return m_Config[lpszSection][lpszKey].ToInt();
	else
		return nDefault;
}

float CGdippSettings::_GetFreeTypeProfileFloat(LPCTSTR lpszKey, float fDefault, LPCTSTR lpszFile)
{
	wstring names = wstring(c_szFreeType) + _T("@") + wstring(m_szexeName);
	if (m_Config.IsPartExists(names.c_str()) && m_Config[names.c_str()].IsValueExists(lpszKey))
		return m_Config[names.c_str()][lpszKey].ToInt();
	else
	{
		names = wstring(c_szGeneral) + _T("@") + wstring(m_szexeName);
		if (m_Config.IsPartExists(names.c_str()) && m_Config[names.c_str()].IsValueExists(lpszKey))
			return m_Config[names.c_str()][lpszKey].ToDouble();
		else
		if (m_Config[c_szFreeType].IsValueExists(lpszKey))
			return m_Config[c_szFreeType][lpszKey].ToDouble();
		if (m_Config[c_szGeneral].IsValueExists(lpszKey))
			return m_Config[c_szGeneral][lpszKey].ToDouble();
		else
			return fDefault;
	}
}

float CGdippSettings::_GetFreeTypeProfileBoundFloat(LPCTSTR lpszKey, float fDefault, float fMin, float fMax, LPCTSTR lpszFile)
{
	const float ret = _GetFreeTypeProfileFloat(lpszKey, fDefault, lpszFile);
	return Bound(ret, fMin, fMax);
}

DWORD CGdippSettings::FastGetProfileString(LPCTSTR lpszSection, LPCTSTR lpszKey, LPCTSTR lpszDefault, LPTSTR lpszRet, DWORD cch)
{
	wstring names = wstring(lpszSection) + _T("@") + wstring(m_szexeName);
	if (m_Config.IsPartExists(names.c_str()) && m_Config[names.c_str()].IsValueExists(lpszKey))
	{
		LPCTSTR p = m_Config[names.c_str()][lpszKey];
		StringCchCopy(lpszRet, cch, p);
		return wcslen(p);
	}
	else
	if (m_Config[lpszSection].IsValueExists(lpszKey))
	{
		LPCTSTR p = m_Config[lpszSection][lpszKey];
		StringCchCopy(lpszRet, cch, p);
		return wcslen(p);
	}
	else
	{
		if (lpszDefault) {
			StringCchCopy(lpszRet, cch, lpszDefault);
			return wcslen(lpszDefault);
		}
		else {
			if (lpszRet && cch) lpszRet[0] = _T('\0');
			return 0;
		}
	}
}


DWORD CGdippSettings::_GetFreeTypeProfileString(LPCTSTR lpszKey, LPCTSTR lpszDefault, LPTSTR lpszRet, DWORD cch, LPCTSTR lpszFile)
{
	wstring names = wstring(c_szFreeType) + _T("@") + wstring(m_szexeName);
	if (m_Config.IsPartExists(names.c_str()) && m_Config[names.c_str()].IsValueExists(lpszKey))
	{
		LPCTSTR p = m_Config[names.c_str()][lpszKey];
		StringCchCopy(lpszRet, cch, p);
		return wcslen(p);
	}
	else
	{
		names = wstring(c_szGeneral) + _T("@") + wstring(m_szexeName);
		if (m_Config.IsPartExists(names.c_str()) && m_Config[names.c_str()].IsValueExists(lpszKey))
		{
			LPCTSTR p = m_Config[names.c_str()][lpszKey];
			StringCchCopy(lpszRet, cch, p);
			return wcslen(p);
		}
		else
		if (m_Config[c_szFreeType].IsValueExists(lpszKey))
		{
			LPCTSTR p = m_Config[c_szFreeType][lpszKey];
			StringCchCopy(lpszRet, cch, p);
			return wcslen(p);
		}
		else
		if (m_Config[c_szGeneral].IsValueExists(lpszKey))
		{
			LPCTSTR p = m_Config[c_szGeneral][lpszKey];
			StringCchCopy(lpszRet, cch, p);
			return wcslen(p);
		}
		else
		{
			StringCchCopy(lpszRet, cch, lpszDefault);
			return wcslen(lpszDefault);
		}
	}
}

bool CGdippSettings::LoadAppSettings(LPCTSTR lpszFile)
{
	// 各種設定読み込み
	// INIファイルの例:
	// [General]
	// HookChildProcesses=0
	// HintingMode=0
	// AntiAliasMode=0
	// NormalWeight=0
	// BoldWeight=0
	// ItalicSlant=0
	// EnableKerning=0
	// MaxHeight=0
	// ForceChangeFont=ＭＳ Ｐゴシック
	// TextTuning=0
	// TextTuningR=0
	// TextTuningG=0
	// TextTuningB=0
	// CacheMaxFaces=0
	// CacheMaxSizes=0
	// CacheMaxBytes=0
	// AlternativeFile=
	// LoadOnDemand=0
	// UseMapping=0
	// LcdFilter=0
	// Shadow=1,1,4
	// [Individual]
	// ＭＳ Ｐゴシック=0,1,2,3,4,5
	WritePrivateProfileString(nullptr, nullptr, nullptr, lpszFile);
	StableProfileDocument mainProfile;
	if (!LoadStableProfileDocument(lpszFile, m_Config, mainProfile) ||
		!m_Config.IsPartExists(c_szGeneral))
	{
		m_Config.Clear();
		return false;
	}
	StableProfileDocument alternativeProfile;
	StableProfileDocument* selectedProfile = &mainProfile;

	TCHAR szAlternative[MAX_PATH];
	if (FastGetProfileString(c_szGeneral, _T("AlternativeFile"), _T(""), szAlternative, MAX_PATH)) {
		if (PathIsRelative(szAlternative)) {
			TCHAR szDir[MAX_PATH];
			if (FAILED(StringCchCopy(szDir, MAX_PATH, lpszFile)) ||
				!PathRemoveFileSpec(szDir) ||
				PathCombine(szAlternative, szDir, szAlternative) == nullptr)
				return false;
		}
		if (FAILED(StringCchCopy(m_szFileName, MAX_PATH, szAlternative)))
			return false;
		lpszFile = m_szFileName;
		WritePrivateProfileString(nullptr, nullptr, nullptr, lpszFile);
		if (!LoadStableProfileDocument(
				lpszFile, m_Config, alternativeProfile) ||
			!m_Config.IsPartExists(c_szGeneral))
		{
			m_Config.Clear();
			return false;
		}
		selectedProfile = &alternativeProfile;
	}

	_GetAlternativeProfileName(m_szexeName, lpszFile);
	CFontSettings& fs = m_FontSettings;
	fs.Clear();
	fs.SetHintingMode(_GetFreeTypeProfileBoundInt(_T("HintingMode"), 0, HINTING_MIN, HINTING_MAX, lpszFile));
	fs.SetAntiAliasMode(_GetFreeTypeProfileBoundInt(_T("AntiAliasMode"), 0, AAMODE_MIN, AAMODE_MAX, lpszFile));
	fs.SetNormalWeight(_GetFreeTypeProfileBoundInt(_T("NormalWeight"), 0, NWEIGHT_MIN, NWEIGHT_MAX, lpszFile));
	fs.SetBoldWeight(_GetFreeTypeProfileBoundInt(_T("BoldWeight"), 0, BWEIGHT_MIN, BWEIGHT_MAX, lpszFile));
	fs.SetItalicSlant(_GetFreeTypeProfileBoundInt(_T("ItalicSlant"), 0, SLANT_MIN, SLANT_MAX, lpszFile));
	fs.SetKerning(!!_GetFreeTypeProfileInt(_T("EnableKerning"), 0, lpszFile));
	m_nAntiAliasModeForDW = fs.GetAntiAliasMode();	// DirectWrite always use the user defined AA mode.
	{
		TCHAR szShadow[256];
		CStringTokenizer token;
		m_bEnableShadow = false;
		if (!_GetFreeTypeProfileString(_T("Shadow"), _T(""), szShadow, countof(szShadow), lpszFile)
				|| token.Parse(szShadow) < 3) {
			goto SKIP;
		}
		for (int i=0; i<3; i++) {
			m_nShadow[i] = _StrToInt(token.GetArgument(i), 0);
		}
		m_bEnableShadow = true;
		if (token.GetCount()>=4)	//如果指定了浅色阴影
			m_nShadowDarkColor = _httoi(token.GetArgument(3));	//读取阴影
		else
			m_nShadowDarkColor = 0;	//否则为黑色
		if (token.GetCount()>=6)	//如果指定了甥瀚阴影
		{
			m_nShadowLightColor = _httoi(token.GetArgument(5));	//读取阴影
			m_nShadow[3] = _StrToInt(token.GetArgument(4), m_nShadow[2]); //读取甥胰
		}
		else
		{
			m_nShadowLightColor = m_nShadowDarkColor;		//否则和浅色阴影相同
			m_nShadow[3] = m_nShadow[2];		//甥胰也相同
		}
SKIP:
		;
	}

	m_bHookChildProcesses = !!_GetFreeTypeProfileInt(_T("HookChildProcesses"), false, lpszFile);
	m_bUseMapping	= !!_GetFreeTypeProfileInt(_T("UseMapping"), false, lpszFile);
	m_nBolderMode	= _GetFreeTypeProfileInt(_T("BolderMode"), 0, lpszFile);
	m_nGammaMode	= _GetFreeTypeProfileInt(_T("GammaMode"), -1, lpszFile);
	m_fGammaValue	= _GetFreeTypeProfileBoundFloat(_T("GammaValue"), 1.0f, GAMMAVALUE_MIN, GAMMAVALUE_MAX, lpszFile);
	m_fRenderWeight	= _GetFreeTypeProfileBoundFloat(_T("RenderWeight"), 1.0f, RENDERWEIGHT_MIN, RENDERWEIGHT_MAX, lpszFile);
	m_fContrast		= _GetFreeTypeProfileBoundFloat(_T("Contrast"), 1.0f, CONTRAST_MIN, CONTRAST_MAX, lpszFile);

//DirectWrite/Direct2D exclusive settings
	float fCalculatedDWGamma = m_fGammaValue*m_fGammaValue > 1.3 ? m_fGammaValue * m_fGammaValue / 2 : 0.7f;
	// if not set, use calculated gamma as DW gamma
	m_fGammaValueForDW = Bound(FastGetProfileFloat(c_szDirectWrite, _T("GammaValue"), fCalculatedDWGamma), 0.0f, GAMMAVALUE_MAX);
	m_fContrastForDW = Bound(FastGetProfileFloat(c_szDirectWrite, _T("Contrast"), 1.0f), CONTRAST_MIN, CONTRAST_MAX);
	m_nRenderingModeForDW = Bound(FastGetProfileInt(c_szDirectWrite, _T("RenderingMode"), 5), 0, 6);
	m_fClearTypeLevelForDW = Bound(FastGetProfileFloat(c_szDirectWrite, _T("ClearTypeLevel"), 1.0f), 0.0f, 1.0f);

	m_bLoadOnDemand	= !!_GetFreeTypeProfileInt(_T("LoadOnDemand"), false, lpszFile);
	m_bFontLink		= _GetFreeTypeProfileInt(_T("FontLink"), 0, lpszFile);

	m_bIsInclude	= !!_GetFreeTypeProfileInt(_T("UseInclude"), false, lpszFile);
	m_nMaxHeight	= _GetFreeTypeProfileBoundInt(_T("MaxHeight"), 0, 0, 0xfff, lpszFile);	//赃只能到65535，cache的限制，而且大字体无实际价值
	m_nMinHeight = _GetFreeTypeProfileBoundInt(_T("MinHeight"), 0, 0,
				(m_nMaxHeight) ? m_nMaxHeight : 0xfff,  // shouldn't be greater than MaxHeight unless it is undefined
				lpszFile);	//Minimum size of rendered font. DPI aware alternative.
				//patched by krrr https://github.com/krrr/mactype/commit/146a213e2304208cb3c1a3e6fa941a386d908761
	m_nBitmapHeight = _GetFreeTypeProfileBoundInt(_T("MaxBitmap"), 0, 0, 255, lpszFile);
	m_bHintSmallFont = _GetFreeTypeProfileInt(_T("HintSmallFont"), 0, lpszFile);
	m_bDirectWrite = _GetFreeTypeProfileInt(_T("DirectWrite"), 0, lpszFile);
	m_nLcdFilter	= _GetFreeTypeProfileInt(_T("LcdFilter"), 0, lpszFile);
	m_nFontSubstitutes = _GetFreeTypeProfileBoundInt(_T("FontSubstitutes"),
													 SETTING_FONTSUBSTITUTE_DISABLE,
													 SETTING_FONTSUBSTITUTE_DISABLE,
													 SETTING_FONTSUBSTITUTE_ALL,
													 lpszFile);
	m_nWidthMode = SETTING_WIDTHMODE_GDI32;


	m_nFontLoader = _GetFreeTypeProfileBoundInt(_T("FontLoader"),
												SETTING_FONTLOADER_FREETYPE,
												SETTING_FONTLOADER_FREETYPE,
												SETTING_FONTLOADER_WIN32,
												lpszFile);
	m_nCacheMaxFaces = _GetFreeTypeProfileInt(_T("CacheMaxFaces"), 64, lpszFile);
	m_nCacheMaxFaces = m_nCacheMaxFaces > 64 ? m_nCacheMaxFaces : 64;
	m_nCacheMaxSizes = _GetFreeTypeProfileInt(_T("CacheMaxSizes"), 1200, lpszFile);
	m_nCacheMaxBytes = _GetFreeTypeProfileInt(_T("CacheMaxBytes"), 10485760, lpszFile);

	//parse display affinity string into an integer set
	{
		TCHAR sAffinity[260] = { 0 };
		_GetFreeTypeProfileString(_T("DisplayAffinity"), _T(""), sAffinity, 256, lpszFile);
		auto displays = SplitString(sAffinity);
		for (auto id : displays) {
			m_nDisplayAffinity.insert(id);
		}
	}

	//experimental settings:
	m_bEnableClipBoxFix = !!_GetFreeTypeProfileIntFromSection(_T("Experimental"), _T("ClipBoxFix"), 1, lpszFile);
	m_bColorFont = !!_GetFreeTypeProfileIntFromSection(_T("Experimental"), _T("ColorFont"), 0, lpszFile);
	m_bInvertColor = !!_GetFreeTypeProfileIntFromSection(_T("Experimental"), _T("InvertColor"), 0, lpszFile);
#ifdef INFINALITY
	// define some macros
#define INF_INT_ENV(y, def) \
	nTemp = _GetFreeTypeProfileIntFromSection(_T("Infinality"), _T(y), def, lpszFile); \
	FT_PutEnv(y, _ltoa(nTemp, buff, 10));
#define INF_BOOL_ENV(y, def) \
	bTemp = _GetFreeTypeProfileBoolFromSection(_T("Infinality"), _T(y), def, lpszFile); \
	FT_PutEnv(y, bTemp?"true":"false");
#define INF_STR_ENV(y, def) \
	sTemp = _GetFreeTypeProfileStrFromSection(_T("Infinality"), _T(y), def, lpszFile); \
	FT_PutEnv(y, WstringToString(sTemp).c_str());

	char buff[256] = {};
	int nTemp; bool bTemp; wstring sTemp;

	// INFINALITY settings:
	INF_INT_ENV( "INFINALITY_FT_CHROMEOS_STYLE_SHARPENING_STRENGTH", 0);
	INF_INT_ENV( "INFINALITY_FT_CONTRAST", 0);
	INF_INT_ENV( "INFINALITY_FT_STEM_FITTING_STRENGTH", 25);
	INF_INT_ENV( "INFINALITY_FT_AUTOHINT_SNAP_STEM_HEIGHT", 100);
	INF_INT_ENV( "INFINALITY_FT_GRAYSCALE_FILTER_STRENGTH", 0);
	INF_INT_ENV( "INFINALITY_FT_WINDOWS_STYLE_SHARPENING_STRENGTH", 20);
	INF_INT_ENV( "INFINALITY_FT_BRIGHTNESS", 0);
	INF_INT_ENV( "INFINALITY_FT_AUTOHINT_HORIZONTAL_STEM_DARKEN_STRENGTH", 10);
	INF_INT_ENV( "INFINALITY_FT_STEM_ALIGNMENT_STRENGTH", 25);
	INF_INT_ENV( "INFINALITY_FT_AUTOHINT_VERTICAL_STEM_DARKEN_STRENGTH", 25);
	INF_INT_ENV( "INFINALITY_FT_FRINGE_FILTER_STRENGTH", 0);
	INF_INT_ENV("INFINALITY_FT_GLOBAL_EMBOLDEN_X_VALUE", 0);
	INF_INT_ENV("INFINALITY_FT_GLOBAL_EMBOLDEN_Y_VALUE", 0);
	INF_INT_ENV("INFINALITY_FT_BOLD_EMBOLDEN_X_VALUE", 0);
	INF_INT_ENV("INFINALITY_FT_BOLD_EMBOLDEN_Y_VALUE", 0);
	INF_INT_ENV("INFINALITY_FT_STEM_SNAPPING_SLIDING_SCALE", 0);

	INF_BOOL_ENV("INFINALITY_FT_USE_KNOWN_SETTINGS_ON_SELECTED_FONTS", true);
	INF_BOOL_ENV( "INFINALITY_FT_AUTOFIT_ADJUST_HEIGHTS", true);
	INF_BOOL_ENV( "INFINALITY_FT_USE_VARIOUS_TWEAKS", true);
	INF_BOOL_ENV( "INFINALITY_FT_AUTOHINT_INCREASE_GLYPH_HEIGHTS", true);
	INF_BOOL_ENV( "INFINALITY_FT_STEM_DARKENING_CFF", true);
	INF_BOOL_ENV( "INFINALITY_FT_STEM_DARKENING_AUTOFIT", true);

	INF_STR_ENV( "INFINALITY_FT_GAMMA_CORRECTION", _T("0 100"));
	INF_STR_ENV( "INFINALITY_FT_FILTER_PARAMS", _T("11 22 38 22 11"));

#endif

	if (m_nFontLoader == SETTING_FONTLOADER_WIN32) {
		// APIが処理してくれるはずなので自前処理は無効化
		if (m_nFontSubstitutes == SETTING_FONTSUBSTITUTE_ALL) {
			m_nFontSubstitutes = SETTING_FONTSUBSTITUTE_DISABLE;
		}
		m_bFontLink = 0;
	}

	ZeroMemory(&m_lfForceFont, sizeof(LOGFONT));
	m_szForceChangeFont[0] = _T('\0');

	STARTUPINFO si = { sizeof(STARTUPINFO) };
	GetStartupInfo(&si);
	m_bRunFromGdiExe = IsGdiPPStartupInfo(si);

	m_arrExcludeFont.clear();
	m_arrIncludeFont.clear();
	m_arrExcludeModule.clear();
	m_arrIncludeModule.clear();
	m_arrUnloadModule.clear();
	m_arrUnFontSubModule.clear();

	AddListFromSection(_T("ExcludeModule"), lpszFile, m_arrExcludeModule);
	AddListFromSection(_T("IncludeModule"), lpszFile, m_arrIncludeModule);
	AddListFromSection(_T("UnloadDLL"), lpszFile, m_arrUnloadModule);
	AddListFromSection(L"ExcludeSub", lpszFile, m_arrUnFontSubModule);
	// A loaded ExcludeSub module disables substitution for this process.
	if (m_nFontSubstitutes)
	{
		ModuleHashMap::const_iterator it=m_arrUnFontSubModule.begin();
		while (it!=m_arrUnFontSubModule.end())
		{
			if (GetModuleHandle(it->c_str()))
			{
				m_nFontSubstitutes = 0;	//关闭替换
				break;
			}
			++it;
		}
	}

	wstring names = _T("LcdFilterWeight@") + wstring(m_szexeName);
	if (_IsFreeTypeProfileSectionExists(names.c_str(), lpszFile))
		m_bUseCustomLcdFilter = AddLcdFilterFromSection(names.c_str(), lpszFile, m_arrLcdFilterWeights);
	else
		m_bUseCustomLcdFilter = AddLcdFilterFromSection(_T("LcdFilterWeight"), lpszFile, m_arrLcdFilterWeights);

	m_bUseCustomPixelLayout = AddPixelModeFromSection(_T("PixelLayout"), lpszFile, m_arrPixelLayout);

	if (!selectedProfile->Unchanged())
		return false;
	m_profileDigest = selectedProfile->digest;
	return PublishRendererPolicySnapshot(false);
}

bool CGdippSettings::AddExcludeListFromSection(LPCTSTR lpszSection, LPCTSTR lpszFile, set<wstring> & arr)
{
	LPTSTR  buffer = _GetPrivateProfileSection(lpszSection, lpszFile);
	if (buffer == nullptr) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}

	LPTSTR p = buffer;
	TCHAR buff[LF_FACESIZE+1];
	LOGFONT truefont={0};
	while (*p) {
		bool b = false;
		GetFontLocalName(p, buff);//转换字体脕E
		set<wstring>::const_iterator it = arr.find(buff);
		if (it==arr.end())
			arr.insert(buff);
		for (; *p; p++);	//来到下一行
		p++;
	}
	return false;
}

//template <typename T>
bool CGdippSettings::AddListFromSection(LPCTSTR lpszSection, LPCTSTR lpszFile, set<wstring> & arr)
{
	LPTSTR  buffer = _GetPrivateProfileSection(lpszSection, lpszFile);
	if (buffer == nullptr) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}

	LPTSTR p = buffer;
	while (*p) {
		bool b = false;
		set<wstring>::const_iterator it = arr.find(p);
		if (it==arr.end())
			arr.insert(p);
		for (; *p; p++);	//来到下一行
			p++;
	}
	return false;
}

bool CGdippSettings::AddLcdFilterFromSection(LPCTSTR lpszKey, LPCTSTR lpszFile, unsigned char* arr)
{
	TCHAR buffer[100];
	_GetFreeTypeProfileString(lpszKey, _T("\0"), buffer, sizeof(buffer), lpszFile);
	if (buffer[0] == '\0') {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}

	LPTSTR p = buffer;
	CStringTokenizer token;
	int argc = 0;
	argc = token.Parse(buffer);

	for (int i = 0; i < 5; i++) {
		LPCTSTR arg = token.GetArgument(i);
		if (!arg)
			return false;	//参数少于5个则视为不使用此参数
		arr[i] = _StrToInt(arg, arr[i]);
	}

	return true;
}

bool CGdippSettings::AddPixelModeFromSection(LPCTSTR lpszKey, LPCTSTR lpszFile, char* arr)
{
	TCHAR buffer[100];
	_GetFreeTypeProfileString(lpszKey, _T("\0"), buffer, sizeof(buffer), lpszFile);
	if (buffer[0] == '\0') {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}

	LPTSTR p = buffer;
	CStringTokenizer token;
	int argc = 0;
	argc = token.Parse(buffer);

	for (int i = 0; i < 6; i++) {
		LPCTSTR arg = token.GetArgument(i);
		if (!arg)
			return false;
		arr[i] = _StrToInt(arg, arr[i]);
	}

	return true;
}

bool CGdippSettings::AddIndividualFromSection(LPCTSTR lpszSection, LPCTSTR lpszFile, IndividualArray& arr)
{
	LPTSTR  buffer = _GetPrivateProfileSection(lpszSection, lpszFile);
	if (buffer == nullptr) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}

	LPTSTR p = buffer;
	TCHAR buff[LF_FACESIZE+1];
	LOGFONT truefont={0};
	while (*p) {
		bool b = false;

		LPTSTR pnext = p;
		for (; *pnext; pnext++);

		//"ＭＳ Ｐゴシック=0,0" みたいな文字列を分割
		LPTSTR value = _tcschr(p, _T('='));
		CStringTokenizer token;
		int argc = 0;
		if (value) {
			*value++ = _T('\0');
			argc = token.Parse(value);
		}

		GetFontLocalName(p, buff);//转换字体脕E

		CFontIndividual fi(buff);
		const CFontSettings& fsCommon = m_FontSettings;
		CFontSettings& fs = fi.GetIndividual();
		//Individualが無ければ共通設定を使う
		fs = fsCommon;
		for (int i = 0; i < MAX_FONT_SETTINGS; i++) {
			LPCTSTR arg = token.GetArgument(i);
			if (!arg)
				break;
			const int n = _StrToInt(arg, fsCommon.GetParam(i));
			fs.SetParam(i, n);
		}

		for (int i = 0 ; i < arr.GetSize(); i++) {
			if (arr[i] == fi) {
				b = true;
				break;
			}
		}
		if (!b) {
			arr.Add(fi);
#ifdef _DEBUG
			TRACE(_T("Individual: %s, %d, %d, %d, %d, %d, %d\n"), fi.GetName(),
					fs.GetParam(0), fs.GetParam(1), fs.GetParam(2), fs.GetParam(3), fs.GetParam(4), fs.GetParam(5));
#endif
		}
		p = pnext;
		p++;
	}
	return false;
}

LPTSTR CGdippSettings::_GetPrivateProfileSection(LPCTSTR lpszSection, LPCTSTR lpszFile)
{
	return const_cast<LPTSTR>(static_cast<LPCTSTR>(m_Config[lpszSection]));
}

int CGdippSettings::_httoi(const TCHAR *value)
{
	struct CHexMap
	{
		TCHAR chr;
		int value;
	};
	const int HexMapL = 16;
	CHexMap HexMap[HexMapL] =
	{
		{'0', 0}, {'1', 1},
		{'2', 2}, {'3', 3},
		{'4', 4}, {'5', 5},
		{'6', 6}, {'7', 7},
		{'8', 8}, {'9', 9},
		{'A', 10}, {'B', 11},
		{'C', 12}, {'D', 13},
		{'E', 14}, {'F', 15}
	};
	renderer_raii::UniqueMallocMemory<TCHAR> mstr(_tcsdup(value));
	if (!mstr) return 0;
	_tcsupr(mstr.get());
	TCHAR *s = mstr.get();
	int result = 0;
	if (*s == '0' && *(s + 1) == 'X') s += 2;
	bool firsttime = true;
	while (*s != '\0')
	{
		bool found = false;
		for (int i = 0; i < HexMapL; i++)
		{
			if (*s == HexMap[i].chr)
			{
				if (!firsttime) result <<= 4;
				result |= HexMap[i].value;
				found = true;
				break;
			}
		}
		if (!found) break;
		s++;
		firsttime = false;
	}
	return result;
}

//atofにデフォルト値を返せるようにしたような物
float CGdippSettings::_StrToFloat(LPCTSTR pStr, float fDefault)
{
#define isspace(ch)		(ch == _T('\t') || ch == _T(' '))
#define isdigit(ch)		(static_cast<_TUCHAR>(ch - _T('0')) <= 9)

	int ret_i;
	int ret_d;
	float ret;
	bool neg = false;
	LPCTSTR pStart;

	for (; isspace(*pStr); pStr++);
	switch (*pStr) {
	case _T('-'):
		neg = true;
	case _T('+'):
		pStr++;
		break;
	}

	pStart = pStr;
	ret = 0;
	ret_i = 0;
	ret_d = 1;
	for (; isdigit(*pStr); pStr++) {
		ret_i = 10 * ret_i + (*pStr - _T('0'));
	}
	if (*pStr == _T('.')) {
		pStr++;
		for (; isdigit(*pStr); pStr++) {
			ret_i = 10 * ret_i + (*pStr - _T('0'));
			ret_d *= 10;
		}
	}
	ret = static_cast<float>(ret_i) / static_cast<float>(ret_d);

	if (pStr == pStart) {
		return fDefault;
	}
	return neg ? -ret : ret;

#undef isspace
#undef isdigit
}

bool CGdippSettings::IsFontExcluded(LPCSTR lpFaceName) const
{
	WCHAR szStack[LF_FACESIZE];
	LPWSTR lpUnicode = _StrDupExAtoW(lpFaceName, -1, szStack, LF_FACESIZE, nullptr);
	if (!lpUnicode) {
		return false;
	}
	renderer_raii::UniqueMallocMemory<WCHAR> heapUnicode(
		lpUnicode == szStack ? nullptr : lpUnicode);

	bool b = IsFontExcluded(lpUnicode);
	return b;
}

bool CGdippSettings::IsFontExcluded(LPCWSTR lpFaceName) const
{
	FontHashMap::const_iterator it = m_arrExcludeFont.find(lpFaceName);
	bool bExcluded = it != m_arrExcludeFont.end();	// if it's excluded, true
	if (!bExcluded && m_arrIncludeFont.size() != 0) {	// if it's not excluded, and includefont enabled
		FontHashMap::const_iterator it = m_arrIncludeFont.find(lpFaceName);
		bExcluded = it == m_arrIncludeFont.end();	// check if it's included
	}
	return bExcluded;
}

void CGdippSettings::AddFontExclude(LPCWSTR lpFaceName)
{
	if (!IsFontExcluded(lpFaceName))
		m_arrExcludeFont.insert(lpFaceName);
}

bool CGdippSettings::IsProcessUnload() const
{
	if (m_bRunFromGdiExe) {
		return false;
	}
	GetEnvironmentVariableW(L"MACTYPE_FORCE_LOAD", nullptr, 0);
	if (GetLastError()!=ERROR_ENVVAR_NOT_FOUND)
		return false;
	ModuleHashMap::const_iterator it = m_arrUnloadModule.begin();
	for(; it != m_arrUnloadModule.end(); ++it) {
		if (IsFolder(it->c_str())) {
			// Folder entries match the executable directory.
			if (GetAppDir() == LowerCase(*it)) {
				return true;
			}
		}
		else
		if (GetModuleHandleW(it->c_str())) {
			return true;
		}
	}
	return false;
}

bool CGdippSettings::IsExeUnload(LPCTSTR lpApp) const	//紒E槭欠裨诤诿チ斜?
{
	if (m_bRunFromGdiExe) {
		return false;
	}
	GetEnvironmentVariableW(L"MACTYPE_FORCE_LOAD", nullptr, 0);
	if (GetLastError()!=ERROR_ENVVAR_NOT_FOUND)
		return false;
	ModuleHashMap::const_iterator it = m_arrUnloadModule.begin();
	for(; it != m_arrUnloadModule.end(); ++it) {
		if (!lstrcmpi(lpApp, it->c_str())) {	//匹配排除蟻E
			return true;
		}
	}
	return false;
}

bool CGdippSettings::IsExeInclude(LPCTSTR lpApp) const	//紒E槭欠裨诎酌チ斜?
{
	if (m_bRunFromGdiExe) {
		return false;
	}
	GetEnvironmentVariableW(L"MACTYPE_FORCE_LOAD", nullptr, 0);
	if (GetLastError()!=ERROR_ENVVAR_NOT_FOUND)
		return false;
	ModuleHashMap::const_iterator it = m_arrIncludeModule.begin();
	for(; it != m_arrIncludeModule.end(); ++it) {
		if (!lstrcmpi(lpApp, it->c_str())) {	//匹配排除蟻E
			return true;
		}
	}
	return false;
}

bool CGdippSettings::IsProcessExcluded() const
{
	if (m_bRunFromGdiExe) {
		return false;
	}
	GetEnvironmentVariableW(L"MACTYPE_FORCE_EXCLUDE", nullptr, 0);
	if (GetLastError() != ERROR_ENVVAR_NOT_FOUND)
		return true;
	GetEnvironmentVariableW(L"MACTYPE_FORCE_LOAD", nullptr, 0);
	if (GetLastError()!=ERROR_ENVVAR_NOT_FOUND)
		return false;
	ModuleHashMap::const_iterator it = m_arrExcludeModule.begin();
	for(; it != m_arrExcludeModule.end(); ++it) {
		if (IsFolder(it->c_str())) {
			// Folder entries match every executable below that directory.
			if (GetAppDir().find(LowerCase(*it)) == 0) {
				return true;
			}
		}
		else
		if (GetModuleHandleW(it->c_str())) {
			return true;
		}
	}
	return false;
}

bool CGdippSettings::IsProcessIncluded() const
{
	if (m_bRunFromGdiExe) {
		return true;
	}
	GetEnvironmentVariableW(L"MACTYPE_FORCE_LOAD", nullptr, 0);
	if (GetLastError()!=ERROR_ENVVAR_NOT_FOUND)
		return true;
	ModuleHashMap::const_iterator it = m_arrIncludeModule.begin();
	for(; it != m_arrIncludeModule.end(); ++it) {
		if (IsFolder(it->c_str())) {
			// Folder entries match the executable directory.
			if (GetAppDir() == LowerCase(*it)) {
				return true;
			}
		}
		else
		if (GetModuleHandleW(it->c_str())) {
			return true;
		}
	}
	return false;
}

void CGdippSettings::InitInitTuneTable()
{
	int i, *table;
#define init_table(name) \
		for (i=0,table=name; i<256; i++) table[i] = i
	init_table(m_nTuneTable);
	init_table(m_nTuneTableR);
	init_table(m_nTuneTableG);
	init_table(m_nTuneTableB);
#undef init_table
}

// テーブル初期化関数 0 - 12まで
// LCD用テーブル初期化関数 各0 - 12まで
void CGdippSettings::InitTuneTable(int v, int* table)
{
	int i;
	int col;
	double tmp, p;

	if (v < 0) {
		return;
	}
	v = Min(v, 12);
	p = static_cast<double>(v);
	p = 1 - (p / (p + 10.0));
	for(i = 0;i < 256;i++){
	    tmp = static_cast<double>(i) / 255.0;
        tmp = pow(tmp, p);
	    col = 255 - static_cast<int>(tmp * 255.0 + 0.5);
		table[255 - i] = col;
	}
}

//見つからない場合は共通設定を返す
extern BOOL g_ccbIndividual;
const CFontSettings& CGdippSettings::FindIndividual(LPCTSTR lpFaceName) const
{
	CFontIndividual* p		= m_arrIndividual.Begin();
	CFontIndividual* end	= m_arrIndividual.End();
	if (lpFaceName && *lpFaceName==L'@')
		++lpFaceName;	//纵向字体使用横向的设定
	StringHashFont hash(lpFaceName);

	for(; p != end; ++p) {
		if (p->GetHash() == hash) {
			CFontSettings& result = p->GetIndividual();
			if (result.GetAntiAliasMode() > 2 && HarmonyLCD())
				result.SetAntiAliasMode(2);
			return result;
		}
	}
	return GetFontSettings();
}

int CGdippSettings::_GetAlternativeProfileName(LPTSTR lpszName, LPCTSTR lpszFile)
{
	TCHAR szexe[MAX_PATH + 1];
	TCHAR* pexe = szexe + GetModuleFileName(nullptr, szexe, MAX_PATH);
	while (pexe >= szexe && *pexe != '\\')
		pexe--;
	pexe++;
	wstring exename = _T("General@") + wstring(pexe);
	if (FastGetProfileString(exename.c_str(), _T("Alternative"), nullptr, lpszName, MAX_PATH))
	{
		return true;
	}
	else
	{
		//StringCchCopy(lpszName, MAX_PATH + 1, pexe);
		return false;
	}
}

bool CGdippSettings::CopyForceFont(LOGFONT& lf, const LOGFONT& lfOrg) const
{
	_ASSERTE(m_bDelayedInit);
	//__asm{ int 3 }
	SetLastError(ERROR_SUCCESS);
	const DWORD environmentLength =
		GetEnvironmentVariableW(L"MACTYPE_FONTSUBSTITUTES_ENV", nullptr, 0);
	if (environmentLength != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND)
		return false;
	// &lf == &lfOrg is supported: preserve the full request before resolving.
	LOGFONT const original = lfOrg;
	std::shared_ptr<const renderer::font_substitution::Snapshot> const snapshot =
		renderer::font_substitution::ProcessRegistry().Load();
	if (snapshot->rules().empty())
		return false;
	LOGFONT canonical = original;
	GetFontSubstitutesInfo().Canonicalize(canonical);
	renderer::font_substitution::Resolution const resolution = snapshot->Resolve(
		{canonical.lfFaceName, canonical.lfCharSet});
	if (!resolution.matched)
		return false;

	lf = original;
	return SUCCEEDED(StringCchCopy(
		lf.lfFaceName, LF_FACESIZE, resolution.family.c_str()));
}

CFontLinkInfo::CFontLinkInfo()
{
	memset(&info, 0, sizeof info);
	memset(AllowDefaultLink, 1, sizeof(AllowDefaultLink));	//默认允喧笾体链接
	memset(DefaultFontLink, 0, sizeof(DefaultFontLink));
}

CFontLinkInfo::~CFontLinkInfo()
{
	clear();
}


void CFontLinkInfo::init()
{
	const TCHAR REGKEY1[] = _T("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink");
	const TCHAR REGKEY2[] = _T("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts");
	const TCHAR REGKEY3[] = _T("SYSTEM\\CurrentControlSet\\Control\\FontAssoc\\Associated DefaultFonts");
	const TCHAR REGKEY4[] = _T("SYSTEM\\CurrentControlSet\\Control\\FontAssoc\\Associated Charset");

	HKEY rawKey = nullptr;
	if (ERROR_SUCCESS != RegOpenKeyEx(HKEY_LOCAL_MACHINE, REGKEY1, 0, KEY_QUERY_VALUE, &rawKey)) return;
	renderer_raii::UniqueRegistryKey h1(rawKey);
	rawKey = nullptr;
	if (ERROR_SUCCESS != RegOpenKeyEx(HKEY_LOCAL_MACHINE, REGKEY2, 0, KEY_QUERY_VALUE, &rawKey)) {
		return;
	}
	renderer_raii::UniqueRegistryKey h2(rawKey);
	std::vector<WCHAR> name(0x2000);
	DWORD namesz;
	DWORD valuesz;
	std::vector<WCHAR> value(0x2000);
	std::vector<WCHAR> buf(0x2000);
	const DWORD nBufSize = 0x2000 * sizeof(WCHAR);
	LONG rc;
	DWORD regtype;

	for (int k = 0; ; ++k) {	//获得字体柄蛐的所有字虂E
		namesz = static_cast<DWORD>(name.size());
		valuesz = nBufSize;
		rc = RegEnumValue(h2.get(), k, name.data(), &namesz, 0, &regtype, reinterpret_cast<LPBYTE>(value.data()), &valuesz);		//从字体柄蛐寻找
		if (rc == ERROR_NO_MORE_ITEMS) break;
		if (rc != ERROR_SUCCESS) break;
		if (regtype != REG_SZ) continue;
		StringCchCopy(buf.data(), buf.size(), name.data());
		size_t displayNameLength = wcslen(buf.data());
		if (displayNameLength != 0 && buf[displayNameLength - 1] == L')') {				//去掉括号
			LPWSTR p;
			if ((p = wcsrchr(buf.data(), L'(')) != nullptr) {
				*p = 0;
			}
		}
		displayNameLength = wcslen(buf.data());
		while (displayNameLength != 0 && buf[displayNameLength - 1] == L' ')
			buf[--displayNameLength] = 0;
		//获得的对应的字体脕E
		FontNameCache.Add(value.data(), buf.data());
	}

	int row = 0;
	LOGFONT truefont;
	memset(&truefont, 0, sizeof(truefont));
	for (int i = 0; row < INFOMAX; ++i) {
		int col = 0;

		namesz = static_cast<DWORD>(name.size());
		valuesz = nBufSize;
		rc = RegEnumValue(h1.get(), i, name.data(), &namesz, 0, &regtype, reinterpret_cast<LPBYTE>(value.data()), &valuesz);	//获得一个字体的字体链接
		if (rc == ERROR_NO_MORE_ITEMS) break;
		if (rc != ERROR_SUCCESS) break;
		if (regtype != REG_MULTI_SZ) continue;		//有效的字体链接
		//获得字体的真实名字

		TCHAR buff[LF_FACESIZE];
		GetFontLocalName(name.data(), buff);

		info[row][col] = _wcsdup(buff);		//第一消戟字体脕E
		++col;

		for (LPCWSTR linep = value.data(); col < FONTMAX && *linep; linep += wcslen(linep) + 1) {
			LPCWSTR valp = nullptr;
			for (LPCWSTR p = linep; *p; ++p) {
				if (*p == L',' && (static_cast<char>(*(p+1)) < 0x30 || static_cast<char>(*(p+1)) > 0x39))		//尝试寻找字体链接中“，”后提供的字体名称
					{
						LPWSTR lp;
						StringCchCopy(buf.data(), buf.size(), p + 1);
						if (lp=wcschr(buf.data(), L','))
							*lp = 0;
						valp = buf.data();
						break;
					}
			}
			if (!valp) {		//没找到字体链接中提供的名称
				LPWSTR lp;
				StringCchCopy(buf.data(), buf.size(), linep);
				if (lp=wcschr(buf.data(), L','))
					*lp = 0;

				valp = FontNameCache.Find(buf.data());
			}
			if (valp) {
				GetFontLocalName(const_cast<TCHAR*>(valp), buff);;
				info[row][col] = _wcsdup(buff);//truefont.lfFaceName);			//复制到链接柄蛐
				++col;
			}
		}
		if (col == 1) {			//只有一消楷即没有链接，删掉。
			free(info[row][0]);
			info[row][0] = nullptr;
		} else {
			++row;
		}
	}
	LOGFONT syslf = {0};
	HGDIOBJ h = ORIG_GetStockObject(DEFAULT_GUI_FONT);
	if (h) {
		ORIG_GetObjectW(h, sizeof syslf, &syslf);
		GetFontLocalName(syslf.lfFaceName, syslf.lfFaceName);
	}

	extern HFONT g_alterGUIFont;
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	if (pSettings->FontSubstitutes()>=SETTING_FONTSUBSTITUTE_SAFE && pSettings->CopyForceFont(truefont, syslf))	//使用蛠E婊荒Ｊ绞保婊坏粝低匙痔丒
	{
		WCHAR envname[30] = L"MT_SYSFONT";
		WCHAR envvalue[30] = { 0 };
		HFONT tempfont;
		if (GetEnvironmentVariable(L"MT_SYSFONT", envvalue, 29) && GetObjectType(tempfont = reinterpret_cast<HFONT>(wcstoull(envvalue, nullptr, 10))) == OBJ_FONT)//已经有字体存在
		{
			g_alterGUIFont = tempfont;	//直接使用先前的字虂E
		}
		else
		{
			g_alterGUIFont = CreateFontIndirectW(&truefont);	//创建一个新的替换字虂E
			_ui64tow(reinterpret_cast<ULONG_PTR>(g_alterGUIFont), envvalue, 10);	//转换为字符串
			SetEnvironmentVariable(envname, envvalue);		//写葋E肪潮淞?
		}
	}

	//现在获取对应字体类型的默认字体链接
	memset(DefaultFontLink, 0, sizeof(TCHAR)*(FF_DECORATIVE+1)*(LF_FACESIZE+1));	//初始化为0
	DWORD len;
	rawKey = nullptr;
	if (ERROR_SUCCESS != RegOpenKeyEx(HKEY_LOCAL_MACHINE, REGKEY3, 0, KEY_QUERY_VALUE, &rawKey)) return;
	renderer_raii::UniqueRegistryKey h3(rawKey);
	len = (LF_FACESIZE+1)*sizeof(TCHAR);
	RegQueryValueEx(h3.get(), _T("FontPackage"), 0, &regtype, reinterpret_cast<LPBYTE>(DefaultFontLink[1]), &len);
	len = (LF_FACESIZE+1)*sizeof(TCHAR);
	RegQueryValueEx(h3.get(), _T("FontPackageDecorative"), 0, &regtype, reinterpret_cast<LPBYTE>(DefaultFontLink[FF_DECORATIVE]), &len);
	len = (LF_FACESIZE+1)*sizeof(TCHAR);
	RegQueryValueEx(h3.get(), _T("FontPackageDontCare"), 0, &regtype, reinterpret_cast<LPBYTE>(DefaultFontLink[FF_DONTCARE]), &len);
	len = (LF_FACESIZE+1)*sizeof(TCHAR);
	RegQueryValueEx(h3.get(), _T("FontPackageModern"), 0, &regtype, reinterpret_cast<LPBYTE>(DefaultFontLink[FF_MODERN]), &len);
	len = (LF_FACESIZE+1)*sizeof(TCHAR);
	RegQueryValueEx(h3.get(), _T("FontPackageRoman"), 0, &regtype, reinterpret_cast<LPBYTE>(DefaultFontLink[FF_ROMAN]), &len);
	len = (LF_FACESIZE+1)*sizeof(TCHAR);
	RegQueryValueEx(h3.get(), _T("FontPackageScript"), 0, &regtype, reinterpret_cast<LPBYTE>(DefaultFontLink[FF_SCRIPT]), &len);
	len = (LF_FACESIZE+1)*sizeof(TCHAR);
	RegQueryValueEx(h3.get(), _T("FontPackageSwiss"), 0, &regtype, reinterpret_cast<LPBYTE>(DefaultFontLink[FF_SWISS]), &len);

	for (int i=0; i<FF_DECORATIVE+1; ++i)	//转换字体名称
	{
		if (!*DefaultFontLink[i])
			GetFontLocalName(DefaultFontLink[i], DefaultFontLink[i]);
	}

	//现在获取对应的CodePage是否需要进行fontlink。默认都需要进行链接。
	rawKey = nullptr;
	if (ERROR_SUCCESS != RegOpenKeyEx(HKEY_LOCAL_MACHINE, REGKEY4, 0, KEY_QUERY_VALUE, &rawKey)) return;
	renderer_raii::UniqueRegistryKey h4(rawKey);

	for (int i=0; i<0xff; ++i)
	{
		namesz = static_cast<DWORD>(name.size());
		valuesz = nBufSize;
		rc = RegEnumValue(h4.get(), i, name.data(), &namesz, 0, &regtype, reinterpret_cast<LPBYTE>(value.data()), &valuesz);	//获得一个charset的值
		if (rc == ERROR_NO_MORE_ITEMS) break;
		if (rc != ERROR_SUCCESS) break;
		if (regtype != REG_SZ) continue;
		if (_tcsicmp(value.data(), _T("YES")))
		{
			TCHAR* p = name.data();
			while (*p != _T('(') && p - name.data() < static_cast<int>(namesz)) ++p;
			++p;
			AllowDefaultLink[_tcstol(p,nullptr,16)]=false;
		}
	}
}

void CFontLinkInfo::clear()
{
	for (int i = 0; i < INFOMAX; ++i) {
		for (int j = 0; j < FONTMAX; ++j) {
			free(info[i][j]);
			info[i][j] = nullptr;
		}
	}
}

const LPCWSTR * CFontLinkInfo::lookup(LPCWSTR fontname) const
{
	for (int i = 0; i < INFOMAX && info[i][0]; ++i) {
		if (_wcsicmp(fontname, info[i][0]) == 0) {
			return &info[i][1];
		}
	}
	return nullptr;
}

LPCWSTR CFontLinkInfo::get(int row, int col) const
{
	if (static_cast<unsigned int>(row) >= static_cast<unsigned int>(INFOMAX) || static_cast<unsigned int>(col) >= static_cast<unsigned int>(FONTMAX)) {
		return nullptr;
	}
	return info[row][col];
}

CFontSubstituteData::CFontSubstituteData()
{
	memset(this, 0, sizeof *this);
}

int CALLBACK
CFontSubstituteData::EnumFontFamProc(const LOGFONT *lplf, const TEXTMETRIC *lptm, DWORD /*FontType*/, LPARAM lParam)
{
	CFontSubstituteData& self = *reinterpret_cast<CFontSubstituteData*>(lParam);
	self.m_lf = *lplf;
	return 0;
}

bool CFontSubstituteData::initnocheck(LPCTSTR config) {
	memset(this, 0, sizeof *this);

	TCHAR buf[LF_FACESIZE + 20];
	StringCchCopy(buf, countof(buf), config);

	memset(&m_lf, 0, sizeof m_lf);

	LPTSTR p;
	for (p = buf + lstrlen(buf) - 1; p >= buf; --p) {
		if (*p == _T(',')) {
			*p++ = 0;
			break;
		}
	}
	if (p >= buf) {
		StringCchCopy(m_lf.lfFaceName, countof(m_lf.lfFaceName), buf);
		m_lf.lfCharSet = static_cast<BYTE>(_StrToInt(p + 1, 0));
		m_bCharSet = true;
	}
	else {
		StringCchCopy(m_lf.lfFaceName, LF_FACESIZE, buf);
		m_lf.lfCharSet = DEFAULT_CHARSET;
		m_bCharSet = false;
	}
	return m_lf.lfFaceName[0] != 0;
}

bool CFontSubstituteData::init(LPCTSTR config)
{
	if (!config || !initnocheck(config)) return false;

	LOGFONT requested = m_lf;
	auto hdc = renderer_raii::AdoptWindowDeviceContext(nullptr, GetDC(nullptr));
	if (hdc) {
		// Prefer the installed family's canonical LOGFONT when GDI is available.
		// If enumeration finds nothing, keep the exact configured name parsed by
		// initnocheck so DirectWrite-only and restricted processes can resolve it.
		EnumFontFamiliesEx(hdc.get(), &requested, &CFontSubstituteData::EnumFontFamProc,
			reinterpret_cast<LPARAM>(this), 0);
	}

	return m_lf.lfFaceName[0] != 0;
}

bool
CFontSubstituteData::operator == (const CFontSubstituteData& o) const
{
	if (m_bCharSet != o.m_bCharSet) return false;
	if (m_bCharSet) {
		if (m_lf.lfCharSet != o.m_lf.lfCharSet) return false;
	}
	if (_wcsicmp(m_lf.lfFaceName, o.m_lf.lfFaceName) == 0) return true;
	return false;
}

// Extend configured substitutions through registry aliases that target them.
void CFontSubstitutesInfo::initreg()
{
	const LPCTSTR REGKEY = _T("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes");
	HKEY rawKey = nullptr;
	if (ERROR_SUCCESS != RegOpenKeyEx(HKEY_LOCAL_MACHINE, REGKEY, 0, KEY_QUERY_VALUE, &rawKey)) return;
	renderer_raii::UniqueRegistryKey h(rawKey);
	CFontSubstituteData k;
	CFontSubstituteData v;

	std::vector<WCHAR> name(0x2000);
	std::vector<WCHAR> value(0x2000);
	DWORD namesz, valuesz;
	DWORD regtype;

	for (int i = 0; ; ++i) {
		namesz = static_cast<DWORD>(name.size());
		valuesz = static_cast<DWORD>(value.size() * sizeof(WCHAR));
		LONG rc = RegEnumValue(h.get(), i, name.data(), &namesz, 0, &regtype, reinterpret_cast<LPBYTE>(value.data()), &valuesz);
		if (rc == ERROR_NO_MORE_ITEMS) break;
		if (rc != ERROR_SUCCESS) break;
		if (regtype != REG_SZ) continue;

		if (k.initnocheck(name.data()) && v.init(value.data())) {	// init k and v (k is a virtual font)
			int pos = FindKey(v);
			if ( pos >= 0) {	// check if v is substituted to another font x
				Add(k, GetValueAt(pos));	// add k=>x as well
			}
		}
	}
}

void
CFontSubstitutesInfo::initini(const CFontSubstitutesIniArray& iniarray)
{
	CFontSubstitutesIniArray::const_iterator it=iniarray.begin();
//	LOGFONT truefont={0}, truefont2={0};
//	TCHAR* buff, *buff2;
	for (; it!=iniarray.end(); ++it) {
		LPCTSTR inistr = it->c_str();
		renderer_raii::UniqueMallocMemory<TCHAR> buf(_tcsdup(inistr));
		if (!buf) continue;
		for (LPTSTR vp = buf.get(); *vp; ++vp) {
			if (*vp == _T('=')) {
				*vp++ = 0;
				CFontSubstituteData k;
				CFontSubstituteData v;
				if (k.init(buf.get()) && v.init(vp)) {
				if (FindKey(k) < 0 && k.m_bCharSet == v.m_bCharSet) Add(k, v);
				}
			}
		}
	}
}

void
CFontSubstitutesInfo::init(int nFontSubstitutes, const CFontSubstitutesIniArray& iniarray)
{
	if (nFontSubstitutes >= SETTING_FONTSUBSTITUTE_SAFE) {
		initini(iniarray);	// init substitution from ini array
		initreg(); // add more substitutions from registry
	}
}

void GetMacTypeInternalFontName(LOGFONT* lf, LPTSTR fn)
{
	StringCchCopy(fn, 9, _T("MACTYPE_"));
	fn+=8;
	TCHAR* lfbyte = reinterpret_cast<TCHAR*>(lf);
	for (int i=0;i<(sizeof(LOGFONT)-sizeof(TCHAR)*LF_FACESIZE+1)/sizeof(TCHAR);i++)
		*fn++=_T('0') + *lfbyte++;
	*fn=0;
}

void CFontSubstitutesInfo::Canonicalize(LOGFONT& lf) const
{
	TCHAR* buff;
	LOGFONT mylf(lf);
	if (!(buff = FontNameCache.Find(lf.lfFaceName)))
	{
		TCHAR localname[LF_FACESIZE+1];
		if (GetFontLocalName(mylf.lfFaceName, localname)) {
			FontNameCache.Add(lf.lfFaceName, localname);
			StringCchCopy(mylf.lfFaceName, LF_FACESIZE, localname);
		}
		else
		{
			TCHAR inName[LF_FACESIZE];
			GetMacTypeInternalFontName(&mylf, inName);
			if (!(buff = FontNameCache.Find(inName)))
			{
				mylf.lfClipPrecision = FONT_MAGIC_NUMBER;
				renderer_raii::UniqueFont tempfont(CreateFontIndirect(&mylf));
				renderer_raii::UniqueDeviceContext dc(CreateCompatibleDC(nullptr));
				if (tempfont && dc) {
					auto selectedFont = renderer_raii::SelectObject(dc.get(), tempfont.get());
					if (selectedFont) {
						ORIG_GetTextFaceW(dc.get(), LF_FACESIZE, mylf.lfFaceName);
					}
				}
				FontNameCache.Add(inName, mylf.lfFaceName);
			}
		}
		buff = mylf.lfFaceName;
	}
	StringCchCopy(lf.lfFaceName, LF_FACESIZE, buff);
}

const LOGFONT *
CFontSubstitutesInfo::lookup(LOGFONT& lf) const
{
	if (GetSize() <= 0) return nullptr;
	Canonicalize(lf);
	CFontSubstituteData k;
	k.m_bCharSet = true;
	k.m_lf = lf;

	int pos = FindKey(k);
	if (pos < 0) {
		k.m_bCharSet = false;
		pos = FindKey(k);
	}
	if (pos >= 0) {
		return reinterpret_cast<const LOGFONT*>(&GetValueAt(pos));
	}
	return nullptr;
}

bool CFontSubstitutesInfo::CopyRule(
	int index,
	LOGFONT& source,
	LOGFONT& replacement,
	bool& charsetSpecific) const
{
	if (index < 0 || index >= GetSize())
		return false;
	CFontSubstituteData const& key = GetKeyAt(index);
	source = key.m_lf;
	replacement = GetValueAt(index).m_lf;
	charsetSpecific = key.m_bCharSet;
	return source.lfFaceName[0] != L'\0' &&
		replacement.lfFaceName[0] != L'\0';
}

CFontFaceNamesEnumerator::CFontFaceNamesEnumerator(LPCWSTR facename, int nFontFamily) : m_pos(0)
{
	//CCriticalSectionLock __lock;
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	TCHAR  buff[LF_FACESIZE+1];
	GetFontLocalName(const_cast<TCHAR*>(facename), buff);
	LPCWSTR srcfacenames[] = {
		buff, nullptr, nullptr
	};

	int destpos = 0;
	for (const LPCWSTR *p = srcfacenames; *p && destpos < MAXFACENAMES; ++p) {
		m_facenames[destpos++] = *p;
		if (pSettings->FontLink()) {
			const LPCWSTR *facenamep = pSettings->GetFontLinkInfo().lookup(*p);
			if (facenamep) {
				for ( ; *facenamep && **facenamep && destpos < MAXFACENAMES; ++facenamep) {
					m_facenames[destpos++] = *facenamep;
				}
			}
		}
	}
	m_facenames[0] = facename;
	if (pSettings->FontLink() &&
		pSettings->FontLoader() == SETTING_FONTLOADER_FREETYPE) {
			m_facenames[destpos++] = pSettings->GetFontLinkInfo().sysfn(nFontFamily);
	}
	m_endpos = destpos;
}

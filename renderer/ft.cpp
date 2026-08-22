/* 2006-10-23(by 555)
* http://hp.vector.co.jp/authors/VA028002/winfreetype.c (higambana(菅野友紀))
* を丸写し
*/
/* 2006-10-27(by 555)
* http://hp.vector.co.jp/authors/VA028002/freetype.html (higambana(菅野友紀))
* を参考にしてやり直し
*/
/* 2006-10-29(by 555)
* 693氏(と呼ぶことにする)の精力的な活動によって出来上がったウハウハソースと
* 上記サイトの変更点を元にみみっちい修正。(ベースgdi0164)
*/
/* (by 555)
* さらに線引きもウハウハにしてもらったgdi0168を元に
* イタリックとボールドを追加。
*/
/* (by sy567)
* 太字のアルゴリズムを変更。
* ガンマ補正を実装してみる。
*/
#include "override.h"
#include "ft.h"
#include <windows.h>
#include <tchar.h>

#include <math.h>

#include <ft2build.h>
#include <freetype/tttables.h>
#include <freetype/ftmodapi.h>
#include <freetype/freetype.h>	/* FT_FREETYPE_H */
#include <freetype/ftcache.h>	/* FT_CACHE_H */
#include <freetype/ftoutln.h>	// FT_OUTLINE_H
#include <freetype/fttrigon.h>	//FT_TRIGONOMETRY_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SFNT_NAMES_H

#ifdef FT_LCD_FILTER_H
#include <freetype/ftlcdfil.h>	// FT_LCD_FILTER_H
#endif

#include "fteng.h"
#include "freetype_raii.h"
#include "freetype_runtime.h"
#include "profile_runtime.h"

#include "ft2vert.h"

#include "colorinvert.h"

FT_BitmapGlyphRec empty_glyph = {};//优化控制字

#define FT_BOLD_LOW 15
#define IsFontBold(lf)		((lf).lfWeight >= FW_BOLD)
#define FT_FixedToInt(x)	(FT_RoundFix(x) >> 16)
#define FT_PosToInt(x)		(((x) + (1 << 5)) >> 6)
#define RESOLUTION_X 72
#define RESOLUTION_Y 72
FT_Error New_FT_Outline_Embolden(FT_Outline* outline, FT_Pos str_h, FT_Pos str_v, FT_Int font_size);
FT_Error Old_FT_Outline_Embolden(FT_Outline* outline, FT_Pos strength);
FT_Error Vert_FT_Outline_Embolden(FT_Outline* outline, FT_Pos strength);
ControlIder CID;

renderer::freetype::RasterPolicy CaptureFreeTypeRasterPolicy()
{
	renderer::RendererPolicyRef const policy =
		renderer::CurrentRendererPolicy();
	return policy ? policy->raster() : renderer::freetype::RasterPolicy{};
}

#if _MSC_VER <= 1200
#pragma warning(disable: 4786)
#endif


//更新
#define RGBA(r,g,b,a)          (static_cast<COLORREF>(static_cast<BYTE>(r) | (static_cast<WORD>(static_cast<BYTE>(g)) << 8) | (static_cast<DWORD>(static_cast<BYTE>(b)) << 16) | (static_cast<DWORD>(static_cast<BYTE>(a)) << 24)))
//!!Snowie

COLORREF GetPaletteColor(HDC hdc, UINT paletteindex)
{
	HPALETTE hpal = reinterpret_cast<HPALETTE>(GetCurrentObject(hdc, OBJ_PAL));
	PALETTEENTRY lppe = {};
	memset(&lppe, 0, sizeof(lppe));
	GetPaletteEntries(hpal, paletteindex & 0xffff, 1, &lppe);
	return RGB(lppe.peRed, lppe.peGreen, lppe.peBlue);
}

bool EmBoldVariableFont(const FT_Face face, int boldWeight) {
	FT_MM_Var* rawMmVar = nullptr;
	auto error = FT_Get_MM_Var(face, &rawMmVar);
	if (error) {
		return false;	// not a varaible font
	}
	auto mmVar = renderer_raii::AdoptMmVar(freetype_library, rawMmVar);
	FT_MM_Var* mm_var = static_cast<FT_MM_Var*>(mmVar.get());
	auto num_axes = mm_var->num_axis;
	std::vector<FT_Fixed> coords(num_axes);
	bool axisFound = false;

	// Find 'wght' axis and set coordinates
	for (unsigned int i = 0; i < num_axes; ++i) {
		FT_Var_Axis* axis = &mm_var->axis[i];
		if (axis->tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
			axisFound = true;
			// fix out-of-range bold weight
			if (boldWeight < axis->minimum) {
				boldWeight = axis->minimum;
			}
			if (boldWeight > axis->maximum) {
				boldWeight = axis->maximum;
			}
			coords[i] = boldWeight;
		}
		else {
			// Set other axes to their default values
			coords[i] = axis->def;
		}
	}

	if (!axisFound) {
		return false;	// no "wght" axis found
	}

	// Apply the design coordinates
	// Variation selection is best-effort; retain the base face on failure.
	FT_Set_Var_Design_Coordinates(face, num_axes, coords.data());
	return true;
}


void Log(char* Msg)
{
#ifndef _DEBUG
	return;
#endif
	renderer_raii::UniqueFile f(fopen(".\\gdipp.log", "a"));
	if (!f) return;
	fputs(Msg, f.get());
}

void Log(wchar_t* Msg)
{
#ifndef _DEBUG
	return;
#endif
	renderer_raii::UniqueFile f(_wfopen(L".\\gdipp.log", L"a,ccs=UNICODE"));
	if (!f) return;
	fputws(Msg, f.get());
}

extern "C" FT_Error FT_Glyph_To_BitmapEx(FT_Glyph * the_glyph,
	FT_Render_Mode  render_mode,
	FT_Vector * origin,
	FT_Bool         destroy,
	FT_Bool			loadcolor,
	FT_UInt			glyphindex,
	FT_Face			face);


class CAlphaBlend
{
private:
	std::vector<int> alphatbl;
	std::vector<int> tbl1;
	std::vector<BYTE> tbl2;
	// 通常のアルファ値補正
	std::vector<int> tunetbl;
	std::vector<int> tunetblR;
	std::vector<int> tunetblG;
	std::vector<int> tunetblB;
	// 影文字用のアルファ値補正
	std::vector<int> tunetblS;
	std::vector<int> tunetblRS;
	std::vector<int> tunetblGS;
	std::vector<int> tunetblBS;

	std::vector<int> tunetblLS;
	std::vector<int> tunetblLRS;
	std::vector<int> tunetblLGS;
	std::vector<int> tunetblLBS;
	//Snowie!!
	std::vector<double> RGB2CRT;	//table used for RGB<->Lab
public:
	static const int BASE;
public:
	CAlphaBlend() :
		alphatbl(256),
		tbl1(257),
		tbl2(256 * 16 + 1),
		tunetbl(256),
		tunetblR(256),
		tunetblG(256),
		tunetblB(256),
		tunetblS(256),
		tunetblRS(256),
		tunetblGS(256),
		tunetblBS(256),
		tunetblLS(256),
		tunetblLRS(256),
		tunetblLGS(256),
		tunetblLBS(256),
		RGB2CRT(256) {}
	~CAlphaBlend() {}
	void init();
	void initRGB();
	double* GetRGBTable() { return RGB2CRT.data(); }
	BYTE doAB(BYTE fg, BYTE bg, int alpha);
	void gettunetbl(int paramalpha, BOOL lcd, BOOL dark, const int*& tblR, const int*& tblG, const int*& tblB) const;
	inline int conv1(BYTE n) {
		return tbl1[n];
	}
	inline BYTE conv2(int n) {
		return tbl2[n / (BASE * BASE / (tbl2.size() - 1))];
	}
private:
	inline int convalpha(int alpha) {
		return alphatbl[alpha];
	}
	inline BYTE rconv1(int n);
};
const int CAlphaBlend::BASE = 0x4000;

static CAlphaBlend s_AlphaBlendTable;

void CAlphaBlend::gettunetbl(int paramalpha, BOOL lcd, BOOL dark, const int*& tblR, const int*& tblG, const int*& tblB) const
{
	if (paramalpha == 1) {	//获取文字混合表
		if (lcd) {
			tblR = tunetblR.data();
			tblG = tunetblG.data();
			tblB = tunetblB.data();
		}
		else {
			tblR = tblG = tblB = tunetbl.data();
		}
	}
	else {	//获取阴影混合表
		if (dark)
		{
			if (lcd) {
				tblR = tunetblRS.data();
				tblG = tunetblGS.data();
				tblB = tunetblBS.data();
			}
			else {
				tblR = tblG = tblB = tunetblS.data();
			}
		}
		else
		{
			if (lcd) {
				tblR = tunetblLRS.data();
				tblG = tunetblLGS.data();
				tblB = tunetblLBS.data();
			}
			else {
				tblR = tblG = tblB = tunetblLS.data();
			}
		}
	}
}

void CAlphaBlend::initRGB()
{
	for (int i = 0; i < 256; i++)
		RGB2CRT[i] = pow(i / 255.0, 2.2);
}

void CAlphaBlend::init()
{
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	const float gamma = pSettings->GammaValue();
	const float weight = pSettings->RenderWeight();
	const float contrast = pSettings->Contrast();
	const int mode = pSettings->GammaMode();

	int i;
	float temp, alpha;

	for (i = 0; i < 256; ++i) {
		temp = pow((1.0f / 255.0f) * i, 1.0f / weight);

		if (temp < 0.5f) {
			alpha = pow(temp * 2, contrast) / 2.0f;
		}
		else {
			alpha = 1.0f - pow((1.0f - temp) * 2, contrast) / 2.0f;
		}
		alphatbl[i] = static_cast<int>(alpha * BASE);

		if (mode < 0) {
			temp = (1.0f / 255.0f) * i;
		}
		else {
			if (mode == 1) {
				if (i <= 10) {
					temp = static_cast<float>(i) / (12.92f * 255.0f);
				}
				else {
					temp = pow(((1.0f / 255.0f) * i + 0.055f) / 1.055f, 2.4f);
				}
			}
			else if (mode == 2) {
				if (i <= 10) {
					temp = (static_cast<float>(i) / (12.92f * 255.0f) + static_cast<float>(i) / 255.0f) / 2;
				}
				else {
					temp = (pow(((1.0f / 255.0f) * i + 0.055f) / 1.055f, 2.4f) + static_cast<float>(i) / 255.0f) / 2;
				}
			}
			else {
				temp = pow((1.0f / 255.0f) * i, gamma);
			}
		}
		tbl1[i] = static_cast<int>(temp * BASE);
	}

	tbl1[i] = BASE;

	for (i = 0; i <= tbl2.size() - 1; ++i) {
		tbl2[i] = rconv1(i * (BASE / (tbl2.size() - 1)));
	}

	const int* table = pSettings->GetTuneTable();
	const int* tableR = pSettings->GetTuneTableR();
	const int* tableG = pSettings->GetTuneTableG();
	const int* tableB = pSettings->GetTuneTableB();
	const int* shadow = pSettings->GetShadowParams();
	const int paramalpha = Max(shadow[2], 1);
	const int lightparamalpha = Max(shadow[3], 1);

	for (i = 0; i < 256; ++i) {
		tunetbl[i] = Bound(0, alphatbl[Bound(table[i], 0, 255)], CAlphaBlend::BASE);
		tunetblR[i] = Bound(0, alphatbl[Bound(tableR[i], 0, 255)], CAlphaBlend::BASE);
		tunetblG[i] = Bound(0, alphatbl[Bound(tableG[i], 0, 255)], CAlphaBlend::BASE);
		tunetblB[i] = Bound(0, alphatbl[Bound(tableB[i], 0, 255)], CAlphaBlend::BASE);
		tunetblS[i] = Bound(0, alphatbl[Bound(table[i] * paramalpha / 100, 0, 255)], CAlphaBlend::BASE);
		tunetblRS[i] = Bound(0, alphatbl[Bound(tableR[i] * paramalpha / 100, 0, 255)], CAlphaBlend::BASE);
		tunetblGS[i] = Bound(0, alphatbl[Bound(tableG[i] * paramalpha / 100, 0, 255)], CAlphaBlend::BASE);
		tunetblBS[i] = Bound(0, alphatbl[Bound(tableB[i] * paramalpha / 100, 0, 255)], CAlphaBlend::BASE);	//浅色混合表

		tunetblLS[i] = Bound(0, alphatbl[Bound(table[i] * lightparamalpha / 100, 0, 255)], CAlphaBlend::BASE);
		tunetblLRS[i] = Bound(0, alphatbl[Bound(tableR[i] * lightparamalpha / 100, 0, 255)], CAlphaBlend::BASE);
		tunetblLGS[i] = Bound(0, alphatbl[Bound(tableG[i] * lightparamalpha / 100, 0, 255)], CAlphaBlend::BASE);
		tunetblLBS[i] = Bound(0, alphatbl[Bound(tableB[i] * lightparamalpha / 100, 0, 255)], CAlphaBlend::BASE);	//深色混合表
	}
}

BYTE CAlphaBlend::rconv1(int n)
{
	int pos = 0x80;
	int i = pos >> 1;
	while (i > 0) {
		if (n >= tbl1[pos]) {
			pos += i;
		}
		else {
			pos -= i;
		}
		i >>= 1;
	}
	if (n >= tbl1[pos]) {
		++pos;
	}
	return static_cast<BYTE>(pos - 1);
}

class CAlphaBlendColorOne
{
private:
	BYTE fg;
	int temp_fg;
	const int* tunetbl;
	BYTE bg0;
	int alpha0;
	BYTE c0;
public:
	CAlphaBlendColorOne()
		: fg(0), temp_fg(0), tunetbl(nullptr), bg0(0), alpha0(0), c0(0) {}
	void init(BYTE f, const int* tbl);
	~CAlphaBlendColorOne() {};
	BYTE doAB(BYTE bg, int alpha);
};

FORCEINLINE void CAlphaBlendColorOne::init(BYTE f, const int* tbl)
{
	fg = f;
	temp_fg = s_AlphaBlendTable.conv1(fg);
	tunetbl = tbl;
}

FORCEINLINE BYTE CAlphaBlendColorOne::doAB(BYTE bg, int alpha)
{
	int temp_alpha = tunetbl[alpha];

	return temp_alpha ? s_AlphaBlendTable.conv2(s_AlphaBlendTable.conv1(bg) * (s_AlphaBlendTable.BASE - tunetbl[alpha]) +
		temp_fg * tunetbl[alpha]) : bg;

}

class CAlphaBlendColor
{
private:
	CAlphaBlendColorOne r;
	CAlphaBlendColorOne g;
	CAlphaBlendColorOne b;
public:
	CAlphaBlendColor(COLORREF newColor, int paramalpha, BOOL lcd, BOOL dark, BOOL gbr = false);
	~CAlphaBlendColor() { }
	BYTE doABsub(BYTE fg, int temp_fg, BYTE bg, int temp_alpha) const;
	COLORREF doAB(COLORREF baseColor, int alphaR, int alphaG, int alphaB, BOOL bClearAlpha);
	COLORREF doAB(COLORREF baseColor, int alpha, BOOL bClearAlpha) {
		return doAB(baseColor, alpha, alpha, alpha, bClearAlpha);
	}
private:
	CAlphaBlendColor() { }
};

FORCEINLINE CAlphaBlendColor::CAlphaBlendColor(COLORREF newColor, int paramalpha, BOOL lcd, BOOL dark, BOOL gbr)
{
	const int* tblR;
	const int* tblG;
	const int* tblB;
	s_AlphaBlendTable.gettunetbl(paramalpha, lcd, dark, tblR, tblG, tblB);
	if (!gbr) {
		r.init(GetRValue(newColor), tblR);
		b.init(GetBValue(newColor), tblB);
	}
	else {
		r.init(GetBValue(newColor), tblB);
		b.init(GetRValue(newColor), tblR);
	}
	g.init(GetGValue(newColor), tblG);
}

FORCEINLINE COLORREF CAlphaBlendColor::doAB(COLORREF baseColor, int alphaR, int alphaG, int alphaB, BOOL bClearAlpha)
{
	if (alphaB | alphaG | alphaR)
	{
		if (bClearAlpha)
			return RGB(r.doAB(GetRValue(baseColor), alphaR),
				g.doAB(GetGValue(baseColor), alphaG),
				b.doAB(GetBValue(baseColor), alphaB));
		else
			return RGBA(r.doAB(GetRValue(baseColor), alphaR),
				g.doAB(GetGValue(baseColor), alphaG),
				b.doAB(GetBValue(baseColor), alphaB),
				baseColor >> 24);
	}
	else
		return baseColor;
}

FORCEINLINE BYTE CAlphaBlend::doAB(BYTE fg, BYTE bg, int alpha)
{
	if (fg == bg || alpha <= 0) return bg;
	if (alpha >= 255) return fg;
	int temp_alpha = convalpha(alpha);
	int temp_bg = conv1(bg);
	int temp_fg = conv1(fg);
	int temp = temp_bg * (BASE - temp_alpha) +
		temp_fg * temp_alpha;
	return conv2(temp);
}

FORCEINLINE BYTE DoAlphaBlend(BYTE fg, BYTE bg, int alpha)
{
	return s_AlphaBlendTable.doAB(fg, bg, alpha);
}

// LCD(液晶)用のアルファブレンド(サブピクセルレンダリング)
static FORCEINLINE
COLORREF AlphaBlendColorLCD(
	COLORREF baseColor,
	COLORREF newColor,
	int alphaR, int alphaG, int alphaB,
	const int* tableR, const int* tableG, const int* tableB,
	const FreeTypeDrawInfo& ftdi)
{
	const BYTE rs = GetRValue(baseColor);
	const BYTE gs = GetGValue(baseColor);
	const BYTE bs = GetBValue(baseColor);
	BYTE rd = GetRValue(newColor);
	BYTE gd = GetGValue(newColor);
	BYTE bd = GetBValue(newColor);
	// アルファ値を補正
	alphaR = tableR[alphaR] / ftdi.params->alpha;
	alphaG = tableG[alphaG] / ftdi.params->alpha;
	alphaB = tableB[alphaB] / ftdi.params->alpha;
	//	rd = (((rd - rs) * alphaR) / 255) + rs;
	//	gd = (((gd - gs) * alphaG) / 255) + gs;
	//	bd = (((bd - bs) * alphaB) / 255) + bs;
	rd = DoAlphaBlend(rd, rs, alphaR);
	gd = DoAlphaBlend(gd, gs, alphaG);
	bd = DoAlphaBlend(bd, bs, alphaB);
	return RGB(rd, gd, bd);
}

// アルファブレンド(256階調)
static FORCEINLINE
COLORREF AlphaBlendColor(
	COLORREF baseColor,
	COLORREF newColor,
	int alpha, const int* table,
	const FreeTypeDrawInfo& ftdi)
{
	const BYTE rs = GetRValue(baseColor);
	const BYTE gs = GetGValue(baseColor);
	const BYTE bs = GetBValue(baseColor);
	BYTE rd = GetRValue(newColor);
	BYTE gd = GetGValue(newColor);
	BYTE bd = GetBValue(newColor);
	// アルファ値を補正
	alpha = table[alpha] / ftdi.params->alpha;
	//	rd = (rs * (255 - alpha) + rd * alpha) / 255;
	//	gd = (gs * (255 - alpha) + gd * alpha) / 255;
	//	bd = (bs * (255 - alpha) + bd * alpha) / 255;

	//	rd = (((rd - rs) * alpha) / 255) + rs;
	//	gd = (((gd - gs) * alpha) / 255) + gs;
	//	bd = (((bd - bs) * alpha) / 255) + bs;
	rd = DoAlphaBlend(rd, rs, alpha);
	gd = DoAlphaBlend(gd, gs, alpha);
	bd = DoAlphaBlend(bd, bs, alpha);
	return RGB(rd, gd, bd);
}

typedef struct
{
	FreeTypeDrawInfo* FTInfo;			//orignal draw information
	WCHAR wch;							//text to draw
	FT_BitmapGlyph FTGlyph;			//glyph
	int	AAMode;							//antialiased mode for every char
	CAlphaBlendColor* solid;
	CAlphaBlendColor* shadow;	//alpha blender
	bool bInvertColor;	// invert color for chrome/skia
} FreeTypeGlyphInfo, * PFreeTypeGlyphInfo;


// 2階調
static void FreeTypeDrawBitmapPixelModeMono(FreeTypeGlyphInfo& FTGInfo,
	CAlphaBlendColor& ab, int x, int y)
{
	CBitmapCache& cache = *FTGInfo.FTInfo->pCache;
	const FT_Bitmap* bitmap = &FTGInfo.FTGlyph->bitmap;
	BYTE alphatuner = FTGInfo.FTInfo->params->alphatuner;
	int i, j;
	int dx, dy;	// display
	FT_Bytes p;

	if (bitmap->pixel_mode != FT_PIXEL_MODE_MONO) {
		return;
	}

	const COLORREF color = RGB2DIB(FTGInfo.FTInfo->Color());

	const SIZE cachebufsize = cache.Size();
	DWORD* const cachebufp = reinterpret_cast<DWORD*>(cache.GetPixels());
	DWORD* cachebufrowp;

	int left, top, width, height;
	if (x < 0) {
		left = -x;
		x = 0;
	}
	else {
		left = 0;
	}
	width = Min(static_cast<int>(bitmap->width), static_cast<int>(cachebufsize.cx - x));
	top = 0;
	height = bitmap->rows;

	for (j = top, dy = y; j < height; ++j, ++dy) {
		if (static_cast<unsigned int>(dy) >= static_cast<unsigned int>(cachebufsize.cy)) continue;
		p = bitmap->pitch < 0 ?
			&bitmap->buffer[(-bitmap->pitch * bitmap->rows) - bitmap->pitch * j] :	// up-flow
			&bitmap->buffer[bitmap->pitch * j];	// down-flow
		cachebufrowp = &cachebufp[dy * cachebufsize.cx];
		for (i = left, dx = x; i < width; ++i, ++dx) {
			if ((p[i / 8] & (1 << (7 - (i % 8)))) != 0) {
				cachebufrowp[dx] = color;
			}
		}
	}
}

// LCD(液晶)用描画(サブピクセルレンダリング)
// RGB順(のはず)
static void FreeTypeDrawBitmapPixelModeLCD(FreeTypeGlyphInfo& FTGInfo,
	CAlphaBlendColor& ab, int x, int y)
{
	CBitmapCache& cache = *FTGInfo.FTInfo->pCache;
	const FT_Bitmap* bitmap = &FTGInfo.FTGlyph->bitmap;
	BYTE alphatuner = FTGInfo.FTInfo->params->alphatuner;
	int AAMode = FTGInfo.AAMode;
	int i, j;
	int dx, dy;	// display
	FT_Bytes p;

	if (bitmap->pixel_mode != FT_PIXEL_MODE_LCD) {
		return;
	}

	const COLORREF color = FTGInfo.FTInfo->Color();

	const SIZE cachebufsize = cache.Size();
	DWORD* const cachebufp = reinterpret_cast<DWORD*>(cache.GetPixels());
	DWORD* cachebufrowp;

	// LCDは3サブピクセル分ある
	int left, top, width, height;
	if (x < 0) {
		left = -x * 3;
		x = 0;
	}
	else {
		left = 0;
	}
	width = Min(static_cast<int>(bitmap->width), static_cast<int>(cachebufsize.cx - x) * 3);
	top = 0;
	height = bitmap->rows;
	//CAlphaBlendColor ab(color, ftdi.params->alpha, true, true);

	COLORREF backColor, newColor;
	unsigned int alphaR, alphaG, alphaB;
	BOOL bAlphaDraw = FTGInfo.FTInfo->params->alpha != 1;

	if (bAlphaDraw)
		for (j = 0, dy = y; j < height; ++j, ++dy) {
			if (static_cast<unsigned int>(dy) >= static_cast<unsigned int>(cachebufsize.cy)) continue;

			p = bitmap->pitch < 0 ?
				&bitmap->buffer[(-bitmap->pitch * bitmap->rows) - bitmap->pitch * j] :	// up-flow
				&bitmap->buffer[bitmap->pitch * j];	// down-flow

			cachebufrowp = &cachebufp[dy * cachebufsize.cx];
			for (i = left, dx = x; i < width; i += 3, ++dx) {
				backColor = cachebufrowp[dx];
				COLORREF last = 0xFFFFFFFF;
				if (AAMode == 2 || AAMode == 4) {
					// This is for displays with subpixels in RGB order
					alphaR = p[i + 0] / alphatuner;
					alphaG = p[i + 1] / alphatuner;
					alphaB = p[i + 2] / alphatuner;
				}
				else {
					// BGR
					alphaR = p[i + 2] / alphatuner;
					alphaG = p[i + 1] / alphatuner;
					alphaB = p[i + 0] / alphatuner;
				}
				newColor = ab.doAB(backColor, alphaB, alphaG, alphaR, !bAlphaDraw);
				cachebufrowp[dx] = newColor;
			}
		}
	else
		for (j = 0, dy = y; j < height; ++j, ++dy) {
			if (static_cast<unsigned int>(dy) >= static_cast<unsigned int>(cachebufsize.cy)) continue;

			p = bitmap->pitch < 0 ?
				&bitmap->buffer[(-bitmap->pitch * bitmap->rows) - bitmap->pitch * j] :	// up-flow
				&bitmap->buffer[bitmap->pitch * j];	// down-flow

			cachebufrowp = &cachebufp[dy * cachebufsize.cx];
			for (i = left, dx = x; i < width; i += 3, ++dx) {
				backColor = cachebufrowp[dx];
				COLORREF last = 0xFFFFFFFF;
				if (AAMode == 2 || AAMode == 4) {
					// これはRGBの順にサブピクセルがあるディスプレイ用
					alphaR = p[i + 0];
					alphaG = p[i + 1];
					alphaB = p[i + 2];
				}
				else {
					// BGR
					alphaR = p[i + 2];
					alphaG = p[i + 1];
					alphaB = p[i + 0];
				}
				newColor = ab.doAB(backColor, alphaB, alphaG, alphaR, !bAlphaDraw);
				cachebufrowp[dx] = newColor;
			}
		}
}

COLORREF _rgbamixer(COLORREF bkColor, int b, int g, int r, int a) {
	int bkr = GetRValue(bkColor), bkg = GetGValue(bkColor), bkb = GetBValue(bkColor);
	return a << 24 | (bkb - a * bkb / 255 + b) << 16 | (bkg - a * bkg / 255 + g) << 8 | (bkr - a * bkr / 255 + r);
}

// color blender for color font
COLORREF _invert_rgbamixer(COLORREF bkColor, int b, int g, int r, int a) {
	if (!a)
		return bkColor;
	int invertr, invertg, invertb;
	if (a == 255) {
		invertr = InvertTable[r];
		invertg = InvertTable[g];
		invertb = InvertTable[b];
	}
	else {
		invertr = InvertTable[r * 255 / a] * a / 255;
		invertg = InvertTable[g * 255 / a] * a / 255;
		invertb = InvertTable[b * 255 / a] * a / 255;
	}
	return _rgbamixer(bkColor, invertb, invertg, invertr, a);
}

// draw color emoji
static void FreeTypeDrawBitmapPixelModeBGRA(FreeTypeGlyphInfo& FTGInfo, int x, int y)
{
	CBitmapCache& cache = *FTGInfo.FTInfo->pCache;
	const FT_Bitmap* bitmap = &FTGInfo.FTGlyph->bitmap;
	BYTE alphatuner = FTGInfo.FTInfo->params->alphatuner;
	BOOL bAlphaDraw = FTGInfo.FTInfo->params->alpha != 1;
	int AAMode = FTGInfo.AAMode;
	int i, j;
	int dx, dy;	// display
	FT_Bytes p;

	if (bAlphaDraw) {	// no shadow for color font
		return;
	}

	if (bitmap->pixel_mode != FT_PIXEL_MODE_BGRA) {
		return;
	}

	const COLORREF color = FTGInfo.FTInfo->Color();

	const SIZE cachebufsize = cache.Size();
	DWORD* const cachebufp = reinterpret_cast<DWORD*>(cache.GetPixels());
	DWORD* cachebufrowp;
	typedef COLORREF(*pfnmixer) (COLORREF bkColor, int b, int g, int r, int a);

	pfnmixer mixer = FTGInfo.bInvertColor ? _invert_rgbamixer : _rgbamixer;

	int left, top, width, height;
	if (x < 0) {
		left = -x * 4;
		x = 0;
	}
	else {
		left = 0;
	}
	width = Min(static_cast<int>(bitmap->width) * 4, static_cast<int>(cachebufsize.cx - x) * 4);
	top = 0;
	height = bitmap->rows;

	COLORREF backColor, newColor;
	unsigned int alphaR, alphaG, alphaB, alpha;

	for (j = 0, dy = y; j < height; ++j, ++dy) {
		if (static_cast<unsigned int>(dy) >= static_cast<unsigned int>(cachebufsize.cy)) continue;

		p = bitmap->pitch < 0 ?
			&bitmap->buffer[(-bitmap->pitch * bitmap->rows) - bitmap->pitch * j] :	// up-flow
			&bitmap->buffer[bitmap->pitch * j];	// down-flow

		cachebufrowp = &cachebufp[dy * cachebufsize.cx];
		for (i = left, dx = x; i < width; i += 4, ++dx) {
			backColor = cachebufrowp[dx];
			COLORREF last = 0xFFFFFFFF;
			// always RGB
			alphaR = p[i + 0];
			alphaG = p[i + 1];
			alphaB = p[i + 2];
			alpha = p[i + 3];
			newColor = mixer(backColor, alphaB, alphaG, alphaR, alpha);
			cachebufrowp[dx] = newColor;
		}
	}
}

static void FreeTypeDrawBitmapGray(FreeTypeGlyphInfo& FTGInfo, CAlphaBlendColor& ab, int x, int y)
{
	int i, j;
	int dx, dy;	// display
	COLORREF c;
	FT_Bytes p;

	CBitmapCache& cache = *FTGInfo.FTInfo->pCache;
	const FT_Bitmap* bitmap = &FTGInfo.FTGlyph->bitmap;
	BYTE alphatuner = FTGInfo.FTInfo->params->alphatuner;

	BOOL bAlphaDraw = FTGInfo.FTInfo->params->alpha != 1;
	const COLORREF color = FTGInfo.FTInfo->Color();
	const SIZE cachebufsize = cache.Size();
	DWORD* const cachebufp = reinterpret_cast<DWORD*>(cache.GetPixels());
	DWORD* cachebufrowp;

	int left, top, width, height;
	if (x < 0) {
		left = -x;
		x = 0;
	}
	else {
		left = 0;
	}
	width = Min(static_cast<int>(bitmap->width), static_cast<int>(cachebufsize.cx - x));
	top = 0;
	height = bitmap->rows;

	//	CAlphaBlendColor ab(color, ftdi.params->alpha, false, true);

	COLORREF backColor;
	int alpha;

	for (j = top, dy = y; j < height; ++j, ++dy) {
		if (static_cast<unsigned int>(dy) >= static_cast<unsigned int>(cachebufsize.cy)) continue;
		p = bitmap->pitch < 0 ?
			&bitmap->buffer[(-bitmap->pitch * bitmap->rows) - bitmap->pitch * j] :	// up-flow
			&bitmap->buffer[bitmap->pitch * j];	// down-flow
		cachebufrowp = &cachebufp[dy * cachebufsize.cx];
		for (i = left, dx = x; i < width; ++i, ++dx) {
			alpha = p[i];
			backColor = cachebufrowp[dx];
			c = ab.doAB(backColor, alpha, !bAlphaDraw);
			cachebufrowp[dx] = c;
		}
	}
}

// グリフビットマップのレンダリング
static bool FreeTypeDrawBitmap(
	FreeTypeGlyphInfo& FTGInfo,
	CAlphaBlendColor& ab,
	int x, int y)
{
	if (FTGInfo.FTGlyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
		// この関数自体はFT_PIXEL_MODE_GRAYにのみ対応し他に委譲する
		switch (FTGInfo.FTGlyph->bitmap.pixel_mode) {
		case FT_PIXEL_MODE_MONO:
			FreeTypeDrawBitmapPixelModeMono(FTGInfo, ab, x, y);
			break;
		case FT_PIXEL_MODE_LCD:
			FreeTypeDrawBitmapPixelModeLCD(FTGInfo, ab, x, y);
			break;
		case FT_PIXEL_MODE_BGRA:
			FreeTypeDrawBitmapPixelModeBGRA(FTGInfo, x, y);
			break;
		default:
			return false;		// 未対応
		}
		return true;
	}
	FreeTypeDrawBitmapGray(FTGInfo, ab, x, y);
	return true;
}

// 縦書き用のレンダリング(コピペ手抜き)
// 2階調
static void FreeTypeDrawBitmapPixelModeMonoV(FreeTypeGlyphInfo& FTGInfo,
	CAlphaBlendColor& ab, int x, int y)
{
	CBitmapCache& cache = *FTGInfo.FTInfo->pCache;
	FT_Bitmap* bitmap = &FTGInfo.FTGlyph->bitmap;
	int i, j;
	int dx, dy;	// display
	FT_Bytes p;

	if (bitmap->pixel_mode != FT_PIXEL_MODE_MONO) {
		return;
	}

	const COLORREF color = FTGInfo.FTInfo->Color();

	const int width = bitmap->width;
	const int height = bitmap->rows;

	for (j = 0, dy = x; j < height; ++j, ++dy) {
		p = bitmap->pitch < 0 ?
			&bitmap->buffer[(-bitmap->pitch * bitmap->rows) - bitmap->pitch * j] :	// up-flow
			&bitmap->buffer[bitmap->pitch * j];	// down-flow
		for (i = 0, dx = y + width; i < width; ++i, --dx) {
			if ((p[i / 8] & (1 << (7 - (i % 8)))) != 0) {
				if (cache.GetPixel(dx, dy) != CLR_INVALID) { // dx dy エラーチェック
					cache.SetCurrentPixel(color);
				}
			}
		}
	}
}

// LCD(液晶)用描画(サブピクセルレンダリング)
// RGB順(のはず)
static void FreeTypeDrawBitmapPixelModeLCDV(FreeTypeGlyphInfo& FTGInfo,
	CAlphaBlendColor& ab, int x, int y)
{
	CBitmapCache& cache = *FTGInfo.FTInfo->pCache;
	const FT_Bitmap* bitmap = &FTGInfo.FTGlyph->bitmap;
	BYTE alphatuner = FTGInfo.FTInfo->params->alphatuner;
	int AAMode = FTGInfo.AAMode;
	int i, j;
	int dx, dy;	// display
	COLORREF c;
	FT_Bytes p;

	if (bitmap->pixel_mode != FT_PIXEL_MODE_LCD_V) {
		return;
	}

	const COLORREF color = FTGInfo.FTInfo->Color();

	// LCDは3サブピクセル分ある
	const int width = bitmap->width;
	const int height = bitmap->rows;
	const int pitch = bitmap->pitch;
	const int pitchabs = pitch < 0 ? -pitch : pitch;
	BOOL bAlphaDraw = FTGInfo.FTInfo->params->alpha != 1;
	//CAlphaBlendColor ab(color, ftdi.params->alpha, true);

	if (bAlphaDraw)
		for (j = 0, dy = x; j < height; j += 3, ++dy) {
			p = pitch < 0 ?
				&bitmap->buffer[(pitchabs * bitmap->rows) + pitchabs * j] :	// up-flow
				&bitmap->buffer[pitchabs * j];	// down-flow

			int alphaR, alphaG, alphaB;
			for (i = 0, dx = y + width; i < width; ++i, --dx) {
				COLORREF backColor = cache.GetPixel(dy, dx);

				if (backColor == color || backColor == CLR_INVALID) continue;
				if (AAMode == 2 || AAMode == 4) {
					// これはRGBの順にサブピクセルがあるディスプレイ用
					alphaR = p[i + 0] / alphatuner;
					alphaG = p[i + pitch] / alphatuner;
					alphaB = p[i + pitch * 2] / alphatuner;
				}
				else {
					// BGR
					alphaR = p[i + pitch * 2] / alphatuner;
					alphaG = p[i + pitch] / alphatuner;
					alphaB = p[i + 0] / alphatuner;
				}

				c = ab.doAB(backColor, alphaR, alphaG, alphaB, !bAlphaDraw);
				cache.SetCurrentPixel(c);
			}

			if (i >= width)
				continue;
		}
	else
		for (j = 0, dy = x; j < height; j += 3, ++dy) {
			p = pitch < 0 ?
				&bitmap->buffer[(pitchabs * bitmap->rows) + pitchabs * j] :	// up-flow
				&bitmap->buffer[pitchabs * j];	// down-flow

			int alphaR, alphaG, alphaB;
			for (i = 0, dx = y + width; i < width; ++i, --dx) {
				COLORREF backColor = cache.GetPixel(dy, dx);

				if (backColor == color || backColor == CLR_INVALID) continue;
				if (AAMode == 2 || AAMode == 4) {
					// これはRGBの順にサブピクセルがあるディスプレイ用
					alphaR = p[i + 0];
					alphaG = p[i + pitch];
					alphaB = p[i + pitch * 2];
				}
				else {
					// BGR
					alphaR = p[i + pitch * 2];
					alphaG = p[i + pitch];
					alphaB = p[i + 0];
				}

				c = ab.doAB(backColor, alphaR, alphaG, alphaB, !bAlphaDraw);
				cache.SetCurrentPixel(c);
			}

			if (i >= width)
				continue;
		}
}

void FreeTypeDrawBitmapGrayV(FreeTypeGlyphInfo& FTGInfo, CAlphaBlendColor& ab, int x, int y)
{
	CBitmapCache& cache = *FTGInfo.FTInfo->pCache;
	const FT_Bitmap* bitmap = &FTGInfo.FTGlyph->bitmap;
	BYTE alphatuner = FTGInfo.FTInfo->params->alphatuner;
	int i, j;
	int dx, dy;	// display
	int width, height;
	COLORREF c;
	FT_Bytes p;


	const COLORREF color = FTGInfo.FTInfo->Color();
	//const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	//const int* table = pSettings->GetTuneTable();
	width = bitmap->width;
	height = bitmap->rows;

	//	CAlphaBlendColor ab(color, ftdi.params->alpha, false);

	for (j = 0, dy = x; j < height; ++j, ++dy) {
		p = bitmap->pitch < 0 ?
			&bitmap->buffer[(-bitmap->pitch * bitmap->rows) - bitmap->pitch * j] :	// up-flow
			&bitmap->buffer[bitmap->pitch * j];	// down-flow
		for (i = 0, dx = y + width; i < width; ++i, --dx) {
			const COLORREF backColor = cache.GetPixel(dy, dx);
			if (backColor == color || backColor == CLR_INVALID) continue;
			c = ab.doAB(backColor, p[i], true);
			cache.SetPixelV(dy, dx, c);
		}
	}
}

static bool FreeTypeDrawBitmapV(FreeTypeGlyphInfo& FTGInfo, CAlphaBlendColor& ab, const int x, const int y)
{
	if (FTGInfo.FTGlyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
		// この関数自体はFT_PIXEL_MODE_GRAYにのみ対応し他に委譲する
		switch (FTGInfo.FTGlyph->bitmap.pixel_mode) {
		case FT_PIXEL_MODE_MONO:
			FreeTypeDrawBitmapPixelModeMonoV(FTGInfo, ab, x, y);
			break;
		case FT_PIXEL_MODE_LCD_V:
			FreeTypeDrawBitmapPixelModeLCDV(FTGInfo, ab, x, y);
			break;
		case FT_PIXEL_MODE_BGRA:
			FreeTypeDrawBitmapPixelModeBGRA(FTGInfo, x, y);
			break;
		default:
			return false;		// 未対応
		}
		return true;
	}
	FreeTypeDrawBitmapGrayV(FTGInfo, ab, x, y);
	return true;
}

class CGGOGlyphLoader
{
private:
	FT_Library m_lib;
	const FT_Glyph_Class* m_clazz;
	BYTE bgtbl[0x41];
	static int CALLBACK EnumFontFamProc(const LOGFONT* lplf, const TEXTMETRIC* lptm, DWORD FontType, LPARAM lParam);
public:
	CGGOGlyphLoader() : m_lib(nullptr), m_clazz(nullptr), bgtbl{} {}
	~CGGOGlyphLoader() {}
	bool init(FT_Library freetype_library);
	FT_Library getlib() { return m_lib; }
	const FT_Glyph_Class* getclazz() { return m_clazz; }
	BYTE convbgpixel(BYTE val) { return bgtbl[val]; }
};
static CGGOGlyphLoader s_GGOGlyphLoader;

int CALLBACK CGGOGlyphLoader::EnumFontFamProc(const LOGFONT* lplf, const TEXTMETRIC* lptm, DWORD FontType, LPARAM lParam)
{
	CGGOGlyphLoader* pThis = reinterpret_cast<CGGOGlyphLoader*>(lParam);
	if (FontType != TRUETYPE_FONTTYPE || lplf->lfCharSet == SYMBOL_CHARSET) {
		return TRUE;
	}

	TRACE(_T("Face: %s\n"), lplf->lfFaceName);
	FreeTypeSysFontData* pFont = FreeTypeSysFontData::CreateInstance(lplf->lfFaceName, 0, false);
	if (!pFont) {
		return TRUE;
	}

	const FT_Glyph_Class* clazz = nullptr;
	renderer_raii::UniqueFreeTypeFace face(pFont->GetFace());
	FT_Error err = FT_Set_Pixel_Sizes(face.get(), 0, 12);//optimized
	if (!err) {
		err = FT_Load_Char(face.get(), lptm->tmDefaultChar, FT_LOAD_NO_BITMAP);
		if (!err) {
			FT_Glyph rawGlyph = nullptr;
			err = FT_Get_Glyph(face->glyph, &rawGlyph);
			if (!err) {
				renderer_raii::UniqueFreeTypeGlyph glyph(rawGlyph);
				if (glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
					clazz = glyph->clazz;
				}
			}
		}
	}

	if (clazz) {
		pThis->m_clazz = clazz;
		//列挙中止
		return FALSE;
	}
	return TRUE;
}

bool
CGGOGlyphLoader::init(FT_Library freetype_library)
{
	if (m_lib) {
		return true;
	}

	if (!freetype_library) {
		return false;
	}

	for (BYTE val = 0; val <= 0x40; ++val) {
		BYTE t = static_cast<BYTE>((static_cast<DWORD>(val) * 256) / 65);
		bgtbl[val] = t + (t >> 6);
	}

	m_lib = freetype_library;
	m_clazz = nullptr;

	//前の方法だと、arial.ttfが無いとまずそうなので
	//適当に使えるアウトラインフォントを探す
	renderer_raii::UniqueDeviceContext hdc(CreateCompatibleDC(nullptr));
	if (hdc) {
		EnumFontFamilies(hdc.get(), nullptr, EnumFontFamProc, reinterpret_cast<LPARAM>(this));
	}

	if (m_clazz != nullptr) {
		return true;
	}
	m_lib = nullptr;
	return false;
}

class CGGOOutlineGlyph
{
private:
	FT_OutlineGlyph m_ptr;
	static FT_F26Dot6 toF26Dot6(const FIXED& fx) {
		LONG raw = 0;
		static_assert(sizeof(raw) == sizeof(fx), "FIXED representation changed");
		memcpy(&raw, &fx, sizeof(raw));
		return raw >> 10;
	}
	static FT_Fixed toFixed(const short n) {
		return static_cast<FT_Fixed>(n) << 16;
	}
	static char getTag(char tag, const FT_Vector& point) {
		if ((point.x & 0x0f) != 0) {
			tag |= FT_CURVE_TAG_TOUCH_X;
		}
		if ((point.y & 0x0f) != 0) {
			tag |= FT_CURVE_TAG_TOUCH_Y;
		}
		return tag;
	}
public:
	CGGOOutlineGlyph() : m_ptr(nullptr) { _ASSERTE(s_GGOGlyphLoader.getlib()); }
	~CGGOOutlineGlyph() { done(); };
	bool init(DWORD bufsize, PVOID bufp, const GLYPHMETRICS& gm);
	void done();
	operator FT_Glyph () { return reinterpret_cast<FT_Glyph>(m_ptr); }
};

void
CGGOOutlineGlyph::done()
{
	if (m_ptr) {
		free(m_ptr->outline.points);
		free(m_ptr->outline.tags);
		free(m_ptr->outline.contours);
	}
	free(m_ptr);
	m_ptr = nullptr;
}

bool
CGGOOutlineGlyph::init(DWORD bufsize, PVOID bufp, const GLYPHMETRICS& gm)
{
	done();
	m_ptr = static_cast<FT_OutlineGlyph>(calloc(1, sizeof * m_ptr));
	if (!m_ptr) {
		return false;
	}

	FT_GlyphRec& root = m_ptr->root;
	FT_Outline& outline = m_ptr->outline;

	root.library = s_GGOGlyphLoader.getlib();
	root.clazz = s_GGOGlyphLoader.getclazz();
	root.format = FT_GLYPH_FORMAT_OUTLINE;
	root.advance.x = toFixed(gm.gmCellIncX);
	root.advance.y = toFixed(gm.gmCellIncY);

	outline.n_contours = 0;
	outline.n_points = 0;
	outline.flags = 0; //FT_OUTLINE_HIGH_PRECISION;

	LPTTPOLYGONHEADER ttphp = static_cast<LPTTPOLYGONHEADER>(bufp);
	LPTTPOLYGONHEADER ttphpend = reinterpret_cast<LPTTPOLYGONHEADER>(reinterpret_cast<PBYTE>(ttphp) + bufsize);

	while (ttphp < ttphpend) {
		LPTTPOLYCURVE ttpcp = reinterpret_cast<LPTTPOLYCURVE>(ttphp + 1);
		LPTTPOLYCURVE ttpcpend = reinterpret_cast<LPTTPOLYCURVE>(reinterpret_cast<PBYTE>(ttphp) + ttphp->cb);
		if (reinterpret_cast<PBYTE>(ttpcpend) > reinterpret_cast<PBYTE>(ttphpend)) {
			break;
		}
		++outline.n_points;
		++outline.n_contours;
		while (ttpcp < ttpcpend) {
			LPPOINTFX pfxp = &ttpcp->apfx[0];
			outline.n_points += ttpcp->cpfx;
			ttpcp = reinterpret_cast<LPTTPOLYCURVE>(pfxp + ttpcp->cpfx);
		}
		ttphp = reinterpret_cast<LPTTPOLYGONHEADER>(ttpcp);
	}

	if (ttphp != ttphpend) {
		return false;
	}
	outline.points = static_cast<FT_Vector*>(calloc(outline.n_points, sizeof * outline.points));
	outline.tags = static_cast<unsigned char*>(calloc(outline.n_points, sizeof * outline.tags));
	outline.contours = static_cast<unsigned short*>(calloc(outline.n_contours, sizeof * outline.contours));
	if (!outline.points || !outline.tags || !outline.contours) {
		done();
		return false;
	}

	unsigned short* cp = outline.contours;
	short ppos = 0;

	ttphp = static_cast<LPTTPOLYGONHEADER>(bufp);
	while (ttphp < ttphpend) {
		LPTTPOLYCURVE ttpcp = reinterpret_cast<LPTTPOLYCURVE>(ttphp + 1);
		LPTTPOLYCURVE ttpcpend = reinterpret_cast<LPTTPOLYCURVE>(reinterpret_cast<PBYTE>(ttphp) + ttphp->cb);

		LPPOINTFX pfxp0 = &ttpcp->apfx[0];
		while (ttpcp < ttpcpend) {
			LPPOINTFX pfxp = &ttpcp->apfx[0];
			pfxp0 = pfxp + (ttpcp->cpfx - 1);
			ttpcp = reinterpret_cast<LPTTPOLYCURVE>(pfxp + ttpcp->cpfx);
		}
		ttpcp = reinterpret_cast<LPTTPOLYCURVE>(ttphp + 1);

		if (pfxp0->x.value != ttphp->pfxStart.x.value || pfxp0->x.fract != ttphp->pfxStart.x.fract ||
			pfxp0->y.value != ttphp->pfxStart.y.value || pfxp0->y.fract != ttphp->pfxStart.y.fract) {
			outline.points[ppos].x = toF26Dot6(ttphp->pfxStart.x);
			outline.points[ppos].y = toF26Dot6(ttphp->pfxStart.y);
			outline.tags[ppos] = getTag(FT_CURVE_TAG_ON, outline.points[ppos]);
			++ppos;
		}
		while (ttpcp < ttpcpend) {
			char tag;
			switch (ttpcp->wType) {
			case TT_PRIM_LINE:
				tag = FT_CURVE_TAG_ON;
				break;
			case TT_PRIM_QSPLINE:
				tag = FT_CURVE_TAG_CONIC;
				break;
			case TT_PRIM_CSPLINE:
				tag = FT_CURVE_TAG_CONIC;
				break;
			default:
				tag = 0;
			}

			LPPOINTFX pfxp = &ttpcp->apfx[0];
			for (WORD cnt = 0; cnt < ttpcp->cpfx; ++cnt) {
				outline.points[ppos].x = toF26Dot6(pfxp->x);
				outline.points[ppos].y = toF26Dot6(pfxp->y);
				outline.tags[ppos] = tag;
				++ppos;
				++pfxp;
			}
			outline.tags[ppos - 1] = getTag(FT_CURVE_TAG_ON, outline.points[ppos - 1]);
			ttpcp = reinterpret_cast<LPTTPOLYCURVE>(pfxp);
		}
		*cp++ = ppos - 1;
		ttphp = reinterpret_cast<LPTTPOLYGONHEADER>(ttpcp);
	}
	outline.n_points = ppos;
	return true;
}

template<typename T>
class CTempMem
{
private:
	char m_localbuf[0x0f80];
	DWORD m_size;
	T m_ptr;
public:
	CTempMem() : m_localbuf{}, m_size(sizeof m_localbuf), m_ptr(reinterpret_cast<T>(m_localbuf)) {
	}
	CTempMem(const CTempMem&) = delete;
	CTempMem& operator=(const CTempMem&) = delete;
	~CTempMem() {
		done();
	}
	T init(DWORD size) {
		done();
		if (size > m_size) {
			m_ptr = static_cast<T>(malloc(size));
			m_size = m_ptr ? size : 0;
		}
		return m_ptr;
	}
	void done() {
		if (m_ptr != reinterpret_cast<T>(m_localbuf)) {
			free(m_ptr);
		}
		m_size = sizeof m_localbuf;
		m_ptr = reinterpret_cast<T>(m_localbuf);
	}
	operator T () { return m_ptr; }
	bool operator ! () { return !m_ptr; }
	DWORD getsize() { return m_size; }
};

bool IsSFNTNameMatch(const FT_Face& face, FT_UInt nameID, wstring name) {
	// get sfnt name by platform id
	auto getSfntName = [](FT_SfntName& fn)->wstring {
		switch (fn.platform_id) {
		case 0: {
			return wstring(reinterpret_cast<wchar_t*>(fn.string), fn.string_len);
		}
		case 1: {
			//	macintosh
			return to_wide_string(string(reinterpret_cast<char*>(fn.string), fn.string_len));
		}
		case 3: {
			// Windows
			switch (fn.encoding_id) {
			case 0:
			case 1: {
				return to_utf16le(wstring(reinterpret_cast<wchar_t*>(fn.string), fn.string_len));
			}
			default:
				return to_wide_string(string(reinterpret_cast<char*>(fn.string), fn.string_len));
			}
		}
		}
		return L"";
		};

	auto getWordSet = [](const wstring& str) {
		set<wstring> words;
		wstringstream ss(to_lower_case(str));
		wstring word;
		while (ss >> word) {
			if (word != L"regular")	// drop the regular style as it can be omitted
				words.insert(word);
		}
		return words;
		};

	auto compareStyle = [&](const wstring nameA, const wstring nameB)->bool {
		auto setA = getWordSet(nameA);
		auto setB = getWordSet(nameB);
		return setA == setB;
		};

	FT_SfntName sfntName;
	FT_UInt nameCount = FT_Get_Sfnt_Name_Count(face);
	for (FT_UInt i = 0; i < nameCount; ++i) {
		if (FT_Get_Sfnt_Name(face, i, &sfntName) == 0) {
			if (sfntName.name_id == nameID) {
				if (sfntName.string_len > 0 && sfntName.string) {
					wstring fontName = getSfntName(sfntName);
					if (compareStyle(fontName, name)) {
						return true;
					}
				}
			}
		}
	}
	return false;
}

void VarFontByAlias(const FT_Face& face, const wstring& lfStyleName) {
	FT_MM_Var* rawMmVar = nullptr;
	auto error = FT_Get_MM_Var(face, &rawMmVar);
	if (error) {
		return;	// not a varaible font
	}
	auto mmVar = renderer_raii::AdoptMmVar(freetype_library, rawMmVar);
	FT_MM_Var* mm_var = static_cast<FT_MM_Var*>(mmVar.get());
	if (!mm_var->num_namedstyles) return;	// no named instance, nothing to adjust.

	for (unsigned int i = 0; i < mm_var->num_namedstyles; ++i) {
		auto& style = mm_var->namedstyle[i];
		if (IsSFNTNameMatch(face, style.strid, lfStyleName)) {
			// found it
			FT_Set_Named_Instance(face, i+1);
			break;
		}
	}
}

BOOL FreeTypePrepare(FreeTypeDrawInfo& FTInfo)
{
	//CDebugElapsedCounter cntr("FreeTypePrepare");
#ifdef _DEBUG
	FTInfo.Validate();
#endif

	FT_Face& freetype_face = FTInfo.freetype_face;
	FT_Int& cmap_index = FTInfo.cmap_index;
	FT_Render_Mode& render_mode = FTInfo.render_mode;
	FTC_ImageTypeRec& font_type = FTInfo.font_type;
	FreeTypeFontInfo*& pfi = FTInfo.pfi;
	const CFontSettings*& pfs = FTInfo.pfs;
	FreeTypeFontCache*& pftCache = FTInfo.pftCache;
	FTC_ScalerRec& scaler = FTInfo.scaler;
	TEXTMETRIC& tm = FTInfo.params->otm->otmTextMetrics;

	FTC_FaceID face_id = nullptr;
	int height = 0;

	const LOGFONTW& lf = FTInfo.LogFont();
	render_mode = FT_RENDER_MODE_NORMAL;
	if (FTInfo.params->alpha < 1)
		FTInfo.params->alpha = 1;

	if (!*lf.lfFaceName)
		return FALSE;	//optimized
	FTInfo.face_id_list_num = 0;
	pfi = nullptr;
	CGdippSettings* pSettings = CGdippSettings::GetInstance();
	const bool bVertical = FTInfo.rasterPolicy.fontLoader == SETTING_FONTLOADER_FREETYPE ? lf.lfFaceName[0] == _T('@') : false;

	FreeTypeFontInfo* pfitemp = g_pFTEngine->FindFont(FTInfo.params);
	if (pfitemp) {
		if (!pfi) pfi = pfitemp;
		FTInfo.face_id_list_num = pfi->GetFTLink(&FTInfo.face_id_list);
		pfi->GetGGOLink(&FTInfo.ggo_font_list);
		FTInfo.face_id_simsun = pfi->GetSimSunID();
	}
	else
		return FALSE;
	if (!(freetype_face = FTInfo.GetFace(0)))
	{
		pSettings->AddFontExclude(lf.lfFaceName);
		return FALSE;
	}

	if (!pfi) {
		return FALSE;
	}

	FTInfo.params->lplf->lfWeight = FTInfo.params->otm->otmTextMetrics.tmWeight;	//更新到标准weight
	pfs = &pfi->GetFontSettings();

	cmap_index = -1;
	switch (FTInfo.rasterPolicy.fontLoader) {
	case SETTING_FONTLOADER_FREETYPE:
	{
		face_id = reinterpret_cast<FTC_FaceID>(pfi->GetId());

		scaler.face_id = face_id;

		height = FTInfo.params->otm->otmTextMetrics.tmHeight - FTInfo.params->otm->otmTextMetrics.tmInternalLeading;	//Snowie!!剪掉空白高度，bugfix。
		scaler.height = height;
		//Snowie!!
		TT_OS2* os2_table = pfitemp->GetOS2Table();

		if (lf.lfQuality && os2_table->xAvgCharWidth)
		{
			if (!(freetype_face->style_flags & FT_STYLE_FLAG_BOLD) && tm.tmWeight >= FW_BOLD)
				--FTInfo.params->otm->otmTextMetrics.tmAveCharWidth;
			scaler.width = MulDiv(FTInfo.params->otm->otmTextMetrics.tmAveCharWidth, FTInfo.params->otm->otmEMSquare, os2_table->xAvgCharWidth);
		}
		else
			scaler.width = scaler.height;
		if (bVertical)
			swap(scaler.width, scaler.height);//如果是竖向字体，交换宽高
		//!!Snowie
		scaler.pixel = 1;
		scaler.x_res = 0;
		scaler.y_res = 0;
		height = scaler.height;
		break;
	}
	case SETTING_FONTLOADER_WIN32:
	{
		height = -lf.lfHeight;
		scaler.height = height;
		scaler.width = lf.lfWidth;
	}
	break;
	default:
		return FALSE;
	}

	// fetch face again to get the correct one.
	if (!(freetype_face = FTInfo.GetFace(0)))
	{
		pSettings->AddFontExclude(lf.lfFaceName);
		return FALSE;
	}

	pftCache = pfi->GetCache(scaler, lf);
	if (!pftCache)
		return FALSE;

	switch (FTInfo.rasterPolicy.fontLoader) {
	case SETTING_FONTLOADER_FREETYPE:
		// font_typeを設定
		font_type.face_id = face_id;
		font_type.width = scaler.width;//freetype_face->size->metrics.x_ppem;
		font_type.height = scaler.height;//freetype_face->size->metrics.y_ppem;
		//Snowie!!
		FTInfo.height = font_type.height;
		FTInfo.width = font_type.width;

		break;
	case SETTING_FONTLOADER_WIN32:
		font_type.face_id = face_id;
		font_type.width = -1;
		font_type.height = -1;
		break;

		DEFAULT_UNREACHABLE;
	}
	font_type.flags = FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH;

	// ヒンティング
	switch (pfs->GetHintingMode()) {
	case 0:
		// ignore.
		break;
	case 1:
		font_type.flags |= FT_LOAD_NO_HINTING;
		break;
	case 2:
		font_type.flags |= FT_LOAD_FORCE_AUTOHINT;
		break;
	}

	//如果含有内置hinting则启用default模式，否则使用autohint模式，以保证效果
	// アンチエイリアス
	if (FTInfo.IsMono()) {
		font_type.flags |= FT_LOAD_TARGET_MONO;
		render_mode = FT_RENDER_MODE_MONO;
	}
	else {
		switch (pfs->GetAntiAliasMode()) {
		case -1:
			font_type.flags |= FT_LOAD_TARGET_MONO;
			render_mode = FT_RENDER_MODE_MONO;
			break;
		case 0:
			font_type.flags |= FT_LOAD_TARGET_NORMAL;
			render_mode = FT_RENDER_MODE_NORMAL;
			break;
		case 1:
			font_type.flags |= FT_LOAD_TARGET_LIGHT;
			render_mode = FT_RENDER_MODE_LIGHT;
			break;
		case 2:
		case 3:
			font_type.flags |= FT_LOAD_TARGET_LCD;
			render_mode = FT_RENDER_MODE_LCD;
			break;
		case 4:
		case 5:
			font_type.flags |= FT_LOAD_TARGET_LIGHT;
			render_mode = FT_RENDER_MODE_LCD;
			break;
		}
	}

	if (FTInfo.rasterPolicy.hintSmallFont && font_type.height != -1 && font_type.height < 12)  //通用设置不使用hinting，但是打开了小字体hinting开关
	{
		font_type.flags &= ~FT_LOAD_NO_HINTING;
	}

	FTInfo.useKerning = FALSE;
	if (pfs->GetKerning()) {
		switch (FTInfo.rasterPolicy.fontLoader) {
		case SETTING_FONTLOADER_FREETYPE:
			FTInfo.useKerning = !!FT_HAS_KERNING(freetype_face);
			break;
		case SETTING_FONTLOADER_WIN32:
		{
			DWORD rc = GetFontLanguageInfo(FTInfo.hdc);
			if (rc != GCP_ERROR) {
				FTInfo.useKerning = !!(rc & GCP_USEKERNING);
				FTInfo.ggokerning.init(FTInfo.hdc);
			}
		}
		break;

		DEFAULT_UNREACHABLE;
		}
	}
	return TRUE;
}

// 縦にするやつはtrue(ASCIIと半角カナはfalse)
inline bool IsVerticalChar(WCHAR wch) {
	if (wch < 0x80)
		return false;
	if (0xFF61 <= wch && wch <= 0xFF9F)
		return false;
	// 本当はもっと真面目にやらないとまずいが。
	return true;
}

struct CGGOFont
{
	HDC m_hdc;
	renderer_raii::UniqueFont m_hfont;
	renderer_raii::SelectedFont m_selection;
	CGGOFont(HDC hdc, const LOGFONT& olf) : m_hdc(hdc) {
		LOGFONT lf = olf;
		lf.lfWeight = FW_REGULAR;
		lf.lfItalic = FALSE;
		lf.lfStrikeOut = FALSE;
		m_hfont.reset(CreateFontIndirect(&lf));
	}
	~CGGOFont() = default;
	void change() {
		m_selection = renderer_raii::SelectObject(m_hdc, m_hfont.get());
	}
	void restore() {
		m_selection.reset();
	}
	operator HFONT () { return m_hfont.get(); }
};

class ClpDx
{
private:
	const INT* p;
	const INT step;
public:
	ClpDx(const INT* lpDx, UINT etoOptions) : p(lpDx), step((etoOptions& ETO_PDY) ? 2 : 1) {
	}
	~ClpDx() {
	}
	int get(int val) {
		int result;
		if (p) {
			result = *p;
			p += step;
		}
		else {
			result = val;
		}
		return result;
	}
	int gety(int val) {	// you must call gety BEFORE call get, gety won't move the pointer, thus has no side effect
		int result;
		if (step == 1) return val;	//only search for values in ETO_PDY mode.
		if (p) {
			result = *(p + 1);
		}
		else {
			result = val;
		}
		return result;
	}
};


BOOL ForEachGetGlyphFT(FreeTypeDrawInfo& FTInfo, LPCTSTR lpString, int cbString, FT_Referenced_Glyph* GlyphArray, FT_DRAW_STATE* drState)
{
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	//Snowie!!
	BOOL bIsSymbol = GetTextCharsetInfo(FTInfo.hdc, nullptr, 0) == SYMBOL_CHARSET;
	BOOL bAllowDefaultLink = pSettings->GetFontLinkInfo().IsAllowFontLink(static_cast<BYTE>(GetTextCharsetInfo(FTInfo.hdc, nullptr, 0)));	//是否为符号
	BOOL nRet = true;
	BOOL bWindowsLink = FTInfo.rasterPolicy.fontLinkMode == 2;
	//!!Snowie

	FT_Face freetype_face = FTInfo.freetype_face;
	const FT_Int cmap_index = FTInfo.cmap_index;
	const FT_Bool useKerning = FTInfo.useKerning;
	FT_Render_Mode render_mode = FTInfo.render_mode;
	const int LinkNum = FTInfo.face_id_list_num;
	int AAMode = FTInfo.pfs->GetAntiAliasMode();
	// fix AAMode to LCD if harmony lcd is enabled. This is will not affect directwrite output.
	if (AAMode > 2 && FTInfo.rasterPolicy.harmonyLcd) {
		AAMode = 2;
	}
	int* AAList = FTInfo.AAModes;
	const LOGFONTW& lf = FTInfo.LogFont();
	FreeTypeFontCache* pftCache = FTInfo.pftCache;
	const CFontSettings*& pfs = FTInfo.pfs;
	FreeTypeFontInfo*& pfi = FTInfo.pfi;
	const bool bLoadColor = FTInfo.rasterPolicy.loadColorFont;
	const bool bGlyphIndex = FTInfo.IsGlyphIndex();
	const LPCTSTR lpStart = lpString;
	const LPCTSTR lpEnd = lpString + cbString;
	FT_UInt previous = 0;
	WCHAR previouswch = 0;
	const bool bVertical = lf.lfFaceName[0] == _T('@');
	bool bLcdMode = render_mode == FT_RENDER_MODE_LCD;
	bool bLightLcdMode = (AAMode == 4) || (AAMode == 5);
	ClpDx clpdx(FTInfo.lpDx, FTInfo.params->etoOptions);
	const bool bWidthGDI32 = true;
	const int ggoformatbase = (FTInfo.font_type.flags & FT_LOAD_NO_HINTING) ? GGO_UNHINTED | GGO_NATIVE : GGO_NATIVE;

	if (!s_GGOGlyphLoader.init(freetype_library)) {
		return FALSE;
	}


	std::vector<WORD> glyphIndices(static_cast<size_t>(cbString));
	WORD* gi = glyphIndices.data();


	//Snowie!!

	//Fast fontlink
	std::vector<std::vector<WORD>> fontLinks;
	HFONT hOldFont = nullptr;
	if (!bGlyphIndex && bWindowsLink)	//使用Windows fontlink
	{
		fontLinks.assign(static_cast<size_t>(LinkNum),
			std::vector<WORD>(static_cast<size_t>(cbString), 0));
		//
		hOldFont = reinterpret_cast<HFONT>(GetCurrentObject(FTInfo.hdc, OBJ_FONT));	//加载第一个字体
	}
	//fontlink

	int* Dx = FTInfo.Dx;
	int* Dy = FTInfo.Dy;
	if (!bAllowDefaultLink && FTInfo.face_id_list_num > 1)
		FTInfo.face_id_list_num--;	//如果是symbol页那就不链接到宋体

	bool bUnicodePlane = false;
	for (int i = 0; lpString < lpEnd; ++lpString, ++gi, ++GlyphArray, ++drState, ++AAList, /*ggdi32++,*/ i++) {
		WCHAR wch = *lpString;
		if (bUnicodePlane)
		{
			*drState = FT_DRAW_NOTFOUND;
			bUnicodePlane = false;
			if (lpString < lpEnd - 1) {
				FTInfo.y -= clpdx.gety(0);
				FTInfo.x += clpdx.get(0);
			}
			else
			{
				int gdi32x = 0;
				GetCharWidth32W(FTInfo.hdc, wch, wch, &gdi32x);
				FTInfo.y -= clpdx.gety(0);
				FTInfo.x += clpdx.get(gdi32x);
				FTInfo.px = FTInfo.x;
			}
			goto cont;
		}
		if (!bGlyphIndex && bIsSymbol && !bWindowsLink)
			wch |= 0xF000;
		FT_Referenced_Glyph* glyph_bitmap = GlyphArray;
		int gdi32x = 0;// = *ggdi32;
		FTInfo.font_type.face_id = FTInfo.face_id_list[0];
		FreeTypeCharData* chData = nullptr;
		FT_UInt glyph_index = 0;
		BOOL bIsBold = false, bIsIndivBold = false;

		{

			chData = bGlyphIndex
				? pftCache->FindGlyphIndex(wch)
				: pftCache->FindChar(wch);	//looking for wch in char cache and glyph cache

			if (chData/* && FTInfo.width==chData->GetWidth()*/) {	// found cache

				gdi32x = chData->GetGDIWidth();
				*AAList = chData->GetAAMode();
				CCriticalSectionLock __lock(CCriticalSectionLock::CS_LIBRARY);
				FT_Glyph_Ref_Copy(reinterpret_cast<FT_Referenced_Glyph>(chData->GetGlyph(render_mode)), glyph_bitmap);	// cached img-> glyph_bitmap
				//TRACE(_T("Cache Hit: %wc, size:%d, 0x%8.8X\n"), wch, chData->GetWidth(), glyph_bitmap);
			}
		}
		if (!*glyph_bitmap) {	// case: no cache found
			renderer_raii::UniqueReferencedGlyph glyph;
			bool f_glyph = false;
			//GLYPHMETRICS gm;
			const MAT2 mat2 = { { 0, 1 },{ 0, 0 },{ 0, 0 },{ 0, 1 } };
			UINT ggoformat = ggoformatbase;
			CTempMem<PVOID> ggobuf;
			DWORD outlinesize = 0;

			if (bGlyphIndex) {	// glyph index doesn't require any font linking
				f_glyph = !!wch;
				glyph_index = wch;
				*AAList = AAMode;
				GetCharWidthI(FTInfo.hdc, wch, 1, reinterpret_cast<LPWORD>(&wch), &gdi32x);	//index的文字必须计算宽度
				if (FTInfo.font_type.height <= FTInfo.rasterPolicy.bitmapHeight && pfi->EmbeddedBmpExist(FTInfo.font_type.height))
				{
					f_glyph = false;	//使用点阵，不绘图
					*drState = FT_DRAW_EMBEDDED_BITMAP;	//设置为点阵绘图方式
				}
			}
			else
				if (wch && !CID.myiswcntrl(lpString[0])) {	// need to draw a non-control character
					for (int j = 0; j < FTInfo.face_id_list_num; ++j) {
						freetype_face = nullptr;	// reinitialize it in case no fontlinking is available.
						if (bWindowsLink)	//使用Windows函数进行fontlink
						{
							if (!fontLinks[j][i])	//还没初始化该字体的fontlink
							{
								SelectFont(FTInfo.hdc, FTInfo.ggo_font_list[j]);	//加载ggo字体
								GetGlyphIndices(FTInfo.hdc, lpString, cbString - i, &fontLinks[j][i], GGI_MARK_NONEXISTING_GLYPHS);	//进行fontlink
								SelectFont(FTInfo.hdc, hOldFont);
							}
							glyph_index = fontLinks[j][i];
							if (glyph_index == 0xffff)
								glyph_index = 0;
						}
						else		//使用freetype进行fontlink
						{
							CCriticalSectionLock __lock(CCriticalSectionLock::CS_MANAGER);
							glyph_index = FTC_CMapCache_Lookup(cmap_cache, FTInfo.face_id_list[j], -1, wch);
							//glyph_index = FT_Get_Char_Index(FTInfo.GetFace(j), wch);
						}
						if (glyph_index) {
							GetCharWidth32W(FTInfo.hdc, wch, wch, &gdi32x);	//有效文字，计算宽度
							f_glyph = true;
							FTInfo.font_type.face_id = FTInfo.face_id_list[j];
							freetype_face = FTInfo.GetFace(j);	//同时更新对应faceid的实际face
							//接下来更新对应的fontsetting
							FTInfo.font_type.flags = FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH;
							// ヒンティング
							//extern CFontSetCache g_fsetcache;
							//pfs = g_fsetcache.Get(FTInfo.font_type.face_id);
							if (FTInfo.font_type.face_id == FTInfo.face_id_simsun && j > 0)
							{
								switch (FTInfo.font_type.height)
								{
								case 11: {FTInfo.font_type.height = 12; FTInfo.font_type.width++; break; }	//对宋体进行特殊处理
								case 13: {FTInfo.font_type.height = 15; FTInfo.font_type.width += 2; break; }
								}
							}
							pfi = g_pFTEngine->FindFont(static_cast<int>(reinterpret_cast<INT_PTR>(FTInfo.font_type.face_id)));
							if (pfi)
							{
								pfs = &pfi->GetFontSettings();
								switch (pfs->GetHintingMode()) {
								case 0:
									// ignore.
									break;
								case 1:
									FTInfo.font_type.flags |= FT_LOAD_NO_HINTING;
									break;
								case 2:
									FTInfo.font_type.flags |= FT_LOAD_FORCE_AUTOHINT;
									break;
								}
								// アンチエイリアス
								if (FTInfo.IsMono()) {
									FTInfo.font_type.flags |= FT_LOAD_TARGET_MONO;
									render_mode = FT_RENDER_MODE_MONO;
								}
								else {
									switch (*AAList = pfs->GetAntiAliasMode()) {
									case -1:
										FTInfo.font_type.flags |= FT_LOAD_TARGET_MONO;
										render_mode = FT_RENDER_MODE_MONO;
										break;
									case 0:
										FTInfo.font_type.flags |= FT_LOAD_TARGET_NORMAL;
										render_mode = FT_RENDER_MODE_NORMAL;
										break;
									case 1:
										FTInfo.font_type.flags |= FT_LOAD_TARGET_LIGHT;
										render_mode = FT_RENDER_MODE_LIGHT;
										break;
									case 2:
									case 3:
										FTInfo.font_type.flags |= FT_LOAD_TARGET_LCD;
										render_mode = FT_RENDER_MODE_LCD;
										break;
									case 4:
									case 5:
										FTInfo.font_type.flags |= FT_LOAD_TARGET_LIGHT;
										render_mode = FT_RENDER_MODE_LCD;
										break;
									}
								}
								if (FTInfo.rasterPolicy.hintSmallFont && FTInfo.font_type.flags & FT_LOAD_TARGET_LIGHT && FTInfo.font_type.height != -1 && FTInfo.font_type.height < 12)  //通用设置不使用hinting，但是打开了小字体hinting开关
									FTInfo.font_type.flags = FTInfo.font_type.flags & (~FT_LOAD_NO_HINTING)/* | (pfi->FontHasHinting() ? FT_LOAD_DEFAULT : FT_LOAD_FORCE_AUTOHINT)*/;

								AAMode = *AAList/*pfs->GetAntiAliasMode()*/;
								bLcdMode = render_mode == FT_RENDER_MODE_LCD;
								bLightLcdMode = (AAMode == 4) || (AAMode == 5);
								//更新完成
							}
							if (FTInfo.font_type.height <= FTInfo.rasterPolicy.bitmapHeight && pfi->EmbeddedBmpExist(FTInfo.font_type.height))
							{
								f_glyph = false;	//使用点阵，不绘图
								*drState = FT_DRAW_EMBEDDED_BITMAP;	//设置为点阵绘图方式
							}
							break;
						}
					}
				}



			if (!f_glyph || !freetype_face) {	//can't find suitable fontface, glyphindex case is already calculated.
#ifdef _DEBUG
				GdiSetBatchLimit(0);
#endif
				if (*drState == FT_DRAW_NORMAL || bGlyphIndex)
					*drState = FT_DRAW_NOTFOUND;	//找不到文字
				if ((!FTInfo.lpDx || lpString == lpEnd - 1) && !bGlyphIndex)	//无效文字，而且没有事先排版或者是排版的最后一个字符了
				{
					GetCharWidth32W(FTInfo.hdc, wch, wch, &gdi32x);
				}
				int cx = gdi32x;
				{
					if (wch) {
						*glyph_bitmap = nullptr;	//无效文字
					}
					BOOL isc = bGlyphIndex ? false : (CID.myiswcntrl(*lpString));
					if (isc == CNTRL_UNICODE_PLANE)
					{
						if (!FTInfo.lpDx) {
							SIZE p = { 0 };
							if (GetTextExtentExPointW(FTInfo.hdc, lpString, 2, 99999, nullptr, nullptr, &p)) {
								gdi32x = p.cx;
								cx = gdi32x;
							}
						}
						bUnicodePlane = true;
					}
					int dyHeight = clpdx.gety(0);
					int dxWidth = clpdx.get(cx);

					if (isc == CNTRL_COMPLEX_TEXT)	//控制字
					{
						cx = dxWidth;	//服从windows的宽度调度
					}
					if (lpString < lpEnd - 1) {
						FTInfo.x += dxWidth;
						FTInfo.y -= dyHeight;
					}
					else {
						FTInfo.px = FTInfo.x + cx;
						FTInfo.x += dxWidth;//Max(clpdx.get(cx), cx);/*(int)abc.abcB+abc.abcC*///Max(clpdx.get(cx), abc.abcB? abc.abcA:0);
					}
					if (!isc)
						FTInfo.x += FTInfo.params->charExtra;
				}
				goto cont;
			}

			// vertical font found
			if (bVertical) {
				glyph_index = ft2vert_get_gid(
					static_cast<struct ft2vert_st*>(freetype_face->generic.data),
					glyph_index);
			}

			// カーニング
			if (useKerning) {
				if (previous != 0 && glyph_index) {
					FT_Vector delta;
					FT_Get_Kerning(freetype_face,
						previous, glyph_index,
						ft_kerning_default, &delta);
					FTInfo.x += FT_PosToInt(delta.x);
				}
				previous = glyph_index;
			}


			// 縦横
			if (bVertical && IsVerticalChar(wch)) {
				FTInfo.font_type.flags |= FT_LOAD_VERTICAL_LAYOUT;
				if (bLcdMode) {
					if ((FTInfo.font_type.flags & FT_LOAD_TARGET_LCD) == FT_LOAD_TARGET_LCD) {
						FTInfo.font_type.flags &= ~FT_LOAD_TARGET_LCD;
						FTInfo.font_type.flags |= FT_LOAD_TARGET_LCD_V;
					}
					render_mode = FT_RENDER_MODE_LCD_V;
				}
			}
			else {
				if (bVertical)
					swap(FTInfo.font_type.height, FTInfo.font_type.width);	//交换无法旋转的文字宽高
				FTInfo.font_type.flags &= ~FT_LOAD_VERTICAL_LAYOUT;
				if (bLcdMode) {
					if (FTInfo.font_type.flags & FT_LOAD_TARGET_LCD_V == FT_LOAD_TARGET_LCD_V) {
						FTInfo.font_type.flags &= ~FT_LOAD_TARGET_LCD_V;
						FTInfo.font_type.flags |= FT_LOAD_TARGET_LCD;
					}
					render_mode = FT_RENDER_MODE_LCD;
				}
			}

			{

				bool bRequiredownsize;

				bIsIndivBold = freetype_face->style_flags & FT_STYLE_FLAG_BOLD;	// separate bold font?
				bIsBold = (IsFontBold(lf) && !bIsIndivBold);	// Synthesize bold when no distinct bold face exists.
				bRequiredownsize = bIsBold && FTInfo.rasterPolicy.bolderMode != 1;
				if (bRequiredownsize)
				{
					FTInfo.font_type.width -= (FTInfo.font_type.width) / 36;
					FTInfo.font_type.height -= (FTInfo.font_type.height) / 36;
				}

				{
					CCriticalSectionLock __lock(CCriticalSectionLock::CS_MANAGER);
					FT_Glyph temp_glyph = nullptr;
					if (FTC_ImageCache_Lookup(
						image_cache,
						&FTInfo.font_type,
						glyph_index,
						&temp_glyph,
						nullptr)) {
						nRet = false;
						goto gdiexit;
					}
					glyph.reset(New_FT_Ref_Glyph());
					if (!glyph || FT_Glyph_Copy(temp_glyph, &(glyph.get()->ft_glyph))) {
						nRet = false;
						goto gdiexit;
					}
				}

				FTInfo.font_type.height = FTInfo.height;
				FTInfo.font_type.width = FTInfo.width;

			}
			{
				CCriticalSectionLock __lock(CCriticalSectionLock::CS_LIBRARY);
				if (FT_Glyph_Ref_Copy(glyph.get(), glyph_bitmap))
				{
					nRet = FALSE;
					goto gdiexit;
				}
				glyph.reset();
			}
			if ((*glyph_bitmap)->ft_glyph->format != FT_GLYPH_FORMAT_BITMAP) {
				int str_h;
				int str_v;
				bool fbold = false;
				str_h = str_v = FTInfo.pfi->CalcNormalWeight();
				if (bIsIndivBold)
					str_h = str_v = FTInfo.pfi->GetExactBoldWeight() << 2;
				if (bIsBold) {
					fbold = true;
					str_h += FTInfo.font_type.height < 24 ? FTInfo.pfi->GetFTWeight() : (FTInfo.pfi->GetFTWeight() * FTInfo.font_type.height / 24);
					str_v = str_h;
				}
				if ((str_h || str_v) && New_FT_Outline_Embolden(
					&(reinterpret_cast<FT_OutlineGlyph>((*glyph_bitmap)->ft_glyph))->outline,
					str_h, str_v, FTInfo.height))
				{
					FT_Done_Ref_Glyph(glyph_bitmap);
					nRet = false;
					goto gdiexit;
				}

				if (fbold) {
					(reinterpret_cast<FT_BitmapGlyph>((*glyph_bitmap)->ft_glyph))->root.advance.x += 0x10000;
				}
				if (lf.lfItalic &&
					!(freetype_face->style_flags & FT_STYLE_FLAG_ITALIC)) {
					FT_Matrix matrix;
					FTInfo.pfi->CalcItalicSlant(matrix);
					FT_Outline_Transform(
						&(reinterpret_cast<FT_OutlineGlyph>((*glyph_bitmap)->ft_glyph))->outline,
						&matrix);
				}
				{
					CCriticalSectionLock __lock(CCriticalSectionLock::CS_LIBRARY);
					if (bLoadColor && FT_HAS_COLOR(freetype_face)) {
						// use custom API to get color bitmap
						if (FT_Glyph_To_BitmapEx(&((*glyph_bitmap)->ft_glyph), render_mode, nullptr, 1, 1, glyph_index, freetype_face)) {
							FT_Done_Ref_Glyph(glyph_bitmap);
							nRet = false;
							goto gdiexit;
						}
					}
					else
						if (FT_Glyph_To_Bitmap(&((*glyph_bitmap)->ft_glyph), render_mode, nullptr, 1)) {
							FT_Done_Ref_Glyph(glyph_bitmap);
							nRet = false;
							goto gdiexit;
						}
				}
			}
		}	// end of "case: no cache found"

		int cx = (bVertical && IsVerticalChar(wch)) ?
			FT_FixedToInt(FT_BitmapGlyph((*glyph_bitmap)->ft_glyph)->root.advance.y) :
			FT_FixedToInt(FT_BitmapGlyph((*glyph_bitmap)->ft_glyph)->root.advance.x);

		{
			int dy = clpdx.gety(0);	//获得高度
			int dx = clpdx.get(bWidthGDI32 ? gdi32x : cx);	//获得宽度
			int left = FT_BitmapGlyph((*glyph_bitmap)->ft_glyph)->left;
			if (gdi32x == 0) {	// zero width text (most likely a diacritic)
				if (FTInfo.x + dx + left < FTInfo.xBase)
					FTInfo.xBase = FTInfo.x + dx + left;
				//it needs to be drawn at the end of the offset (Windows specific, Windows will "share" half of letter's width to the diacritic)
				if (i > 0) {
					// Preserve the previous glyph's logical start across this API's spacing adjustment.
					*(Dx - 1) = FTInfo.x + dx;
				}
			}
			else {
				if (FTInfo.x + left < FTInfo.xBase)
					FTInfo.xBase = FTInfo.x + left;	//如果有字符是负数起始位置的（合成符号）， 调整文字的起始位置
			}

			if (lpString < lpEnd - 1) {
				FTInfo.x += dx;
				FTInfo.y -= dy;
			}
			else {
				int bx = FT_BitmapGlyph((*glyph_bitmap)->ft_glyph)->bitmap.width;
				if (render_mode == FT_RENDER_MODE_LCD && FT_BitmapGlyph((*glyph_bitmap)->ft_glyph)->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA) bx /= 3;
				bx += left;
				FTInfo.px = FTInfo.x + Max(Max(dx, bx), cx);	//有文字的情况下,绘图宽度=ft计算的宽度，鼠标位置=win宽度
				FTInfo.x += dx;//Max(dx, gdi32x);//Max(Max(dx, bx), cx);
			}

		}
		FTInfo.x += FTInfo.params->charExtra;

		//キャッシュ化
		if (glyph_index) {

			if (bGlyphIndex) {
				pftCache->AddGlyphData(glyph_index, /*cx*/FTInfo.width, gdi32x, reinterpret_cast<FT_Referenced_BitmapGlyph>(*glyph_bitmap), render_mode, AAMode);
			}
			else {
				pftCache->AddCharData(wch, glyph_index, /*cx*/FTInfo.width, gdi32x, reinterpret_cast<FT_Referenced_BitmapGlyph>(*glyph_bitmap), render_mode, AAMode);
			}
		}

	cont:
		*Dx = FTInfo.x;		//Dx的位置是下一个字符开始的基准位置，并不是下一个字符开始画的位置
		*Dy = FTInfo.y;		//Dy的位置是下一个字符的y坐标
		++Dx;
		++Dy;
	}
gdiexit:
	return nRet;
}


BOOL ForEachGetGlyphGGO(FreeTypeDrawInfo& FTInfo, LPCTSTR lpString, int cbString, FT_Referenced_Glyph* GlyphArray, FT_DRAW_STATE* drState)
{
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	//Snowie!!
	BOOL bIsSymbol = GetTextCharsetInfo(FTInfo.hdc, nullptr, 0) == SYMBOL_CHARSET;
	BOOL bAllowDefaultLink = pSettings->GetFontLinkInfo().IsAllowFontLink(static_cast<BYTE>(GetTextCharsetInfo(FTInfo.hdc, nullptr, 0)));	//是否为符号
	BOOL nRet = true;
	BOOL bWindowsLink = FTInfo.rasterPolicy.fontLinkMode == 2;
	//!!Snowie

	FT_Face freetype_face = FTInfo.freetype_face;
	const FT_Int cmap_index = FTInfo.cmap_index;
	const FT_Bool useKerning = FTInfo.useKerning;
	FT_Render_Mode render_mode = FTInfo.render_mode;
	const int LinkNum = FTInfo.face_id_list_num;
	int AAMode = FTInfo.pfs->GetAntiAliasMode();
	int* AAList = FTInfo.AAModes;
	const LOGFONTW& lf = FTInfo.LogFont();
	FreeTypeFontCache* pftCache = FTInfo.pftCache;
	const CFontSettings*& pfs = FTInfo.pfs;
	FreeTypeFontInfo*& pfi = FTInfo.pfi;
	const bool bGlyphIndex = FTInfo.IsGlyphIndex();
	const LPCTSTR lpStart = lpString;
	const LPCTSTR lpEnd = lpString + cbString;
	FT_UInt previous = 0;
	WCHAR previouswch = 0;
	const bool bVertical = false;
	bool bLcdMode = render_mode == FT_RENDER_MODE_LCD;
	bool bLightLcdMode = (AAMode == 4) || (AAMode == 5);
	ClpDx clpdx(FTInfo.lpDx, FTInfo.params->etoOptions);
	const bool bWidthGDI32 = FTInfo.rasterPolicy.widthMode == SETTING_WIDTHMODE_GDI32;
	const int ggoformatbase = (FTInfo.font_type.flags & FT_LOAD_NO_HINTING) ? GGO_UNHINTED | GGO_NATIVE : GGO_NATIVE;

	if (!s_GGOGlyphLoader.init(freetype_library)) {
		return FALSE;
	}

	std::vector<WORD> glyphIndices(static_cast<size_t>(cbString));
	WORD* gi = glyphIndices.data();

	if (!bGlyphIndex)  	//仅对win32情况进行优化，ft情况另议
		if (GetGlyphIndices(FTInfo.hdc, lpString, cbString, gi, GGI_MARK_NONEXISTING_GLYPHS) != cbString)
		{
			nRet = false;
			goto gdiexit;
		}
	//!!Snowie
	int* Dx = FTInfo.Dx;
	int* Dy = FTInfo.Dy;
	if (!bAllowDefaultLink && FTInfo.face_id_list_num > 1)
		FTInfo.face_id_list_num--;	//如果是symbol页那就不链接到宋体

	for (int i = 0; lpString < lpEnd; ++lpString, gi++, GlyphArray++, drState++, ++AAList, i++) {
		WCHAR wch = *lpString;
		if (!bGlyphIndex && bIsSymbol && !bWindowsLink)
			wch |= 0xF000;
		FT_Referenced_Glyph* glyph_bitmap = GlyphArray;
		int gdi32x = 0;
		FTInfo.font_type.face_id = FTInfo.face_id_list[0];
		FreeTypeCharData* chData = nullptr;
		FT_UInt glyph_index = 0;
		BOOL bIsBold = false, bIsIndivBold = false;

		{

			chData = bGlyphIndex
				? pftCache->FindGlyphIndex(wch)
				: pftCache->FindChar(wch);

			if (chData && FTInfo.width == chData->GetWidth()) {
				gdi32x = chData->GetGDIWidth();
				*AAList = chData->GetAAMode();
				CCriticalSectionLock __lock(CCriticalSectionLock::CS_LIBRARY);
				FT_Glyph_Ref_Copy(reinterpret_cast<FT_Referenced_Glyph>(chData->GetGlyph(render_mode)), glyph_bitmap);
			}
		}
		if (!*glyph_bitmap) {
			renderer_raii::UniqueReferencedGlyph glyph;
			bool f_glyph = false;
			GLYPHMETRICS gm;
			const MAT2 mat2 = { { 0, 1 },{ 0, 0 },{ 0, 0 },{ 0, 1 } };
			UINT ggoformat = ggoformatbase;
			CTempMem<PVOID> ggobuf;
			DWORD outlinesize = 0;


			if (bGlyphIndex) {
				f_glyph = true;
				*AAList = AAMode;
				glyph_index = wch;
				ggoformat |= GGO_GLYPH_INDEX;
				GetCharWidthI(FTInfo.hdc, wch, 1, reinterpret_cast<LPWORD>(&wch), &gdi32x);	//index的文字必须计算宽度
			}
			else
			{
				if (*(gi) != 0xffff) {
					glyph_index = *(gi);
					f_glyph = true;
					*AAList = AAMode;
				}
				GetCharWidth32W(FTInfo.hdc, wch, wch, &gdi32x);	//有效文字，计算宽度
			}
			if (lpString == lpStart && FTInfo.font_type.flags & FT_LOAD_FORCE_AUTOHINT) {
				// FORCE_AUTOHINT
				GetGlyphOutlineW(FTInfo.hdc, 0, GGO_METRICS | GGO_GLYPH_INDEX | GGO_NATIVE | GGO_UNHINTED, &gm, 0, nullptr, &mat2);
			}
			outlinesize = GetGlyphOutlineW(FTInfo.hdc, wch, ggoformat, &gm, ggobuf.getsize(), ggobuf, &mat2);

			if (outlinesize == GDI_ERROR || outlinesize == 0) {
				glyph_index = 0;
				f_glyph = false;
			}
			else
			{
				glyph_index = wch;
				f_glyph = true;
			}


			if (!f_glyph) {	//glyphindex的文字上面已经计算过了
#ifdef _DEBUG
				GdiSetBatchLimit(0);
#endif
				if (*drState == FT_DRAW_NORMAL || bGlyphIndex)
					*drState = FT_DRAW_NOTFOUND;	//找不到文字
				if ((!FTInfo.lpDx || lpString == lpEnd - 1) && !bGlyphIndex)	//无效文字，而且没有事先排版或者是排版的最后一个字符了
				{
					GetCharWidth32W(FTInfo.hdc, wch, wch, &gdi32x);
				}
				int cx = gdi32x;
				if (wch) {
					*glyph_bitmap = nullptr;	//无效文字
				}
				BOOL isc = bGlyphIndex ? false : (CID.myiswcntrl(*lpString));
				if (isc)
					cx = 0;
				if (lpString < lpEnd - 1) {
					FTInfo.y -= clpdx.gety(0);
					FTInfo.x += clpdx.get(cx);
				}
				else {
					FTInfo.px = FTInfo.x + cx;
					FTInfo.x += clpdx.get(cx);
				}
				if (!isc)
					FTInfo.x += FTInfo.params->charExtra;
				goto cont;
			}


			if (useKerning && !bGlyphIndex) {
				if (previouswch && wch) {
					FTInfo.x += FTInfo.ggokerning.get(previouswch, wch);
				}
				previouswch = wch;
			}


			// 縦横
			if (bVertical && IsVerticalChar(wch)) {
				FTInfo.font_type.flags |= FT_LOAD_VERTICAL_LAYOUT;
				if (bLcdMode) {
					if (FTInfo.font_type.flags & FT_LOAD_TARGET_LCD == FT_LOAD_TARGET_LCD) {
						FTInfo.font_type.flags &= ~FT_LOAD_TARGET_LCD;
						FTInfo.font_type.flags |= FT_LOAD_TARGET_LCD_V;
					}
					render_mode = FT_RENDER_MODE_LCD_V;
				}
			}
			else {
				if (bVertical)
					swap(FTInfo.font_type.height, FTInfo.font_type.width);	//交换无法旋转的文字宽高
				FTInfo.font_type.flags &= ~FT_LOAD_VERTICAL_LAYOUT;
				if (bLcdMode) {
					if (FTInfo.font_type.flags & FT_LOAD_TARGET_LCD_V == FT_LOAD_TARGET_LCD_V) {
						FTInfo.font_type.flags &= ~FT_LOAD_TARGET_LCD_V;
						FTInfo.font_type.flags |= FT_LOAD_TARGET_LCD;
					}
					render_mode = FT_RENDER_MODE_LCD;
				}
			}

			CGGOOutlineGlyph ggoog;
			{

				if (outlinesize > ggobuf.getsize()) {
					if (!ggobuf.init(outlinesize)) {
						nRet = false;
						goto gdiexit;
					}
					//ggofont.change();
					outlinesize = GetGlyphOutlineW(FTInfo.hdc, wch, ggoformat, &gm, ggobuf.getsize(), ggobuf, &mat2);
					//ggofont.restore();
				}
				if (outlinesize > ggobuf.getsize()) {
					nRet = false;
					goto gdiexit;
				}
				if (!ggoog.init(outlinesize, ggobuf, gm)) {
					nRet = false;
					goto gdiexit;
				}
				glyph.reset(New_FT_Ref_Glyph());
				if (!glyph || FT_Glyph_Copy(static_cast<FT_Glyph>(ggoog), &(glyph.get()->ft_glyph))) {
					nRet = false;
					goto gdiexit;
				}
				//glyph = ggoog;
			}
			{
				CCriticalSectionLock __lock(CCriticalSectionLock::CS_LIBRARY);
				if (FT_Glyph_Ref_Copy(glyph.get(), glyph_bitmap))
				{
					nRet = FALSE;
					goto gdiexit;
				}
				glyph.reset();
			}
			if ((*glyph_bitmap)->ft_glyph->format != FT_GLYPH_FORMAT_BITMAP) {
				int str_h;
				int str_v;
				bool fbold = false;
				str_h = str_v = FTInfo.pfi->CalcNormalWeight();
				if (bIsIndivBold)
					str_h = str_v = FTInfo.pfi->GetExactBoldWeight() << 2;
				if (bIsBold) {
					fbold = true;
					str_h += FTInfo.font_type.height < 24 ? FTInfo.pfi->GetFTWeight() : (FTInfo.pfi->GetFTWeight() * FTInfo.font_type.height / 24);
					str_v = str_h;
				}
				if ((str_h || str_v) && New_FT_Outline_Embolden(
					&(reinterpret_cast<FT_OutlineGlyph>((*glyph_bitmap)->ft_glyph))->outline,
					str_h, str_v, FTInfo.height))
				{
					FT_Done_Ref_Glyph(glyph_bitmap);
					nRet = false;
					goto gdiexit;
				}

				if (fbold) {
					(reinterpret_cast<FT_BitmapGlyph>((*glyph_bitmap)->ft_glyph))->root.advance.x += 0x10000;
				}

				{
					CCriticalSectionLock __lock(CCriticalSectionLock::CS_LIBRARY);
					if (FT_Glyph_To_Bitmap(&((*glyph_bitmap)->ft_glyph), render_mode, nullptr, 1)) {
						FT_Done_Ref_Glyph(glyph_bitmap);
						nRet = false;
						goto gdiexit;
					}
				}
			}
		}

		int cx = (bVertical && IsVerticalChar(wch)) ?
			FT_FixedToInt(FT_BitmapGlyph((*glyph_bitmap)->ft_glyph)->root.advance.y) :
			FT_FixedToInt(FT_BitmapGlyph((*glyph_bitmap)->ft_glyph)->root.advance.x);
		{
			int dy = clpdx.gety(0);
			int dx = clpdx.get(bWidthGDI32 ? gdi32x : cx);	//获得宽度
			int left = FT_BitmapGlyph((*glyph_bitmap)->ft_glyph)->left;
			if (FTInfo.x + left < FTInfo.xBase)
				FTInfo.xBase = FTInfo.x + left;	//如果有字符是负数起始位置的（合成符号）， 调整文字的起始位置

			if (lpString < lpEnd - 1) {
				FTInfo.x += dx;
				FTInfo.y -= dy;
			}
			else {
				int bx = FT_BitmapGlyph((*glyph_bitmap)->ft_glyph)->bitmap.width;
				if (render_mode == FT_RENDER_MODE_LCD) bx /= 3;
				bx += left;
				FTInfo.px = FTInfo.x + Max(Max(dx, bx), cx);	//有文字的情况下,绘图宽度=ft计算的宽度，鼠标位置=win宽度
				FTInfo.x += dx;
			}

		}
		FTInfo.x += FTInfo.params->charExtra;

		//キャッシュ化
		if (glyph_index) {

			if (bGlyphIndex) {
				pftCache->AddGlyphData(glyph_index, FTInfo.width, gdi32x, reinterpret_cast<FT_Referenced_BitmapGlyph>(*glyph_bitmap), render_mode, AAMode);
			}
			else {
				pftCache->AddCharData(wch, glyph_index, FTInfo.width, gdi32x, reinterpret_cast<FT_Referenced_BitmapGlyph>(*glyph_bitmap), render_mode, AAMode);
			}
		}
	cont:
		*Dx = FTInfo.x;
		*Dy = FTInfo.y;
		++Dx;
		++Dy;
	}
gdiexit:
	return nRet;
}


BOOL GetLogFontFromDC(HDC hdc, LOGFONT& lf)
{
	HFONT hf = GetCurrentFont(hdc);
	if (!ORIG_GetObjectW(hf, sizeof(LOGFONTW), &lf))
		return FALSE;
	return TRUE;
}

BOOL CALLBACK TextOutCallback(FreeTypeGlyphInfo& FTGInfo)
{
	FreeTypeDrawInfo* FTInfo = FTGInfo.FTInfo;
	FT_BitmapGlyph& glyph_bitmap = FTGInfo.FTGlyph;
	const bool bVertical = FTInfo->LogFont().lfFaceName[0] == _T('@');
	int nOldAlpha = FTInfo->params->alpha;

	if (!FTGInfo.FTGlyph->bitmap.buffer) {
		ORIG_ExtTextOutW(FTInfo->hdc, FTInfo->x, FTInfo->yTop, FTInfo->GetETO(), nullptr, &FTGInfo.wch, 1, nullptr);
	}
	else {

		const CGdippSettings* pSettings = CGdippSettings::GetInstance();
		if (bVertical && IsVerticalChar(FTGInfo.wch) &&
			pSettings->FontLoader() == SETTING_FONTLOADER_FREETYPE) {
			if (FTInfo->params->alpha > 1)
			{
				FreeTypeDrawBitmapV(FTGInfo, *FTGInfo.shadow, FTInfo->x + FTInfo->sx,
					FTInfo->yTop + FTInfo->params->otm->otmTextMetrics.tmHeight - (glyph_bitmap->left + glyph_bitmap->bitmap.width) - 1 + FTInfo->sy);//画阴影
				FTInfo->params->alpha = 1;
			}
			if (!FreeTypeDrawBitmapV(FTGInfo, *FTGInfo.solid, FTInfo->x,
				FTInfo->yTop + FTInfo->params->otm->otmTextMetrics.tmHeight - (glyph_bitmap->left + glyph_bitmap->bitmap.width) - 1))	//画文字
			{
				// fallback to GDI when fail to draw with FT
				ORIG_ExtTextOutW(FTInfo->hdc, FTInfo->x, FTInfo->yTop, FTInfo->GetETO(), nullptr, &FTGInfo.wch, 1, nullptr);
			}
		}
		else {
			if (FTInfo->params->alpha > 1)
			{
				FreeTypeDrawBitmap(FTGInfo, *FTGInfo.shadow,
					FTInfo->x + glyph_bitmap->left + FTInfo->sx,
					FTInfo->yTop + FTInfo->yBase - glyph_bitmap->top + FTInfo->sy);	//画阴影
				FTInfo->params->alpha = 1;
			}
			if (!FreeTypeDrawBitmap(FTGInfo, *FTGInfo.solid,
				FTInfo->x + glyph_bitmap->left,
				FTInfo->yTop + FTInfo->yBase - glyph_bitmap->top))	//画文字
			{
				// fallback to GDI when fail to draw with FT
				ORIG_ExtTextOutW(FTInfo->hdc, FTInfo->x, FTInfo->yTop, FTInfo->GetETO(), nullptr, &FTGInfo.wch, 1, nullptr);
			}

		}
	}
	FTInfo->params->alpha = nOldAlpha;
	return TRUE;
}

int IsColorDark(DWORD Color, double Gamma)
{
	//===============================================================
	//采用Photoshop sRGB的RGB->Lab算法进行换算，L为色彩视觉亮度
	//感谢 西安理工大学 贾婉丽 的分析
	//===============================================================
	static double s_multipler = 116 / pow(100, (double)1.0 / 3.0);	//预计算常数,强制使用double版本
	double* RGBTable = s_AlphaBlendTable.GetRGBTable();	//获得显示器转换表
	double ret = pow(23.9746 * RGBTable[GetRValue(Color)] + 73.0653 * RGBTable[GetGValue(Color)] + 6.13799 * RGBTable[GetBValue(Color)], 1.0 / 3.0) * s_multipler - 16;
	return max(int(ret + 0.499), 0);
}

BOOL FreeTypeTextOut(
	const HDC hdc,     // デバイスコンテキストのハンドル
	CBitmapCache& cache,
	LPCWSTR lpString,  // 文字列
	int cbString,      // 文字数
	FreeTypeDrawInfo& FTInfo,
	FT_Referenced_Glyph* Glyphs,
	FT_DRAW_STATE* drState
)
{
	if (cbString <= 0 || lpString == nullptr)
		return FALSE;
	std::unique_ptr<CAlphaBlendColor> solid;
	std::unique_ptr<CAlphaBlendColor> shadow;

	FT_Face freetype_face = FTInfo.freetype_face;
	const LOGFONT& lf = FTInfo.LogFont();

	FTInfo.x = -FTInfo.xBase;
	FTInfo.yTop = 0;

	const TEXTMETRIC& tm = FTInfo.params->otm->otmTextMetrics;
	FTInfo.yBase = tm.tmAscent;

	//===============计算颜色缓存======================

	int lightdiff, darkdiff, bDarkColor = 0, ShadowColor = 0;
	if (FTInfo.params->alpha != 1)
	{
		float Gamma = FTInfo.rasterPolicy.gamma;
		bDarkColor = IsColorDark(FTInfo.params->color, Gamma);
		int diff = max(darkdiff = abs(IsColorDark(FTInfo.rasterPolicy.shadowDarkColor, Gamma) - bDarkColor), lightdiff = abs(IsColorDark(FTInfo.rasterPolicy.shadowLightColor, Gamma) - bDarkColor));
		ShadowColor = lightdiff <= darkdiff ? FTInfo.rasterPolicy.shadowDarkColor : FTInfo.rasterPolicy.shadowLightColor;
		bDarkColor = lightdiff <= darkdiff;
		if (FTInfo.rasterPolicy.shadowDarkColor == FTInfo.rasterPolicy.shadowLightColor)
		{
			//无视底色问题，强制开启阴影
			FTInfo.params->alphatuner = 1;
		}
		else
		{
			diff = abs(lightdiff - darkdiff);
			if (diff < 10)
				FTInfo.params->alpha = 1;
			else
				FTInfo.params->alphatuner = max(1, 100 / diff);	//根据色差调整阴影浓度
		}
	}
	char mode = (*Glyphs) ? FT_BitmapGlyph((*Glyphs)->ft_glyph)->bitmap.pixel_mode : FT_PIXEL_MODE_LCD;
	switch (mode) {
	case FT_PIXEL_MODE_MONO:
		return false;
	case FT_PIXEL_MODE_LCD:
		solid = std::make_unique<CAlphaBlendColor>(FTInfo.params->color, 1, true, true, true);
		shadow = std::make_unique<CAlphaBlendColor>(ShadowColor, FTInfo.params->alpha, true, bDarkColor, true);
		break;
	case FT_PIXEL_MODE_LCD_V:
		solid = std::make_unique<CAlphaBlendColor>(FTInfo.params->color, 1, true, true, false);
		shadow = std::make_unique<CAlphaBlendColor>(ShadowColor, FTInfo.params->alpha, true, bDarkColor, false);
		break;
	case FT_PIXEL_MODE_GRAY:
		solid = std::make_unique<CAlphaBlendColor>(FTInfo.params->color, 1, false, true, true);
		shadow = std::make_unique<CAlphaBlendColor>(ShadowColor, FTInfo.params->alpha, false, bDarkColor, true);
		break;
	default:
		solid = std::make_unique<CAlphaBlendColor>(FTInfo.params->color, 1, FTInfo.rasterPolicy.lcdFilter, true);
		shadow = std::make_unique<CAlphaBlendColor>(ShadowColor, FTInfo.params->alpha, FTInfo.rasterPolicy.lcdFilter, bDarkColor);
		break;
	}

	//计算下划线或删除线的信息
	int decorationInfo_height;
	int decorationInfo_thickness;
	OUTLINETEXTMETRIC& decorationInfo_otm = *FTInfo.params->otm;
	if (lf.lfUnderline || lf.lfStrikeOut) {

		if (lf.lfUnderline) {
			switch (FTInfo.rasterPolicy.fontLoader) {
			case SETTING_FONTLOADER_FREETYPE:
				decorationInfo_height = decorationInfo_otm.otmTextMetrics.tmHeight; //FT_PosToInt(freetype_face->size->metrics.height);
				decorationInfo_thickness =
					MulDiv(freetype_face->underline_thickness,
						FTInfo.font_type.height/*freetype_face->size->metrics.y_ppem*/,
						freetype_face->units_per_EM);
				break;
			case SETTING_FONTLOADER_WIN32:
				decorationInfo_height = decorationInfo_otm.otmTextMetrics.tmHeight;
				decorationInfo_thickness = decorationInfo_otm.otmsUnderscoreSize;
				break;
			}
		}

		if (lf.lfStrikeOut) {
			switch (FTInfo.rasterPolicy.fontLoader) {
			case SETTING_FONTLOADER_FREETYPE:
				decorationInfo_thickness =
					MulDiv(freetype_face->underline_thickness,
						FTInfo.font_type.height,// freetype_face->size->metrics.y_ppem,
						freetype_face->units_per_EM);
				break;
			case SETTING_FONTLOADER_WIN32:
				decorationInfo_thickness = decorationInfo_otm.otmsStrikeoutSize;
				break;
			}
		}
	}

	//===============计算完成==========================

	FreeTypeGlyphInfo FTGInfo = { &FTInfo, 0, nullptr, 0, solid.get(), shadow.get(), FTInfo.rasterPolicy.invertColor };
	for (int i = 0; i < cbString; ++i, ++lpString)
	{
		WCHAR wch = *lpString;
		if (Glyphs[i])	// paint text with FreeType
		{
			FTGInfo.wch = wch;
			FTGInfo.FTGlyph = reinterpret_cast<FT_BitmapGlyph>(Glyphs[i]->ft_glyph);
			FTGInfo.AAMode = FTInfo.AAModes[i];
			TextOutCallback(FTGInfo);
		}
		else // paint text(bitmap) with gdi
		{
			int j = i;
			FT_DRAW_STATE st = drState[i];
			while (++j < cbString && !Glyphs[j] && drState[j] == st) {};
			if (st == FT_DRAW_EMBEDDED_BITMAP)
				ORIG_ExtTextOutW(hdc, FTInfo.x, FTInfo.yTop, FTInfo.GetETO() & ETO_IGNORELANGUAGE, nullptr, lpString, j - i, FTInfo.lpDx ? FTInfo.lpDx + i : nullptr);
			else
				ORIG_ExtTextOutW(hdc, FTInfo.x, FTInfo.yTop, FTInfo.GetETO(), nullptr, lpString, j - i, FTInfo.lpDx ? FTInfo.lpDx + i : nullptr);
			lpString += --j - i;
			i = j;
		}
		//draw underline or strikeline separated

		if (lf.lfUnderline) {
			int yPos = FTInfo.yBase - decorationInfo_otm.otmsUnderscorePosition + FTInfo.yTop;
			if (yPos >= decorationInfo_height) {
				yPos = decorationInfo_height - 1;
			}
			cache.DrawHorizontalLine(FTInfo.x, yPos, FTInfo.Dx[i], FTInfo.Color(), decorationInfo_thickness);
		}

		if (lf.lfStrikeOut) {
			int yPos = FTInfo.yBase - decorationInfo_otm.otmsStrikeoutPosition + FTInfo.yTop;
			cache.DrawHorizontalLine(FTInfo.x, yPos, FTInfo.Dx[i], FTInfo.Color(), decorationInfo_thickness);
		}


		//draw line end.
		FTInfo.x = FTInfo.Dx[i];
		FTInfo.yTop = FTInfo.Dy[i];
	}

	return TRUE;
}

BOOL FreeTypeGetGlyph(	//获得所有图形和需要的宽度
	FreeTypeDrawInfo& FTInfo,
	LPCWSTR lpString,
	int cbString,
	int& width,
	FT_Referenced_Glyph* Glyphs,
	FT_DRAW_STATE* drState
)
{
	COwnedCriticalSectionLock __lock(1);
	{
		//CCriticalSectionLock __lock;
		if (!FreeTypePrepare(FTInfo))
			return false;
	}
	BOOL nRet = false;
	switch (FTInfo.rasterPolicy.fontLoader) {
	case SETTING_FONTLOADER_FREETYPE:
		nRet = ForEachGetGlyphFT(FTInfo, lpString, cbString, Glyphs, drState);
		break;
	case SETTING_FONTLOADER_WIN32:
		nRet = ForEachGetGlyphGGO(FTInfo, lpString, cbString, Glyphs, drState);
		break;
	}
	width = FTInfo.px;	//获得了宽度
	return nRet;
}



void VertFinalizer(void* object) {
	FT_Face face = static_cast<FT_Face>(object);
	ft2vert_final(face, static_cast<struct ft2vert_st*>(face->generic.data));
}

FT_Error face_requester(
	FTC_FaceID face_id,
	FT_Library /*library*/,
	FT_Pointer /*request_data*/,
	FT_Face* aface)
{
	FT_Error ret = FT_Err_Ok;
	FT_Face face;

	FreeTypeFontInfo* pfi = g_pFTEngine->FindFont(static_cast<int>(reinterpret_cast<INT_PTR>(face_id)));
	Assert(pfi);
	if (!pfi) {
		return FT_Err_Invalid_Argument;
	}
	LPCTSTR fontname = pfi->GetName();

	// 名称を指定してフォントを取得
	std::unique_ptr<FreeTypeSysFontData> fontData(
		FreeTypeSysFontData::CreateInstance(fontname, pfi->GetFontWeight(), pfi->IsItalic()));
	if (!fontData) {
		return FT_Err_Cannot_Open_Resource;
	}

	renderer_raii::UniqueFreeTypeFace faceOwner(fontData->GetFace());
	if (!faceOwner)
		return 0x6;	// FreeTypeSysFontData returned no face.
	// The FT_Stream close callback owns and deletes the stream holder after the
	// face crosses this C callback boundary.
	fontData.release();
	face = faceOwner.get();
	// Charmapを設定しておく
	ret = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
	if (ret != FT_Err_Ok)
		ret = FT_Select_Charmap(face, FT_ENCODING_MS_SYMBOL);

	if (ret != FT_Err_Ok)
	{
		return ret;
	}
	struct ft2vert_st* vert = ft2vert_init(face);
	face->generic.data = vert;
	face->generic.finalizer = VertFinalizer;

	// select named instance for variable font
	VarFontByAlias(face, pfi->GetStyleName());
	*aface = faceOwner.release();
	return 0;
}

//新太字アルゴリズム
FT_Error New_FT_Outline_Embolden(FT_Outline* outline, FT_Pos str_h, FT_Pos str_v, FT_Int font_size)
{
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	int orientation = 0;
	switch (pSettings->BolderMode()) {
	case 1:
		return FT_Outline_EmboldenXY(outline, str_h, 0);

	case 2:
		return FT_Outline_Embolden(outline, str_h);

	default:
	{
		if (!outline) return FT_Err_Invalid_Argument;
		if (font_size < FT_BOLD_LOW && str_h>32)
		{
			FT_Outline_EmboldenXY(outline, str_h, Min(long(32), str_v));
		}
		else
			FT_Outline_Embolden(outline, str_h);
		return FT_Err_Ok;
	}
	}
}

//横方向だけ太らせるFT_Outline_Embolden
FT_Error Old_FT_Outline_Embolden(FT_Outline* outline, FT_Pos strength)
{
	FT_Vector* points;
	FT_Vector	v_prev, v_first, v_next, v_cur;
	FT_Angle	rotate, angle_in, angle_out;
	FT_Int		c, n, first;
	FT_Int		orientation;

	if (!outline)
		return FT_Err_Invalid_Argument;

	strength /= 2;
	if (strength == 0)
		return FT_Err_Ok;

	orientation = FT_Outline_Get_Orientation(outline);
	if (orientation == FT_ORIENTATION_NONE)
	{
		if (outline->n_contours)
			return FT_Err_Invalid_Argument;
		else
			return FT_Err_Ok;
	}

	if (orientation == FT_ORIENTATION_TRUETYPE)
		rotate = -FT_ANGLE_PI2;
	else
		rotate = FT_ANGLE_PI2;

	points = outline->points;

	first = 0;
	for (c = 0; c < outline->n_contours; c++)
	{
		int  last = outline->contours[c];

		v_first = points[first];
		v_prev = points[last];
		v_cur = v_first;

		for (n = first; n <= last; n++)
		{
			FT_Vector	in, out;
			FT_Angle	angle_diff;
			FT_Pos		d;
			FT_Fixed	scale;

			if (n < last)
				v_next = points[n + 1];
			else
				v_next = v_first;

			/* compute the in and out vectors */
			in.x = v_cur.x - v_prev.x;
			in.y = v_cur.y - v_prev.y;

			out.x = v_next.x - v_cur.x;
			out.y = v_next.y - v_cur.y;

			angle_in = FT_Atan2(in.x, in.y);
			angle_out = FT_Atan2(out.x, out.y);
			angle_diff = FT_Angle_Diff(angle_in, angle_out);
			scale = FT_Cos(angle_diff / 2);

			if (scale < 0x4000L && scale > -0x4000L)
				in.x = in.y = 0;
			else
			{
				d = FT_DivFix(strength, scale);

				FT_Vector_From_Polar(&in, d, angle_in + angle_diff / 2 - rotate);
			}

			outline->points[n].x = v_cur.x + strength + in.x;
			//↓これをコメントアウトしただけ
			//outline->points[n].y = v_cur.y + strength + in.y;

			v_prev = v_cur;
			v_cur = v_next;
		}

		first = last + 1;
	}

	return FT_Err_Ok;
}

//こっちは縦方向
FT_Error Vert_FT_Outline_Embolden(FT_Outline* outline, FT_Pos strength)
{
	FT_Vector* points;
	FT_Vector	v_prev, v_first, v_next, v_cur;
	FT_Angle	rotate, angle_in, angle_out;
	FT_Int		c, n, first;
	FT_Int		orientation;

	if (!outline)
		return FT_Err_Invalid_Argument;

	strength /= 2;
	if (strength == 0)
		return FT_Err_Ok;

	orientation = FT_Outline_Get_Orientation(outline);
	if (orientation == FT_ORIENTATION_NONE)
	{
		if (outline->n_contours)
			return FT_Err_Invalid_Argument;
		else
			return FT_Err_Ok;
	}

	if (orientation == FT_ORIENTATION_TRUETYPE)
		rotate = -FT_ANGLE_PI2;
	else
		rotate = FT_ANGLE_PI2;

	points = outline->points;

	first = 0;
	for (c = 0; c < outline->n_contours; c++)
	{
		int  last = outline->contours[c];

		v_first = points[first];
		v_prev = points[last];
		v_cur = v_first;

		for (n = first; n <= last; n++)
		{
			FT_Vector	in, out;
			FT_Angle	angle_diff;
			FT_Pos		d;
			FT_Fixed	scale;

			if (n < last)
				v_next = points[n + 1];
			else
				v_next = v_first;

			/* compute the in and out vectors */
			in.x = v_cur.x - v_prev.x;
			in.y = v_cur.y - v_prev.y;

			out.x = v_next.x - v_cur.x;
			out.y = v_next.y - v_cur.y;

			angle_in = FT_Atan2(in.x, in.y);
			angle_out = FT_Atan2(out.x, out.y);
			angle_diff = FT_Angle_Diff(angle_in, angle_out);
			scale = FT_Cos(angle_diff / 2);

			if (scale < 0x4000L && scale > -0x4000L)
				in.x = in.y = 0;
			else
			{
				d = FT_DivFix(strength, scale);

				FT_Vector_From_Polar(&in, d, angle_in + angle_diff / 2 - rotate);
			}

			//outline->points[n].x = v_cur.x + strength + in.x;
			//↑これをコメントアウトしただけ
			outline->points[n].y = v_cur.y + strength + in.y;

			v_prev = v_cur;
			v_cur = v_next;
		}

		first = last + 1;
	}

	return FT_Err_Ok;
		}

using FreeTypeRuntimeResources = renderer::freetype::OrderedRuntimeOwners<
	renderer_raii::UniqueFreeTypeLibrary,
	renderer_raii::UniqueFreeTypeManager>;

static FreeTypeRuntimeResources& GetFreeTypeRuntimeResources()
{
	// Explicit unload calls FontLFree outside the loader lock. Process exit
	// leaves only this tiny state allocation for the OS, avoiding FreeType
	// teardown from a static destructor while Windows owns the loader lock.
	static FreeTypeRuntimeResources* resources = new FreeTypeRuntimeResources;
	return *resources;
}

BOOL FontLInit(void) {
	CCriticalSectionLock __lock;
	FreeTypeRuntimeResources& resources = GetFreeTypeRuntimeResources();
	if (resources.initialized())
	{
		freetype_library = resources.library();
		cache_man = resources.manager();
		return TRUE;
	}
	resources.Reset();
	freetype_library = nullptr;
	cache_man = nullptr;
	cmap_cache = nullptr;
	image_cache = nullptr;

	FT_Library rawLibrary = nullptr;
	if (FT_Init_FreeType(&rawLibrary)) {
		return FALSE;
	}
	renderer_raii::UniqueFreeTypeLibrary library(rawLibrary);

#ifdef INFINALITY
#define TT_INTERPRETER_VERSION_35  35
#define TT_INTERPRETER_VERSION_38  38
#define TT_INTERPRETER_VERSION_40  40
	FT_UInt     interpreter_version = TT_INTERPRETER_VERSION_38;
	FT_Property_Set(library.get(), "truetype", "interpreter-version", &interpreter_version);
#endif

	//enable stem darkening feature introduced in 2.6.2
	FT_Bool     no_stem_darkening = FALSE;
	FT_Property_Set(library.get(), "cff", "no-stem-darkening", &no_stem_darkening);
	renderer::RendererPolicyRef const policy =
		renderer::CurrentRendererPolicy();
	if (!policy)
		return FALSE;
	const renderer::freetype::StartupPolicy& startup = policy->free_type();
	FTC_Manager rawManager = nullptr;
	if (FTC_Manager_New(library.get(),
		startup.cacheMaxFaces,
		startup.cacheMaxSizes,
		startup.cacheMaxBytes,
		face_requester, nullptr,
		&rawManager))
	{
		return FALSE;
	}
	renderer_raii::UniqueFreeTypeManager manager(rawManager);
	FTC_CMapCache newCmapCache = nullptr;
	FTC_ImageCache newImageCache = nullptr;
	if (FTC_CMapCache_New(manager.get(), &newCmapCache)) {
		return FALSE;
	}
	if (FTC_ImageCache_New(manager.get(), &newImageCache)) {
		return FALSE;
	}

	resources.Publish(std::move(library), std::move(manager));
	freetype_library = resources.library();
	cache_man = resources.manager();
	cmap_cache = newCmapCache;
	image_cache = newImageCache;

	//s_AlphaBlendTable.init();
	s_AlphaBlendTable.initRGB();

	return TRUE;
}

void FontLFree(void) {
	CCriticalSectionLock __lock;
	FreeTypeRuntimeResources& resources = GetFreeTypeRuntimeResources();

	cache_man = nullptr;
	cmap_cache = nullptr;
	image_cache = nullptr;
	resources.Reset();
	freetype_library = nullptr;
}

//Snowie
void RefreshAlphaTable()
{
	s_AlphaBlendTable.init();
}

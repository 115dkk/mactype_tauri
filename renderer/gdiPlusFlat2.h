#ifndef _GDIPLUSFLAT_H
#define _GDIPLUSFLAT_H

#include <Windows.h>
#include <algorithm>

// The legacy SDK header uses unqualified min/max when NOMINMAX suppresses
// the Windows macros. Keep the std replacements inside this include.
#if defined(NOMINMAX) && !defined(min)
#define MACTYPE_UNDEFINE_GDIPLUS_MIN
#define min std::min
#endif
#if defined(NOMINMAX) && !defined(max)
#define MACTYPE_UNDEFINE_GDIPLUS_MAX
#define max std::max
#endif
#include "GdiPlusTypes2.h"
#ifdef MACTYPE_UNDEFINE_GDIPLUS_MAX
#undef max
#undef MACTYPE_UNDEFINE_GDIPLUS_MAX
#endif
#ifdef MACTYPE_UNDEFINE_GDIPLUS_MIN
#undef min
#undef MACTYPE_UNDEFINE_GDIPLUS_MIN
#endif
#include <GdiPlusEnums.h>
#include <GdiplusGpStubs.h>
#include <GdiPlusPixelFormats.h>

#define GDIPCONST const
#define WINGDIPAPI __stdcall
using GpStatus = Status;

using GdipDrawString = GpStatus (WINGDIPAPI *)(
			   GpGraphics               *graphics,
			   GDIPCONST WCHAR          *string,
			   INT                       length,
			   GDIPCONST GpFont         *font,
			   GDIPCONST RectF          *layoutRect,
			   GDIPCONST GpStringFormat *stringFormat,
			   GDIPCONST GpBrush        *brush
			   );//绘制文字的函数

using GdipGetLogFontW = GpStatus (WINGDIPAPI *)(GpFont * font, GpGraphics *graphics, LOGFONTW * logfontW);//从GPFont获取Logfont

using GdipGetDC = GpStatus (WINGDIPAPI *)(GpGraphics* graphics, HDC * hdc);	//从GPGraphic获取HDC

using GdipReleaseDC = GpStatus (WINGDIPAPI *)(GpGraphics* graphics, HDC hdc);

using GdipGetStringFormatAlign = GpStatus (WINGDIPAPI *)(GDIPCONST GpStringFormat *format,
						 StringAlignment *align);

using GdipGetStringFormatTrimming = GpStatus (WINGDIPAPI *)(
							GDIPCONST GpStringFormat *format,
							StringTrimming       *trimming
							);	//获取字符串的缩略方式。当字符串长度超过矩形长度时，将使用设置的方式删除中间的部分文字。
								//转换成GDI后应使用DrawText来模拟。
using GdipGetStringFormatHotkeyPrefix = GpStatus (WINGDIPAPI *)(GDIPCONST GpStringFormat *format,
								INT *hotkeyPrefix);	//获取&符号的显示方式，隐藏，显示下划线或者不处理&。由DrawText模拟。

using GdipGetBrushType = GpStatus (WINGDIPAPI *)(GpBrush *brush, GpBrushType *type);

using GdipGetSolidFillColor = GpStatus (WINGDIPAPI *)(GpSolidFill *brush, ARGB *color);	//获取单色Brush的颜色

extern GdipDrawString pfnGdipDrawString ;
extern GdipGetBrushType pfnGdipGetBrushType ;
extern GdipGetDC pfnGdipGetDC ;
extern GdipGetLogFontW pfnGdipGetLogFontW ;
extern GdipGetSolidFillColor pfnGdipGetSolidFillColor ;
extern GdipGetStringFormatAlign pfnGdipGetStringFormatAlign ;
extern GdipGetStringFormatHotkeyPrefix pfnGdipGetStringFormatHotkeyPrefix ;
extern GdipGetStringFormatTrimming pfnGdipGetStringFormatTrimming ;
extern GdipReleaseDC pfnGdipReleaseDC ;

bool InitGdiplusFuncs();

#endif

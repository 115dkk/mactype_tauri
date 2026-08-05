#include "gdiPlusFlat2.h"
#include <tchar.h>


GdipDrawString pfnGdipDrawString = NULL;
GdipGetBrushType pfnGdipGetBrushType = NULL;
GdipGetDC pfnGdipGetDC = NULL;
GdipGetLogFontW pfnGdipGetLogFontW = NULL;
GdipGetSolidFillColor pfnGdipGetSolidFillColor = NULL;
GdipGetStringFormatAlign pfnGdipGetStringFormatAlign = NULL;
GdipGetStringFormatHotkeyPrefix pfnGdipGetStringFormatHotkeyPrefix = NULL;
GdipGetStringFormatTrimming pfnGdipGetStringFormatTrimming = NULL;
GdipReleaseDC pfnGdipReleaseDC = NULL;

bool InitGdiplusFuncs(){
	static bool bInited = false;
	if (!bInited)
	{
		bInited = true;
		HMODULE	hGdiplusDll = GetModuleHandle(_T("Gdiplus.dll"));
		if (hGdiplusDll)
		{
			pfnGdipDrawString = reinterpret_cast<GdipDrawString>(GetProcAddress(hGdiplusDll, "GdipDrawString"));
			pfnGdipGetBrushType = reinterpret_cast<GdipGetBrushType>(GetProcAddress(hGdiplusDll, "GdipGetBrushType"));
			pfnGdipGetDC = reinterpret_cast<GdipGetDC>(GetProcAddress(hGdiplusDll, "GdipGetDC"));
			pfnGdipGetLogFontW = reinterpret_cast<GdipGetLogFontW>(GetProcAddress(hGdiplusDll, "GdipGetLogFontW"));
			pfnGdipGetSolidFillColor = reinterpret_cast<GdipGetSolidFillColor>(GetProcAddress(hGdiplusDll, "GdipGetSolidFillColor"));
			pfnGdipGetStringFormatAlign = reinterpret_cast<GdipGetStringFormatAlign>(GetProcAddress(hGdiplusDll, "GdipGetStringFormatAlign"));
			pfnGdipGetStringFormatHotkeyPrefix = reinterpret_cast<GdipGetStringFormatHotkeyPrefix>(GetProcAddress(hGdiplusDll, "GdipGetStringFormatHotkeyPrefix"));
			pfnGdipGetStringFormatTrimming = reinterpret_cast<GdipGetStringFormatTrimming>(GetProcAddress(hGdiplusDll, "GdipGetStringFormatTrimming"));
			pfnGdipReleaseDC = reinterpret_cast<GdipReleaseDC>(GetProcAddress(hGdiplusDll, "GdipReleaseDC"));
			return true;
		}
		else
			return false;
	}
	else
		return true;
}

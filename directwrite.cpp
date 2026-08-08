#include "directwrite.h"
#include "directwrite_alias.h"
#include "dynCodeHelper.h"
#include "hookCounter.h"
#include "settings.h"

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

void MyDebug(const TCHAR *sz, ...)
{
#ifdef DEBUG
	TCHAR szData[512] = { 0 };

	va_list args;
	va_start(args, sz);
	StringCchVPrintf(szData, sizeof(szData)-1, sz, args);
	va_end(args);

	OutputDebugString(szData);
#endif
}

template <typename Target, typename Source>
void SetPointerValue(Target& target, Source source) noexcept
{
	static_assert(sizeof(Target) == sizeof(Source), "hook pointer size mismatch");
	memcpy(&target, &source, sizeof(target));
}

#define SET_VAL(x, y) SetPointerValue((x), (y))
// To hook a method, add HOOK_MANUALLY() in hooklist.h and use this.

#ifdef EASYHOOK
#define ISHOOKED(name) (!!HOOK_##name.Link)
#else
#define ISHOOKED(name) (IsHooked_##name)
#endif

#define HOOK(obj, name, index)                                                 \
	{                                                                          \
		if (!ISHOOKED(name))                                                   \
		{                                                                      \
			AutoEnableDynamicCodeGen dynHelper(true);                          \
			SET_VAL(ORIG_##name, (*reinterpret_cast<void ***>(obj.p))[index]); \
			hook_demand_##name(false);                                         \
			if (!ISHOOKED(name))                                               \
			{                                                                  \
				MyDebug(L"##name hook failed");                                \
			}                                                                  \
		}                                                                      \
	};

static void SignalDirectWriteDiagnostic(WCHAR const* stage);
static void SignalDirectWriteFamilyDiagnostic(
	WCHAR const* stagePrefix, WCHAR const* familyName);

struct ComMethodHooker {
	// The target function if it has been hooked
	BOOL (*lpIsHooked)();
	// The method the vftable refers to
	void*(*lpGetMethod)(IUnknown* obj);
	// Hook the method
	void(*lpHookFunc)(IUnknown* obj);
};

#define COM_METHOD_HOOKER(type, name, index) ComMethodHooker { \
	[]() -> BOOL { \
		return ISHOOKED(name); \
	}, \
	[](IUnknown* obj) -> void* { \
		return (*reinterpret_cast<void***>(obj))[index]; \
	}, \
	[](IUnknown* obj) -> void { \
		CComPtr<type> ptr = static_cast<type*>(obj); \
		HOOK(ptr, name, index); \
	} \
}

#define COM_METHOD_HOOKER_EMPTY() ComMethodHooker { \
	[]() -> BOOL { \
		return false; \
	}, \
	[](IUnknown* obj) -> void* { \
		return nullptr; \
	}, \
	[](IUnknown* obj) -> void { \
		return; \
	} \
}

struct Params {
	D2D1_TEXT_ANTIALIAS_MODE AntialiasMode;
	// Don't access directly. Use Get(D2D|DW)RenderingParams().
	IDWriteRenderingParams *RenderingParams;

	FLOAT Gamma;
	FLOAT EnhancedContrast;
	FLOAT ClearTypeLevel;
	DWRITE_PIXEL_GEOMETRY PixelGeometry;
	// RenderingMode=6 is invalid for DWrite interface
	DWRITE_RENDERING_MODE RenderingMode;
	FLOAT GrayscaleEnhancedContrast;
	DWRITE_GRID_FIT_MODE GridFitMode;
	DWRITE_RENDERING_MODE1 RenderingMode1;

	Params();
	void CreateParams(IDWriteFactory* dw_factory);
};

//IDWriteFactory* g_pDWriteFactory = nullptr;
enum D2D1RenderTargetCategory {
	D2D1_RENDER_TARGET_CATEGORY = 1,
	// ID2D1DCRenderTarget, ID2D1HwndRenderTarget, ID2D1BitmapRenderTarget
	D2D1_RENDER_TARGET1_CATEGORY,
	D2D1_DEVICE_CONTEXT_CATEGORY
};

template<typename Intf>
inline HRESULT IfSupport(IUnknown* pUnknown, void(*lpFunc)(Intf*)) {
	CComPtr<Intf> comObject;
	HRESULT hr = pUnknown->QueryInterface(&comObject);
	if (SUCCEEDED(hr)) {
		lpFunc(comObject);
	}
	return hr;
}

void Params::CreateParams(IDWriteFactory *dw_factory)
{
	IDWriteFactory3* dw3 = nullptr;
	IDWriteFactory2* dw2 = nullptr;
	IDWriteFactory1* dw1 = nullptr;
	IDWriteRenderingParams3* r3 = nullptr;
	IDWriteRenderingParams2* r2 = nullptr;
	IDWriteRenderingParams1* r1 = nullptr;
	IDWriteRenderingParams* r0 = nullptr;

	CComPtr<IDWriteFactory> pDWriteFactory;
	if (nullptr == dw_factory) {
		ORIG_DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
			__uuidof(IDWriteFactory),
			reinterpret_cast<IUnknown**>(&pDWriteFactory));
		dw_factory = pDWriteFactory;
	}

	HRESULT hr = dw_factory->QueryInterface(&dw3);
	if SUCCEEDED(hr) {
		hr = dw3->CreateCustomRenderingParams(
			this->Gamma,
			this->EnhancedContrast,
			this->GrayscaleEnhancedContrast,
			this->ClearTypeLevel,
			this->PixelGeometry,
			this->RenderingMode1,
			this->GridFitMode,
			&r3);
		dw3->Release();
		if SUCCEEDED(hr) {
			RenderingParams = r3;
			return;
		}
	}

	hr = dw_factory->QueryInterface(&dw2);
	if SUCCEEDED(hr) {
		hr = dw2->CreateCustomRenderingParams(
			this->Gamma,
			this->EnhancedContrast,
			this->GrayscaleEnhancedContrast,
			this->ClearTypeLevel,
			this->PixelGeometry,
			this->RenderingMode,
			this->GridFitMode,
			&r2);
		dw2->Release();
		if SUCCEEDED(hr) {
			RenderingParams = r2;
			return;
		}
	}

	hr = dw_factory->QueryInterface(&dw1);
	if SUCCEEDED(hr) {
		hr = dw1->CreateCustomRenderingParams(
			this->Gamma,
			this->EnhancedContrast,
			this->GrayscaleEnhancedContrast,
			this->ClearTypeLevel,
			this->PixelGeometry,
			this->RenderingMode,
			&r1);
		dw1->Release();
		if SUCCEEDED(hr) {
			RenderingParams = r1;
			return;
		}
	}

	hr = dw_factory->CreateCustomRenderingParams(
		this->Gamma,
		this->EnhancedContrast,
		this->ClearTypeLevel,
		this->PixelGeometry,
		this->RenderingMode,
		&r0);
	if (SUCCEEDED(hr)) {
		RenderingParams = r0;
		return;
	}

	RenderingParams = nullptr;
}

Params::Params() {
	//MessageBox(nullptr, L"MakeParam", nullptr, MB_OK);
	const CGdippSettings* pSettings = CGdippSettings::GetInstanceNoInit();
	//
	Gamma = pSettings->GammaValueForDW();	//user defined value preferred.
	//if (Gamma == 0)
	//	Gamma = pSettings->GammaValue()*pSettings->GammaValue() > 1.3 ? pSettings->GammaValue()*pSettings->GammaValue() / 2 : 0.7f;
	EnhancedContrast = pSettings->ContrastForDW();
	ClearTypeLevel = pSettings->ClearTypeLevelForDW();
	AntialiasMode = static_cast<D2D1_TEXT_ANTIALIAS_MODE>(D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
	switch (pSettings->AntiAliasModeForDW())
	{
		case 2:
		case 4:
			PixelGeometry = DWRITE_PIXEL_GEOMETRY_RGB;
			break;
		case 3:
		case 5:
			PixelGeometry = DWRITE_PIXEL_GEOMETRY_BGR;
			break;
		default:
			PixelGeometry = DWRITE_PIXEL_GEOMETRY_FLAT;
			AntialiasMode = D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
	}

	RenderingMode = static_cast<DWRITE_RENDERING_MODE>(pSettings->RenderingModeForDW());
	GrayscaleEnhancedContrast = pSettings->ContrastForDW();
	switch (pSettings->GetFontSettings().GetHintingMode())
	{
		case 0: GridFitMode = DWRITE_GRID_FIT_MODE_DEFAULT;
			break;
		case 1: GridFitMode = DWRITE_GRID_FIT_MODE_DISABLED;
			break;
		default:
			GridFitMode = DWRITE_GRID_FIT_MODE_ENABLED;
			break;
	}
	RenderingMode1 = static_cast<DWRITE_RENDERING_MODE1>(pSettings->RenderingModeForDW());
	RenderingParams = nullptr;
}

Params* GetD2DParams() {
	static Params d2dParams;
	return &d2dParams;
}

Params* GetDWParams() {
	static Params dwParams = [] {
		const CGdippSettings* pSettings = CGdippSettings::GetInstanceNoInit();

		Params p;
		if (pSettings->RenderingModeForDW() == 6) {	//DW rendering in mode6 is horrible
			p.RenderingMode = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
			p.RenderingMode1 = DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC;
		}
		//g_DWParams.Gamma = powf(g_D2DParams.Gamma, 1.0 / 3.0);
		return p;
	}();
	return &dwParams;
}

IDWriteRenderingParams* GetD2DRenderingParams(IDWriteRenderingParams* fallback) {
	Params* params = GetD2DParams();
	static bool inited = [&] {
		params->CreateParams(nullptr);
		return true;
	}();

	if (params->RenderingParams)
		return params->RenderingParams;
	else
		return fallback;
}

IDWriteRenderingParams* GetDWRenderingParams(IDWriteRenderingParams* fallback) {
	Params* params = GetDWParams();
	static bool inited = [&] {
		params->CreateParams(nullptr);
		return true;
	}();

	if (params->RenderingParams)
		return params->RenderingParams;
	else
		return fallback;
}

// Hook the implementation rather than an interface.
// There are many versions but inside the DLL there's only one implementation
// that supports the latest available one. Older versions are just upcasts,
// they share the same vftable.
void HookFactory(ID2D1Factory* pD2D1Factory) {
	static bool loaded = [&] {
		HRESULT hr;
		CComPtr<ID2D1Factory> ptr = pD2D1Factory;
		// index is the index in the vftable. Their order is the same as
		// methods declared in the header.
		HOOK(ptr, CreateWicBitmapRenderTarget, 13);
		HOOK(ptr, CreateHwndRenderTarget, 14);
		//HOOK(ptr, CreateDxgiSurfaceRenderTarget, 15);
		HOOK(ptr, CreateDCRenderTarget, 16);
		MyDebug(L"ID2D1Factory hooked");

		CComPtr<ID2D1Factory1> ptr1;
		hr = pD2D1Factory->QueryInterface(&ptr1);
		if (SUCCEEDED(hr)){
			HOOK(ptr1, CreateDevice1, 17);
			MyDebug(L"ID2D1Factory1 hooked");
		}

		CComPtr<ID2D1Factory2> ptr2;
		hr = pD2D1Factory->QueryInterface(&ptr2);
		if (SUCCEEDED(hr)){
			HOOK(ptr2, CreateDevice2, 27);
			MyDebug(L"ID2D1Factory2 hooked");
		}

		CComPtr<ID2D1Factory3> ptr3;
		hr = pD2D1Factory->QueryInterface(&ptr3);
		if (SUCCEEDED(hr)){
			HOOK(ptr3, CreateDevice3, 28);
			MyDebug(L"ID2D1Factory3 hooked");
		}

		CComPtr<ID2D1Factory4> ptr4;
		hr = pD2D1Factory->QueryInterface(&ptr4);
		if (SUCCEEDED(hr)){
			HOOK(ptr4, CreateDevice4, 29);
			MyDebug(L"ID2D1Factory4 hooked");
		}

		CComPtr<ID2D1Factory5> ptr5;
		hr = pD2D1Factory->QueryInterface(&ptr5);
		if (SUCCEEDED(hr)){
			HOOK(ptr5, CreateDevice5, 30);
			MyDebug(L"ID2D1Factory5 hooked");
		}

		CComPtr<ID2D1Factory6> ptr6;
		hr = pD2D1Factory->QueryInterface(&ptr6);
		if (SUCCEEDED(hr)){
			HOOK(ptr6, CreateDevice6, 31);
			MyDebug(L"ID2D1Factory6 hooked");
		}

		CComPtr<ID2D1Factory7> ptr7;
		hr = pD2D1Factory->QueryInterface(&ptr7);
		if (SUCCEEDED(hr)){
			HOOK(ptr7, CreateDevice7, 32);
			MyDebug(L"ID2D1Factory7 hooked");
		}
		return true;
	}();
}

void HookDevice(ID2D1Device* d2dDevice){
	static bool loaded = [&] {
		CComPtr<ID2D1Device> ptr = d2dDevice;
		HOOK(ptr, CreateDeviceContext, 4);
		MyDebug(L"ID2D1Device hooked");

		CComPtr<ID2D1Device1> ptr2;
		HRESULT hr = (d2dDevice)->QueryInterface(&ptr2);
		if SUCCEEDED(hr) {
			HOOK(ptr2, CreateDeviceContext2, 11);
			MyDebug(L"ID2D1Device1 hooked");
		}
		CComPtr<ID2D1Device2> ptr3;
		hr = (d2dDevice)->QueryInterface(&ptr3);
		if SUCCEEDED(hr) {
			HOOK(ptr3, CreateDeviceContext3, 12);
			MyDebug(L"ID2D1Device2 hooked");
		}
		CComPtr<ID2D1Device3> ptr4;
		hr = (d2dDevice)->QueryInterface(&ptr4);
		if SUCCEEDED(hr) {
			HOOK(ptr4, CreateDeviceContext4, 15);
			MyDebug(L"ID2D1Device3 hooked");
		}
		CComPtr<ID2D1Device4> ptr5;
		hr = (d2dDevice)->QueryInterface(&ptr5);
		if SUCCEEDED(hr) {
			HOOK(ptr5, CreateDeviceContext5, 16);
			MyDebug(L"ID2D1Device4 hooked");
		}
		CComPtr<ID2D1Device5> ptr6;
		hr = (d2dDevice)->QueryInterface(&ptr6);
		if SUCCEEDED(hr) {
			HOOK(ptr6, CreateDeviceContext6, 19);
			MyDebug(L"ID2D1Device5 hooked");
		}
		CComPtr<ID2D1Device6> ptr7;
		hr = (d2dDevice)->QueryInterface(&ptr7);
		if SUCCEEDED(hr) {
			HOOK(ptr7, CreateDeviceContext7, 20);
			MyDebug(L"ID2D1Device6 hooked");
		}
		return true;
	}();
}

// Hook the method if it has not been hooked. It's not thread safe.
void HookRenderTargetMethod(
	ID2D1RenderTarget* pD2D1RenderTarget,
	D2D1RenderTargetCategory hookCategory,
	ComMethodHooker methodHookers[]
	) {
	void* method = methodHookers[hookCategory].lpGetMethod(pD2D1RenderTarget);
	if (!method || methodHookers[hookCategory].lpIsHooked()) return;	// fn is not available or already hooked

	methodHookers[hookCategory].lpHookFunc(pD2D1RenderTarget);
}

void HookRenderTarget(
	ID2D1RenderTarget* pD2D1RenderTarget,
	D2D1RenderTargetCategory hookCategory
	){
	MyDebug(L"HookRenderTarget %d", hookCategory);
	static bool loaded = [&] {
		CComPtr<ID2D1RenderTarget> ptr = pD2D1RenderTarget;
		HOOK(ptr, CreateCompatibleRenderTarget, 12);
		HOOK(ptr, D2D1RenderTarget_DrawTextLayout, 28);

		ID2D1Factory* pD2D1Factory;
		pD2D1RenderTarget->GetFactory(&pD2D1Factory);
		if (pD2D1Factory)
			HookFactory(pD2D1Factory);

		// Actually it always implements ID2D1DeviceContext regardless of
		// hookCategory.
		CComPtr<ID2D1DeviceContext> ptr1;
		HRESULT hr = pD2D1RenderTarget->QueryInterface(&ptr1);
		if (SUCCEEDED(hr)) {
			ID2D1Device* pD2D1Device;
			ptr1->GetDevice(&pD2D1Device);
			if (pD2D1Device)
				HookDevice(pD2D1Device);
		}
		return true;
	}();

	// Some methods are duplicated across interfaces. Hook them whenever they're
	// available from an instance. Make sure don't hook the same function
	// multiple times.
	//
	// Up to two instances of the same function
	static ComMethodHooker hookDrawText[] = {
		COM_METHOD_HOOKER_EMPTY(),
		COM_METHOD_HOOKER(ID2D1RenderTarget, D2D1RenderTarget_DrawText, 27),
		COM_METHOD_HOOKER(ID2D1RenderTarget, D2D1RenderTarget_DrawText, 27),
		COM_METHOD_HOOKER(ID2D1DeviceContext, D2D1DeviceContext_DrawText, 27)
	};
	// Up to three instances of the same function
	static ComMethodHooker hookDrawGlyphRun[] = {
		COM_METHOD_HOOKER_EMPTY(),
		COM_METHOD_HOOKER(ID2D1RenderTarget, D2D1RenderTarget_DrawGlyphRun, 29),
		COM_METHOD_HOOKER(ID2D1RenderTarget, D2D1RenderTarget1_DrawGlyphRun, 29),
		COM_METHOD_HOOKER(ID2D1DeviceContext, D2D1DeviceContext_DrawGlyphRun, 29)
	};
	static ComMethodHooker hookSetTextAntialiasMode[] = {
		COM_METHOD_HOOKER_EMPTY(),
		COM_METHOD_HOOKER(ID2D1RenderTarget, D2D1RenderTarget_SetTextAntialiasMode, 34),
		COM_METHOD_HOOKER(ID2D1RenderTarget, D2D1RenderTarget_SetTextAntialiasMode, 34),
		COM_METHOD_HOOKER(ID2D1DeviceContext, D2D1DeviceContext_SetTextAntialiasMode, 34)
	};
	static ComMethodHooker hookSetTextRenderingParams[] = {
		COM_METHOD_HOOKER_EMPTY(),
		COM_METHOD_HOOKER(ID2D1RenderTarget, D2D1RenderTarget_SetTextRenderingParams, 36),
		COM_METHOD_HOOKER(ID2D1RenderTarget, D2D1RenderTarget_SetTextRenderingParams, 36),
		COM_METHOD_HOOKER(ID2D1DeviceContext, D2D1DeviceContext_SetTextRenderingParams, 36)
	};
	// Note that there's a branch in the inheritance hierarchy in
	// ID2D1RenderTarget. But the implementation always supports
	// ID2D1DeviceContext, thus multiple inheritance should take place. The
	// vftable for ID2D1DeviceContext is different if hookCategory is not
	// D2D1_DEVICE_CONTEXT_CATEGORY. It consists of thunks each of which adjusts
	// the this pointer and then jumps to the corresponding method for
	// ID2D1RenderTarget (So we don't have to hook them as well), and the other
	// functions for ID2D1DeviceContext.
	static ComMethodHooker hookDrawGlyphRun1[] = {
		COM_METHOD_HOOKER_EMPTY(),
		COM_METHOD_HOOKER(ID2D1RenderTarget, D2D1RenderTarget_DrawGlyphRun1, 82),
		COM_METHOD_HOOKER(ID2D1RenderTarget, D2D1RenderTarget1_DrawGlyphRun1, 82),
		COM_METHOD_HOOKER(ID2D1DeviceContext, D2D1DeviceContext_DrawGlyphRun1, 82)
	};

	if (hookCategory == D2D1_RENDER_TARGET_CATEGORY) {
		static bool loaded1 = [&] {
			CCriticalSectionLock __lock(CCriticalSectionLock::CS_DWRITE);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookDrawText);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookDrawGlyphRun);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookSetTextAntialiasMode);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookSetTextRenderingParams);

			CComPtr<ID2D1DeviceContext> ptr;
			HRESULT hr = pD2D1RenderTarget->QueryInterface(&ptr);
			if (SUCCEEDED(hr)) {
				HookRenderTargetMethod(ptr, hookCategory, hookDrawGlyphRun1);
			}
			return true;
		}();
	} else if (hookCategory == D2D1_RENDER_TARGET1_CATEGORY) {
		static bool loaded2 = [&] {
			CCriticalSectionLock __lock(CCriticalSectionLock::CS_DWRITE);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookDrawText);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookDrawGlyphRun);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookSetTextAntialiasMode);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookSetTextRenderingParams);

			CComPtr<ID2D1DeviceContext> ptr;
			HRESULT hr = pD2D1RenderTarget->QueryInterface(&ptr);
			if (SUCCEEDED(hr)) {
				HookRenderTargetMethod(ptr, hookCategory, hookDrawGlyphRun1);
			}
			return true;
		}();
	} else if (hookCategory == D2D1_DEVICE_CONTEXT_CATEGORY) {
		static bool loaded3 = [&] {
			CCriticalSectionLock __lock(CCriticalSectionLock::CS_DWRITE);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookDrawText);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookDrawGlyphRun);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookSetTextAntialiasMode);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookSetTextRenderingParams);
			HookRenderTargetMethod(pD2D1RenderTarget, hookCategory, hookDrawGlyphRun1);
			return true;
		}();
	}

	//pD2D1RenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
	pD2D1RenderTarget->SetTextAntialiasMode(GetD2DParams()->AntialiasMode);
	if (GetD2DRenderingParams(nullptr)) {
		pD2D1RenderTarget->SetTextRenderingParams(GetD2DRenderingParams(nullptr));
	}
}

//DWrite hooks
HRESULT WINAPI IMPL_CreateGlyphRunAnalysis(
	IDWriteFactory* This,
	DWRITE_GLYPH_RUN const* glyphRun,
	FLOAT pixelsPerDip,
	DWRITE_MATRIX const* transform,
	DWRITE_RENDERING_MODE renderingMode,
	DWRITE_MEASURING_MODE measuringMode,
	FLOAT baselineOriginX,
	FLOAT baselineOriginY,
	IDWriteGlyphRunAnalysis** glyphRunAnalysis)
{
	HRESULT hr = E_FAIL;
	if (FAILED(hr) && renderingMode != DWRITE_RENDERING_MODE_ALIASED) {
		MyDebug(L"Try DW2");
		IDWriteFactory2* f;
		hr = This->QueryInterface(&f);
		if (SUCCEEDED(hr)) {
			DWRITE_MATRIX m = {};
			if (transform) {
				m = *transform;
				m.m11 *= pixelsPerDip;
				m.m12 *= pixelsPerDip;
				m.m21 *= pixelsPerDip;
				m.m22 *= pixelsPerDip;
				m.dx *= pixelsPerDip;
				m.dy *= pixelsPerDip;
			}
			else {
				m.m11 = pixelsPerDip;
				m.m22 = pixelsPerDip;
			}
			hr = f->CreateGlyphRunAnalysis(
				glyphRun,
				&m,
				renderingMode,
				measuringMode,
				DWRITE_GRID_FIT_MODE_DEFAULT,
				DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE,
				baselineOriginX,
				baselineOriginY,
				glyphRunAnalysis
				);
			f->Release();
		}
	}

	if (FAILED(hr) && renderingMode != DWRITE_RENDERING_MODE_ALIASED) {
		MyDebug(L"Try DW1");
		Params* dwParams = GetDWParams();
		DWRITE_MATRIX m;
		DWRITE_MATRIX const* pm = transform;
		if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
			if (transform) {
				m = *transform;
				m.m12 += 1.0f / 0xFFFF;
				m.m21 += 1.0f / 0xFFFF;
			}
			else {
				m = { 1, 1.0f / 0xFFFF, 1.0f / 0xFFFF, 1 };
			}
			pm = &m;
		}
		hr = ORIG_CreateGlyphRunAnalysis(
			This,
			glyphRun,
			pixelsPerDip,
			pm,
			dwParams->RenderingMode,
			measuringMode,
			baselineOriginX,
			baselineOriginY,
			glyphRunAnalysis
			);
	}
	if (FAILED(hr)) {
		MyDebug(L"Try Original Params");
		hr = ORIG_CreateGlyphRunAnalysis(
			This,
			glyphRun,
			pixelsPerDip,
			transform,
			renderingMode,
			measuringMode,
			baselineOriginX,
			baselineOriginY,
			glyphRunAnalysis
			);
	}
	if (SUCCEEDED(hr)) {
		MyDebug(L"CreateGlyphRunAnalysis hooked");
		static bool loaded = [&] {
			CComPtr<IDWriteGlyphRunAnalysis> ptr = *glyphRunAnalysis;
			HOOK(ptr, GetAlphaBlendParams, 5);
			return true;
		}();
	}
	return hr;
}

HRESULT WINAPI IMPL_GetGdiInterop(
	IDWriteFactory* This,
	IDWriteGdiInterop** gdiInterop
	)
{
	HRESULT hr = ORIG_GetGdiInterop(This, gdiInterop);
	static bool loaded = [&] {
		CComPtr<IDWriteGdiInterop> gdip = *gdiInterop;
		HOOK(gdip, CreateBitmapRenderTarget, 7);
		return true;
	}();
	MyDebug(L"IMPL_GetGdiInterop hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateBitmapRenderTarget(
	IDWriteGdiInterop* This,
	HDC hdc,
	UINT32 width,
	UINT32 height,
	IDWriteBitmapRenderTarget** renderTarget
	)
{
	HRESULT hr = ORIG_CreateBitmapRenderTarget(
		This,
		hdc,
		width,
		height,
		renderTarget
		);
	if (SUCCEEDED(hr)) {
		static bool loaded = [&] {
			CComPtr<IDWriteBitmapRenderTarget> ptr = *renderTarget;
			HOOK(ptr, BitmapRenderTarget_DrawGlyphRun, 3);
			return true;
		}();
	}
	MyDebug(L"CreateBitmapRenderTarget hooked");
	return hr;
}

HRESULT WINAPI IMPL_GetAlphaBlendParams(
	IDWriteGlyphRunAnalysis* This,
	IDWriteRenderingParams* renderingParams,
	FLOAT* blendGamma,
	FLOAT* blendEnhancedContrast,
	FLOAT* blendClearTypeLevel
	)
{
	HRESULT hr = E_FAIL;
	if (FAILED(hr)) {
		hr = ORIG_GetAlphaBlendParams(
			This,
			GetDWRenderingParams(renderingParams),
			blendGamma,
			blendEnhancedContrast,
			blendClearTypeLevel
			);
	}
	if (FAILED(hr)) {
		hr = ORIG_GetAlphaBlendParams(
			This,
			renderingParams,
			blendGamma,
			blendEnhancedContrast,
			blendClearTypeLevel
			);
	}
	MyDebug(L"GetAlphaBlendParams hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateGlyphRunAnalysis2(
	IDWriteFactory2* This,
	DWRITE_GLYPH_RUN const* glyphRun,
	DWRITE_MATRIX const* transform,
	DWRITE_RENDERING_MODE renderingMode,
	DWRITE_MEASURING_MODE measuringMode,
	DWRITE_GRID_FIT_MODE gridFitMode,
	DWRITE_TEXT_ANTIALIAS_MODE antialiasMode,
	FLOAT baselineOriginX,
	FLOAT baselineOriginY,
	IDWriteGlyphRunAnalysis** glyphRunAnalysis
	)
{
	HRESULT hr = E_FAIL;
	if (FAILED(hr) && renderingMode != DWRITE_RENDERING_MODE_ALIASED) {
		IDWriteFactory3* f;
		hr = This->QueryInterface(&f);
		if (SUCCEEDED(hr)) {
			hr = f->CreateGlyphRunAnalysis(
				glyphRun,
				transform,
				static_cast<DWRITE_RENDERING_MODE1>(renderingMode),
				measuringMode,
				gridFitMode,
				antialiasMode,
				baselineOriginX,
				baselineOriginY,
				glyphRunAnalysis
				);
			f->Release();
		}
	}

	Params* dwParams = GetDWParams();
	if (FAILED(hr) && renderingMode != DWRITE_RENDERING_MODE_ALIASED) {
		hr = ORIG_CreateGlyphRunAnalysis2(
			This,
			glyphRun,
			transform,
			dwParams->RenderingMode,
			measuringMode,
			dwParams->GridFitMode,
			antialiasMode,
			baselineOriginX,
			baselineOriginY,
			glyphRunAnalysis
			);
	}
	if (FAILED(hr)) {
		DWRITE_MATRIX m = {};
		DWRITE_MATRIX const* pm = transform;
		if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
			if (transform) {
				m = *transform;
				m.m12 += 1.0f / 0xFFFF;
				m.m21 += 1.0f / 0xFFFF;
			}
			else {
				m = { 1, 1.0f / 0xFFFF, 1.0f / 0xFFFF, 1 };
			}
			pm = &m;
		}
		hr = ORIG_CreateGlyphRunAnalysis2(
			This,
			glyphRun,
			pm,
			renderingMode,
			measuringMode,
			gridFitMode,
			antialiasMode,
			baselineOriginX,
			baselineOriginY,
			glyphRunAnalysis
			);
	}
	if (FAILED(hr)) {
		hr = ORIG_CreateGlyphRunAnalysis2(
			This,
			glyphRun,
			transform,
			renderingMode,
			measuringMode,
			gridFitMode,
			antialiasMode,
			baselineOriginX,
			baselineOriginY,
			glyphRunAnalysis
			);
	}
	if (SUCCEEDED(hr)) {
		MyDebug(L"CreateGlyphRunAnalysis2 hooked");
		static bool loaded = [&] {
			CComPtr<IDWriteGlyphRunAnalysis> ptr = *glyphRunAnalysis;
			HOOK(ptr, GetAlphaBlendParams, 5);
			return true;
		}();
	}
	return hr;
}

HRESULT WINAPI IMPL_CreateGlyphRunAnalysis3(
	IDWriteFactory3* This,
	DWRITE_GLYPH_RUN const* glyphRun,
	DWRITE_MATRIX const* transform,
	DWRITE_RENDERING_MODE1 renderingMode,
	DWRITE_MEASURING_MODE measuringMode,
	DWRITE_GRID_FIT_MODE gridFitMode,
	DWRITE_TEXT_ANTIALIAS_MODE antialiasMode,
	FLOAT baselineOriginX,
	FLOAT baselineOriginY,
	IDWriteGlyphRunAnalysis** glyphRunAnalysis
	)
{
	MyDebug(L"CreateGlyphRunAnalysis3 hooked");
	Params* dwParams = GetDWParams();
	HRESULT hr = E_FAIL;
	if (FAILED(hr) && renderingMode != DWRITE_RENDERING_MODE1_ALIASED) {
		hr = ORIG_CreateGlyphRunAnalysis3(
			This,
			glyphRun,
			transform,
			dwParams->RenderingMode1,
			measuringMode,
			dwParams->GridFitMode,
			antialiasMode,
			baselineOriginX,
			baselineOriginY,
			glyphRunAnalysis
			);
	}
	if (FAILED(hr)) {
		MyDebug(L"try again with only transformation");
		DWRITE_MATRIX m = {};
		DWRITE_MATRIX const* pm = transform;
		if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
			if (transform) {
				m = *transform;
				m.m12 += 1.0f / 0xFFFF;
				m.m21 += 1.0f / 0xFFFF;
			}
			else {
				m = { 1, 1.0f / 0xFFFF, 1.0f / 0xFFFF, 1 };
			}
			pm = &m;
		}
		hr = ORIG_CreateGlyphRunAnalysis3(
			This,
			glyphRun,
			pm,
			renderingMode,
			measuringMode,
			gridFitMode,
			antialiasMode,
			baselineOriginX,
			baselineOriginY,
			glyphRunAnalysis
			);
	}
	if (FAILED(hr)) {
		MyDebug(L"fallback to original params");
		hr = ORIG_CreateGlyphRunAnalysis3(
			This,
			glyphRun,
			transform,
			renderingMode,
			measuringMode,
			gridFitMode,
			antialiasMode,
			baselineOriginX,
			baselineOriginY,
			glyphRunAnalysis
			);
	}
	if (SUCCEEDED(hr)) {
		MyDebug(L"CreateGlyphRunAnalysis3 hooked");
		static bool loaded = [&] {
			CComPtr<IDWriteGlyphRunAnalysis> ptr = *glyphRunAnalysis;
			HOOK(ptr, GetAlphaBlendParams, 5);
			return true;
		}();
	}
	return hr;
}


//d2d1 hooks
HRESULT WINAPI IMPL_D2D1CreateDevice(
	IDXGIDevice* dxgiDevice,
	CONST D2D1_CREATION_PROPERTIES* creationProperties,
	ID2D1Device** d2dDevice) {
	HRESULT hr = ORIG_D2D1CreateDevice(
		dxgiDevice,
		creationProperties,
		d2dDevice
		);
	if (SUCCEEDED(hr)) {
		HookDevice(*d2dDevice);
	}
	MyDebug(L"IMPL_D2D1CreateDevice hooked");
	return hr;
}

HRESULT WINAPI IMPL_D2D1CreateDeviceContext(
	IDXGISurface* dxgiSurface,
	CONST D2D1_CREATION_PROPERTIES* creationProperties,
	ID2D1DeviceContext** d2dDeviceContext){
	HRESULT hr = ORIG_D2D1CreateDeviceContext(
		dxgiSurface,
		creationProperties,
		d2dDeviceContext
		);
	if SUCCEEDED(hr) {
		HookRenderTarget(*d2dDeviceContext, D2D1_DEVICE_CONTEXT_CATEGORY);
	}
	MyDebug(L"IMPL_D2D1CreateDeviceContext hooked");
	return hr;
}

HRESULT WINAPI IMPL_D2D1CreateFactory(
	D2D1_FACTORY_TYPE factoryType,
	REFIID riid,
	const D2D1_FACTORY_OPTIONS* pFactoryOptions,
	void** ppIFactory){
	HRESULT hr = ORIG_D2D1CreateFactory(factoryType, riid, pFactoryOptions, ppIFactory);
	if (SUCCEEDED(hr)) {
		auto pUnknown = reinterpret_cast<IUnknown*>(*ppIFactory);
		ID2D1Factory* pD2D1Factory;
		HRESULT hr2 = pUnknown->QueryInterface(&pD2D1Factory);
		if (SUCCEEDED(hr2)) {
			HookFactory(pD2D1Factory);
			pD2D1Factory->Release();
		}
	}
	return hr;
}

HRESULT WINAPI IMPL_CreateWicBitmapRenderTarget(
	ID2D1Factory* This,
	IWICBitmap* target,
	const D2D1_RENDER_TARGET_PROPERTIES* renderTargetProperties,
	ID2D1RenderTarget** renderTarget
	) {
	HRESULT hr = ORIG_CreateWicBitmapRenderTarget(
		This,
		target,
		renderTargetProperties,
		renderTarget
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*renderTarget, D2D1_RENDER_TARGET_CATEGORY);
	}
	MyDebug(L"IMPL_CreateWicBitmapRenderTarget hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateHwndRenderTarget(
	ID2D1Factory* This,
	const D2D1_RENDER_TARGET_PROPERTIES* renderTargetProperties,
	const D2D1_HWND_RENDER_TARGET_PROPERTIES* hwndRenderTargetProperties,
	ID2D1HwndRenderTarget** hwndRenderTarget
	) {
	HRESULT hr = ORIG_CreateHwndRenderTarget(
		This,
		renderTargetProperties,
		hwndRenderTargetProperties,
		hwndRenderTarget
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*hwndRenderTarget, D2D1_RENDER_TARGET1_CATEGORY);
	}
	MyDebug(L"IMPL_CreateHwndRenderTarget hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDxgiSurfaceRenderTarget(
	ID2D1Factory* This,
	IDXGISurface* dxgiSurface,
	const D2D1_RENDER_TARGET_PROPERTIES* renderTargetProperties,
	ID2D1RenderTarget** renderTarget
	) {
	HRESULT hr = ORIG_CreateDxgiSurfaceRenderTarget(
		This,
		dxgiSurface,
		renderTargetProperties,
		renderTarget
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*renderTarget, D2D1_RENDER_TARGET_CATEGORY);
	}
	MyDebug(L"IMPL_CreateDxgiSurfaceRenderTarget hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDCRenderTarget(
	ID2D1Factory* This,
	const D2D1_RENDER_TARGET_PROPERTIES* renderTargetProperties,
	ID2D1DCRenderTarget** dcRenderTarget
	) {
	HRESULT hr = ORIG_CreateDCRenderTarget(
		This,
		renderTargetProperties,
		dcRenderTarget
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*dcRenderTarget, D2D1_RENDER_TARGET1_CATEGORY);
	}
	MyDebug(L"IMPL_CreateDCRenderTarget hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateCompatibleRenderTarget(
	ID2D1RenderTarget* This,
	CONST D2D1_SIZE_F* desiredSize,
	CONST D2D1_SIZE_U* desiredPixelSize,
	CONST D2D1_PIXEL_FORMAT* desiredFormat,
	D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS options,
	ID2D1BitmapRenderTarget** bitmapRenderTarget
	) {
	HRESULT hr = ORIG_CreateCompatibleRenderTarget(
		This,
		desiredSize,
		desiredPixelSize,
		desiredFormat,
		options,
		bitmapRenderTarget
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*bitmapRenderTarget, D2D1_RENDER_TARGET1_CATEGORY);
	}
	MyDebug(L"IMPL_CreateCompatibleRenderTarget hooked");
	return hr;
}

void WINAPI IMPL_D2D1RenderTarget_SetTextAntialiasMode(
	ID2D1RenderTarget* This,
	D2D1_TEXT_ANTIALIAS_MODE textAntialiasMode
	) {
	MyDebug(L"IMPL_D2D1RenderTarget_SetTextAntialiasMode hooked");
	ORIG_D2D1RenderTarget_SetTextAntialiasMode(This, GetD2DParams()->AntialiasMode);
}

void WINAPI IMPL_D2D1DeviceContext_SetTextAntialiasMode(
	ID2D1DeviceContext* This,
	D2D1_TEXT_ANTIALIAS_MODE textAntialiasMode
	) {
	MyDebug(L"IMPL_D2D1DeviceContext_SetTextAntialiasMode hooked");
	ORIG_D2D1DeviceContext_SetTextAntialiasMode(This, GetD2DParams()->AntialiasMode);
}

void WINAPI IMPL_D2D1RenderTarget_SetTextRenderingParams(
	ID2D1RenderTarget* This,
	_In_opt_ IDWriteRenderingParams* textRenderingParams
	) {
	MyDebug(L"IMPL_D2D1RenderTarget_SetTextRenderingParams hooked");
	ORIG_D2D1RenderTarget_SetTextRenderingParams(This, GetD2DRenderingParams(textRenderingParams));
}

void WINAPI IMPL_D2D1DeviceContext_SetTextRenderingParams(
	ID2D1DeviceContext* This,
	_In_opt_ IDWriteRenderingParams* textRenderingParams
	) {
	MyDebug(L"IMPL_D2D1DeviceContext_SetTextRenderingParams hooked");
	ORIG_D2D1DeviceContext_SetTextRenderingParams(This, GetD2DRenderingParams(textRenderingParams));
}

HRESULT WINAPI IMPL_CreateDeviceContext(
	ID2D1Device* This,
	D2D1_DEVICE_CONTEXT_OPTIONS options,
	ID2D1DeviceContext** deviceContext
	) {
	HRESULT hr = ORIG_CreateDeviceContext(
		This,
		options,
		deviceContext
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*deviceContext, D2D1_DEVICE_CONTEXT_CATEGORY);
	}
	MyDebug(L"IMPL_CreateDeviceContext hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDeviceContext2(
	ID2D1Device1* This,
	D2D1_DEVICE_CONTEXT_OPTIONS options,
	ID2D1DeviceContext1** deviceContext1
	) {
	HRESULT hr = ORIG_CreateDeviceContext2(
		This,
		options,
		deviceContext1
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*deviceContext1, D2D1_DEVICE_CONTEXT_CATEGORY);
	}
	MyDebug(L"IMPL_CreateDeviceContext2 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDeviceContext3(
	ID2D1Device2* This,
	D2D1_DEVICE_CONTEXT_OPTIONS options,
	ID2D1DeviceContext2** deviceContext2
	) {
	HRESULT hr = ORIG_CreateDeviceContext3(
		This,
		options,
		deviceContext2
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*deviceContext2, D2D1_DEVICE_CONTEXT_CATEGORY);
	}
	MyDebug(L"IMPL_CreateDeviceContext3 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDeviceContext4(
	ID2D1Device3* This,
	D2D1_DEVICE_CONTEXT_OPTIONS options,
	ID2D1DeviceContext3** deviceContext2
	) {
	HRESULT hr = ORIG_CreateDeviceContext4(
		This,
		options,
		deviceContext2
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*deviceContext2, D2D1_DEVICE_CONTEXT_CATEGORY);
	}
	MyDebug(L"IMPL_CreateDeviceContext4 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDeviceContext5(
	ID2D1Device4* This,
	D2D1_DEVICE_CONTEXT_OPTIONS options,
	ID2D1DeviceContext4** deviceContext
	) {
	HRESULT hr = ORIG_CreateDeviceContext5(
		This,
		options,
		deviceContext
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*deviceContext, D2D1_DEVICE_CONTEXT_CATEGORY);
	}
	MyDebug(L"IMPL_CreateDeviceContext5 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDeviceContext6(
	ID2D1Device5* This,
	D2D1_DEVICE_CONTEXT_OPTIONS options,
	ID2D1DeviceContext5** deviceContext
	) {
	HRESULT hr = ORIG_CreateDeviceContext6(
		This,
		options,
		deviceContext
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*deviceContext, D2D1_DEVICE_CONTEXT_CATEGORY);
	}
	MyDebug(L"IMPL_CreateDeviceContext6 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDeviceContext7(
	ID2D1Device6* This,
	D2D1_DEVICE_CONTEXT_OPTIONS options,
	ID2D1DeviceContext6** deviceContext
	) {
	HRESULT hr = ORIG_CreateDeviceContext7(
		This,
		options,
		deviceContext
		);
	if (SUCCEEDED(hr)) {
		HookRenderTarget(*deviceContext, D2D1_DEVICE_CONTEXT_CATEGORY);
	}
	MyDebug(L"IMPL_CreateDeviceContext7 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDevice1(
	ID2D1Factory1* This,
	IDXGIDevice* dxgiDevice,
	ID2D1Device** d2dDevice
	) {
	HRESULT hr = ORIG_CreateDevice1(
		This,
		dxgiDevice,
		d2dDevice
		);
	if (SUCCEEDED(hr)) {
		HookDevice(*d2dDevice);
	}
	MyDebug(L"IMPL_CreateDevice1 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDevice2(
	ID2D1Factory2* This,
	IDXGIDevice* dxgiDevice,
	ID2D1Device1** d2dDevice1
	){
	HRESULT hr = ORIG_CreateDevice2(
		This,
		dxgiDevice,
		d2dDevice1
		);
	if (SUCCEEDED(hr)) {
		HookDevice(*d2dDevice1);
	}
	MyDebug(L"IMPL_CreateDevice2 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDevice3(
	ID2D1Factory3* This,
	IDXGIDevice* dxgiDevice,
	ID2D1Device2** d2dDevice2
	){
	HRESULT hr = ORIG_CreateDevice3(
		This,
		dxgiDevice,
		d2dDevice2
		);
	if (SUCCEEDED(hr)) {
		HookDevice(*d2dDevice2);
	}
	MyDebug(L"IMPL_CreateDevice3 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDevice4(
	ID2D1Factory4* This,
	IDXGIDevice* dxgiDevice,
	ID2D1Device3** d2dDevice3
	){
	HRESULT hr = ORIG_CreateDevice4(
		This,
		dxgiDevice,
		d2dDevice3
		);
	if (SUCCEEDED(hr)) {
		HookDevice(*d2dDevice3);
	}
	MyDebug(L"IMPL_CreateDevice4 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDevice5(
	ID2D1Factory5* This,
	IDXGIDevice* dxgiDevice,
	ID2D1Device4** d2dDevice4
	){
	HRESULT hr = ORIG_CreateDevice5(
		This,
		dxgiDevice,
		d2dDevice4
		);
	if (SUCCEEDED(hr)) {
		HookDevice(*d2dDevice4);
	}
	MyDebug(L"IMPL_CreateDevice5 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDevice6(
	ID2D1Factory6* This,
	IDXGIDevice* dxgiDevice,
	ID2D1Device5** d2dDevice5
	){
	HRESULT hr = ORIG_CreateDevice6(
		This,
		dxgiDevice,
		d2dDevice5
		);
	if (SUCCEEDED(hr)) {
		HookDevice(*d2dDevice5);
	}
	MyDebug(L"IMPL_CreateDevice6 hooked");
	return hr;
}

HRESULT WINAPI IMPL_CreateDevice7(
	ID2D1Factory7* This,
	IDXGIDevice* dxgiDevice,
	ID2D1Device6** d2dDevice6
	){
	HRESULT hr = ORIG_CreateDevice7(
		This,
		dxgiDevice,
		d2dDevice6
		);
	if (SUCCEEDED(hr)) {
		HookDevice(*d2dDevice6);
	}
	MyDebug(L"IMPL_CreateDevice7 hooked");
	return hr;
}


/*
bool CreateFontFace(IDWriteGdiInterop* gdi, IDWriteFont*** dfont, LOGFONT* lf)
{
__try
{
gdi->CreateFontFromLOGFONT(lf, *dfont);
return true;
}
__except(EXCEPTION_EXECUTE_HANDLER)
{
return false;
}
}*/

/*
void WINAPI IMPL_SetTextRenderingParams(ID2D1RenderTarget* self, __in_opt IDWriteRenderingParams *textRenderingParams = nullptr)
{
return ORIG_SetTextRenderingParams(self, g_D2DParamsLarge.RenderingParams);
}

void WINAPI IMPL_SetTextAntialiasMode(ID2D1RenderTarget* self,  D2D1_TEXT_ANTIALIAS_MODE textAntialiasMode)
{
return ORIG_SetTextAntialiasMode(self, g_D2DParamsLarge.AntialiasMode);
}*/

bool hookD2D1() {
	//MessageBox(nullptr, L"HookD2D1", nullptr, MB_OK);
	static bool loaded = [&] {
		return true;
	}();
	return loaded;
}

#define FAILEXIT { /*CoUninitialize();*/ return false;}
using FactoryGetSystemFontCollectionMethod = HRESULT (WINAPI *)(
	IDWriteFactory*, IDWriteFontCollection**, BOOL);
using Factory3GetSystemFontSetMethod = HRESULT (WINAPI *)(
	IDWriteFactory3*, IDWriteFontSet**);
using Factory3GetSystemFontCollectionMethod = HRESULT (WINAPI *)(
	IDWriteFactory3*, BOOL, IDWriteFontCollection1**, BOOL);

HRESULT WINAPI IMPL_Factory_GetSystemFontCollection(
	IDWriteFactory*, IDWriteFontCollection**, BOOL);
HRESULT WINAPI IMPL_Factory3_GetSystemFontSet(
	IDWriteFactory3*, IDWriteFontSet**);
HRESULT WINAPI IMPL_Factory3_GetSystemFontCollection(
	IDWriteFactory3*, BOOL, IDWriteFontCollection1**, BOOL);

namespace {

constexpr size_t kFactoryGetSystemFontCollectionSlot = 3;
constexpr size_t kFactory3GetSystemFontSetSlot = 35;
constexpr size_t kFactory3GetSystemFontCollectionSlot = 38;

struct FactoryAliasVtableHooks
{
	void** vtable = nullptr;
	FactoryGetSystemFontCollectionMethod getSystemFontCollection = nullptr;
	Factory3GetSystemFontSetMethod getSystemFontSet = nullptr;
	Factory3GetSystemFontCollectionMethod getModernSystemFontCollection = nullptr;
	bool getSystemFontCollectionPatched = false;
	bool getSystemFontSetPatched = false;
	bool getModernSystemFontCollectionPatched = false;
};

struct FactoryAliasSource
{
	CComPtr<IUnknown> factoryIdentity;
	CComPtr<IDWriteFontSet> systemSet;
};

std::mutex& GetFactoryAliasVtableMutex()
{
	static std::mutex* mutex = new std::mutex;
	return *mutex;
}

std::vector<FactoryAliasVtableHooks>& GetFactoryAliasVtableRegistry()
{
	static std::vector<FactoryAliasVtableHooks>* registry =
		new std::vector<FactoryAliasVtableHooks>;
	return *registry;
}

std::vector<FactoryAliasSource>& GetFactoryAliasSourceRegistry()
{
	static std::vector<FactoryAliasSource>* registry =
		new std::vector<FactoryAliasSource>;
	return *registry;
}

CComPtr<IUnknown> GetFactoryIdentity(IDWriteFactory* factory)
{
	CComPtr<IUnknown> identity;
	if (factory != nullptr)
		factory->QueryInterface(&identity);
	return identity;
}

void PublishFactoryAliasSource(
	IDWriteFactory* factory,
	IDWriteFontSet* systemSet,
	bool replaceExisting = false)
{
	CComPtr<IUnknown> const identity = GetFactoryIdentity(factory);
	if (identity == nullptr || systemSet == nullptr)
		return;
	std::lock_guard<std::mutex> lock(GetFactoryAliasVtableMutex());
	for (FactoryAliasSource& source : GetFactoryAliasSourceRegistry())
	{
		if (source.factoryIdentity == identity)
		{
			if (replaceExisting)
				source.systemSet = systemSet;
			return;
		}
	}
	GetFactoryAliasSourceRegistry().push_back({identity, systemSet});
}

CComPtr<IDWriteFontSet> GetFactoryAliasSource(IDWriteFactory* factory)
{
	CComPtr<IUnknown> const identity = GetFactoryIdentity(factory);
	if (identity == nullptr)
		return nullptr;
	std::lock_guard<std::mutex> lock(GetFactoryAliasVtableMutex());
	for (FactoryAliasSource const& source : GetFactoryAliasSourceRegistry())
	{
		if (source.factoryIdentity == identity)
			return source.systemSet;
	}
	return nullptr;
}

FactoryAliasVtableHooks& GetOrAddFactoryAliasVtable(void** vtable)
{
	auto& registry = GetFactoryAliasVtableRegistry();
	for (FactoryAliasVtableHooks& hooks : registry)
	{
		if (hooks.vtable == vtable)
			return hooks;
	}
	registry.emplace_back();
	registry.back().vtable = vtable;
	return registry.back();
}

template <typename Method>
void* MethodAddress(Method method) noexcept
{
	void* address = nullptr;
	SetPointerValue(address, method);
	return address;
}

template <typename Method>
bool PatchFactoryVtableMethod(
	FactoryAliasVtableHooks& hooks,
	size_t slot,
	Method replacement,
	Method& original,
	bool& patched)
{
	if (hooks.vtable == nullptr)
		return false;
	void** const entry = hooks.vtable + slot;
	void* const replacementAddress = MethodAddress(replacement);
	if (patched)
		return *entry == replacementAddress;

	void* const originalAddress = *entry;
	if (originalAddress == nullptr || originalAddress == replacementAddress)
		return false;
	SetPointerValue(original, originalAddress);

	auto protection = renderer_raii::PageProtection::TrySet(
		entry, sizeof(*entry), PAGE_READWRITE);
	if (!protection)
	{
		original = nullptr;
		return false;
	}
	InterlockedExchangePointer(
		reinterpret_cast<PVOID*>(entry), replacementAddress);
	if (!protection.restore())
	{
		InterlockedExchangePointer(
			reinterpret_cast<PVOID*>(entry), originalAddress);
		original = nullptr;
		return false;
	}
	patched = true;
	return true;
}

template <typename Method>
void RestoreFactoryVtableMethod(
	FactoryAliasVtableHooks& hooks,
	size_t slot,
	Method replacement,
	Method original,
	bool& patched) noexcept
{
	if (!patched || hooks.vtable == nullptr || original == nullptr)
		return;
	void** const entry = hooks.vtable + slot;
	void* const replacementAddress = MethodAddress(replacement);
	void* const originalAddress = MethodAddress(original);
	if (*entry != replacementAddress)
	{
		patched = false;
		return;
	}

	auto protection = renderer_raii::PageProtection::TrySet(
		entry, sizeof(*entry), PAGE_READWRITE);
	if (!protection)
		return;
	InterlockedExchangePointer(
		reinterpret_cast<PVOID*>(entry), originalAddress);
	if (protection.restore())
		patched = false;
}

bool PatchFactoryAliasVtables(
	IDWriteFactory* factory,
	IDWriteFactory3* factory3)
{
	if (factory == nullptr || factory3 == nullptr)
		return false;
	void** const factoryVtable = *reinterpret_cast<void***>(factory);
	void** const factory3Vtable = *reinterpret_cast<void***>(factory3);
	if (factoryVtable == nullptr || factory3Vtable == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(GetFactoryAliasVtableMutex());
	GetOrAddFactoryAliasVtable(factoryVtable);
	GetOrAddFactoryAliasVtable(factory3Vtable);
	FactoryAliasVtableHooks& factoryHooks =
		GetOrAddFactoryAliasVtable(factoryVtable);
	FactoryAliasVtableHooks& factory3Hooks =
		GetOrAddFactoryAliasVtable(factory3Vtable);
	bool const baseWasPatched = factoryHooks.getSystemFontCollectionPatched;
	bool const setWasPatched = factory3Hooks.getSystemFontSetPatched;
	bool const modernWasPatched =
		factory3Hooks.getModernSystemFontCollectionPatched;
	bool const patched = PatchFactoryVtableMethod(
			factoryHooks,
			kFactoryGetSystemFontCollectionSlot,
			&IMPL_Factory_GetSystemFontCollection,
			factoryHooks.getSystemFontCollection,
			factoryHooks.getSystemFontCollectionPatched) &&
		PatchFactoryVtableMethod(
			factory3Hooks,
			kFactory3GetSystemFontSetSlot,
			&IMPL_Factory3_GetSystemFontSet,
			factory3Hooks.getSystemFontSet,
			factory3Hooks.getSystemFontSetPatched) &&
		PatchFactoryVtableMethod(
			factory3Hooks,
			kFactory3GetSystemFontCollectionSlot,
			&IMPL_Factory3_GetSystemFontCollection,
			factory3Hooks.getModernSystemFontCollection,
			factory3Hooks.getModernSystemFontCollectionPatched);
	if (patched)
		return true;

	if (!modernWasPatched)
		RestoreFactoryVtableMethod(
			factory3Hooks,
			kFactory3GetSystemFontCollectionSlot,
			&IMPL_Factory3_GetSystemFontCollection,
			factory3Hooks.getModernSystemFontCollection,
			factory3Hooks.getModernSystemFontCollectionPatched);
	if (!setWasPatched)
		RestoreFactoryVtableMethod(
			factory3Hooks,
			kFactory3GetSystemFontSetSlot,
			&IMPL_Factory3_GetSystemFontSet,
			factory3Hooks.getSystemFontSet,
			factory3Hooks.getSystemFontSetPatched);
	if (!baseWasPatched)
		RestoreFactoryVtableMethod(
			factoryHooks,
			kFactoryGetSystemFontCollectionSlot,
			&IMPL_Factory_GetSystemFontCollection,
			factoryHooks.getSystemFontCollection,
			factoryHooks.getSystemFontCollectionPatched);
	return false;
}

FactoryGetSystemFontCollectionMethod GetOriginalSystemFontCollection(
	IDWriteFactory* factory)
{
	void** const vtable = factory == nullptr ? nullptr :
		*reinterpret_cast<void***>(factory);
	std::lock_guard<std::mutex> lock(GetFactoryAliasVtableMutex());
	for (FactoryAliasVtableHooks const& hooks : GetFactoryAliasVtableRegistry())
	{
		if (hooks.vtable == vtable)
			return hooks.getSystemFontCollection;
	}
	return nullptr;
}

Factory3GetSystemFontSetMethod GetOriginalSystemFontSet(IDWriteFactory3* factory)
{
	void** const vtable = factory == nullptr ? nullptr :
		*reinterpret_cast<void***>(factory);
	std::lock_guard<std::mutex> lock(GetFactoryAliasVtableMutex());
	for (FactoryAliasVtableHooks const& hooks : GetFactoryAliasVtableRegistry())
	{
		if (hooks.vtable == vtable)
			return hooks.getSystemFontSet;
	}
	return nullptr;
}

Factory3GetSystemFontCollectionMethod GetOriginalModernSystemFontCollection(
	IDWriteFactory3* factory)
{
	void** const vtable = factory == nullptr ? nullptr :
		*reinterpret_cast<void***>(factory);
	std::lock_guard<std::mutex> lock(GetFactoryAliasVtableMutex());
	for (FactoryAliasVtableHooks const& hooks : GetFactoryAliasVtableRegistry())
	{
		if (hooks.vtable == vtable)
			return hooks.getModernSystemFontCollection;
	}
	return nullptr;
}

bool CaptureFactoryAliasSource(IDWriteFactory* factory)
{
	if (factory == nullptr)
		return false;
	CComPtr<IDWriteFontCollection> systemCollection;
	FactoryGetSystemFontCollectionMethod const original =
		GetOriginalSystemFontCollection(factory);
	HRESULT const result = original == nullptr ?
		factory->GetSystemFontCollection(&systemCollection, FALSE) :
		original(factory, &systemCollection, FALSE);
	if (FAILED(result) || systemCollection == nullptr)
		return false;

	CComPtr<IDWriteFontCollection1> systemCollection1;
	CComPtr<IDWriteFontSet> systemSet;
	if (FAILED(systemCollection->QueryInterface(&systemCollection1)) ||
		systemCollection1 == nullptr ||
		FAILED(systemCollection1->GetFontSet(&systemSet)) || systemSet == nullptr)
		return false;
	PublishFactoryAliasSource(factory, systemSet);

	directwrite_alias::AliasFontSet aliases;
	directwrite_alias::BuildStatus const status =
		directwrite_alias::GetOrCreate(factory, systemSet, aliases);
	SignalDirectWriteDiagnostic(directwrite_alias::StatusName(status));
	return true;
}

void RestoreFactoryAliasVtables() noexcept
{
	std::lock_guard<std::mutex> lock(GetFactoryAliasVtableMutex());
	for (FactoryAliasVtableHooks& hooks : GetFactoryAliasVtableRegistry())
	{
		RestoreFactoryVtableMethod(
			hooks,
			kFactory3GetSystemFontCollectionSlot,
			&IMPL_Factory3_GetSystemFontCollection,
			hooks.getModernSystemFontCollection,
			hooks.getModernSystemFontCollectionPatched);
		RestoreFactoryVtableMethod(
			hooks,
			kFactory3GetSystemFontSetSlot,
			&IMPL_Factory3_GetSystemFontSet,
			hooks.getSystemFontSet,
			hooks.getSystemFontSetPatched);
		RestoreFactoryVtableMethod(
			hooks,
			kFactoryGetSystemFontCollectionSlot,
			&IMPL_Factory_GetSystemFontCollection,
			hooks.getSystemFontCollection,
			hooks.getSystemFontCollectionPatched);
	}
}

void ClearFactoryAliasSources() noexcept
{
	std::vector<FactoryAliasSource> sources;
	{
		std::lock_guard<std::mutex> lock(GetFactoryAliasVtableMutex());
		sources.swap(GetFactoryAliasSourceRegistry());
	}
	sources.clear();
}

} // namespace

static bool HookDirectWriteAliasCollection(CComPtr<IDWriteFactory>& factory)
{
	HOOK(factory, CreateTextFormat, 15);

	CComPtr<IDWriteFactory3> factory3;
	if (FAILED(factory->QueryInterface(&factory3)) || factory3 == nullptr)
		return false;
	return ISHOOKED(CreateTextFormat) && CaptureFactoryAliasSource(factory) &&
		PatchFactoryAliasVtables(factory, factory3);
}

bool hookDirectWrite(IUnknown** factory)
{
	if (factory == nullptr || *factory == nullptr)
		return false;

	CComPtr<IDWriteFactory> writeFactory;
	if (FAILED((*factory)->QueryInterface(&writeFactory)) ||
		writeFactory == nullptr)
		return false;

	HOOK(writeFactory, CreateGlyphRunAnalysis, 23);
	HOOK(writeFactory, GetGdiInterop, 17);

	CGdippSettings const* settings = CGdippSettings::GetInstance();
	bool aliasCollectionReady = true;
	if (!settings->IsWindows81() && !settings->IsWindows8() &&
		settings->DelayedInited() &&
		settings->GetFontSubstitutesInfo().GetSize() != 0)
	{
		aliasCollectionReady = HookDirectWriteAliasCollection(writeFactory);
	}
	MyDebug(L"DW1 hooked");

	CComPtr<IDWriteFactory2> factory2;
	if (SUCCEEDED((*factory)->QueryInterface(&factory2)) && factory2)
	{
		HOOK(factory2, CreateGlyphRunAnalysis2, 30);
		MyDebug(L"DW2 hooked");
	}

	CComPtr<IDWriteFactory3> factory3;
	if (SUCCEEDED((*factory)->QueryInterface(&factory3)) && factory3)
	{
		HOOK(factory3, CreateGlyphRunAnalysis3, 31);
		MyDebug(L"DW3 hooked");
	}
	return aliasCollectionReady;
}

#undef FAILEXIT
#define FAILEXIT {return;}
void TriggerHook(ID2D1Factory* d2d_factory) {
	const D2D1_PIXEL_FORMAT format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
	const D2D1_RENDER_TARGET_PROPERTIES properties =
		D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, format, 0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_NONE);
	CComPtr<ID2D1DCRenderTarget>target;
	if (FAILED(d2d_factory->CreateDCRenderTarget(&properties, &target))) FAILEXIT;
}

// The entry point of DirectWrite hooking

// D2D1CreateFactory
// 	HookFactory
// 		ID2D1Factory
// 			CreateWicBitmapRenderTarget
// 			CreateHwndRenderTarget
// 			CreateDxgiSurfaceRenderTarget
// 			CreateDCRenderTarget
// 				HookRenderTarget ->
// 		ID2D1Factory<N>
// 			CreateDevice<N>
// 				HookDevice ->

// D2D1CreateDevice
// 	HookDevice
// 		ID2D1Device<N>
// 			CreateDeviceContext<N>
// 				HookRenderTarget ->

// D2D1CreateDeviceContext
// 	HookRenderTarget
// 		ID2D1RenderTarget
// 			CreateCompatibleRenderTarget
// 				HookRenderTarget ->
// 			DrawText
// 			DrawTextLayout
// 			DrawGlyphRun
// 			SetTextAntialiasMode
// 			SetTextRenderingParams
// 			GetFactory()
// 				HookFactory ->

// 		ID2D1DeviceContext
// 			DrawGlyphRun1
// 			GetDevice()
// 				HookDevice ->

// DWriteCreateFactory
// 	hookDirectWrite
// 		IDWriteFactory
// 			GetGdiInterop
// 				CreateBitmapRenderTarget
// 					IDWriteBitmapRenderTarget
// 						DrawGlyphRun

// 			CreateGlyphRunAnalysis
// 				IDWriteGlyphRunAnalysis
// 					GetAlphaBlendParams

// 			Alias collection boundary
// 				GetSystemFontCollection
// 				IDWriteFactory3::GetSystemFontSet
// 				IDWriteFactory3::GetSystemFontCollection
// 				CreateTextFormat with the native system collection

// 		IDWriteFactory<N>
// 			CreateGlyphRunAnalysis<N>
// 				IDWriteGlyphRunAnalysis
// 					GetAlphaBlendParams
static DWORD WINAPI HookExistingDirectWriteFactory(LPVOID moduleReference)
{
	renderer_raii::UniqueModuleReference selfReference(
		static_cast<HMODULE>(moduleReference));
	bool sharedFactoryHooked = false;
	CComPtr<IUnknown> factory;
	if (ORIG_DWriteCreateFactory != nullptr &&
		SUCCEEDED(ORIG_DWriteCreateFactory(
			DWRITE_FACTORY_TYPE_SHARED,
			__uuidof(IDWriteFactory),
			&factory)))
	{
		IUnknown* rawFactory = factory;
		sharedFactoryHooked = hookDirectWrite(&rawFactory);
	}

	if (sharedFactoryHooked)
	{
		// The launch gate may release the image-entry thread only after the
		// shared factory's collection boundary is installed.
		SignalDirectWriteDiagnostic(L"hook-ready");
	}
	HMODULE rawSelfReference = selfReference.release();
	FreeLibraryAndExitThread(rawSelfReference, 0);
	return 0;
}

static void ScheduleExistingDirectWriteFactoryHook()
{
	HMODULE moduleReference = nullptr;
	if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
		reinterpret_cast<LPCTSTR>(&HookExistingDirectWriteFactory),
		&moduleReference))
		return;
	renderer_raii::UniqueModuleReference selfReference(moduleReference);
	auto thread = renderer_raii::AdoptHandle(CreateThread(
		nullptr, 0, HookExistingDirectWriteFactory, selfReference.get(), 0, nullptr));
	if (thread) {
		selfReference.release();
	}
}

struct RendererModulePins
{
	renderer_raii::UniqueModuleReference d2d1;
	renderer_raii::UniqueModuleReference dwrite;
	std::vector<renderer_raii::UniqueHandle> diagnosticEvents;
};

static SRWLOCK g_directWriteDiagnosticLock = SRWLOCK_INIT;

class DirectWriteDiagnosticLock
{
public:
	DirectWriteDiagnosticLock() noexcept
	{
		AcquireSRWLockExclusive(&g_directWriteDiagnosticLock);
	}

	~DirectWriteDiagnosticLock() noexcept
	{
		ReleaseSRWLockExclusive(&g_directWriteDiagnosticLock);
	}

	DirectWriteDiagnosticLock(const DirectWriteDiagnosticLock&) = delete;
	DirectWriteDiagnosticLock& operator=(const DirectWriteDiagnosticLock&) = delete;
};

static RendererModulePins& GetRendererModulePins()
{
	// The process owns this state. Explicit DLL unload releases its contained
	// module references before FreeLibraryAndExitThread; process termination
	// leaves the tiny state allocation to the OS to avoid FreeLibrary under the
	// loader lock.
	static RendererModulePins* pins = new RendererModulePins;
	return *pins;
}

static WCHAR const* GetDirectWriteDiagnosticRole() noexcept
{
	WCHAR const* const commandLine = GetCommandLineW();
	if (commandLine == nullptr || wcsstr(commandLine, L"--type=") == nullptr)
	{
		if (commandLine != nullptr &&
			wcsstr(commandLine, L"-contentproc") != nullptr)
			return L"renderer";
		return L"main";
	}
	if (wcsstr(commandLine, L"--type=renderer") != nullptr)
		return L"renderer";
	if (wcsstr(commandLine, L"--type=utility") != nullptr)
		return L"utility";
	if (wcsstr(commandLine, L"--type=gpu-process") != nullptr)
		return L"gpu";
	return L"other";
}

static void RetainDirectWriteDiagnosticEvent(WCHAR const* eventName)
{
	renderer_raii::UniqueHandle event = renderer_raii::AdoptHandle(
		CreateEventW(nullptr, TRUE, TRUE, eventName));
	DWORD const createError = GetLastError();
	if (!event || createError == ERROR_ALREADY_EXISTS)
		return;

	DirectWriteDiagnosticLock lock;
	GetRendererModulePins().diagnosticEvents.emplace_back(std::move(event));
}

static void SignalDirectWriteDiagnostic(WCHAR const* stage)
{
	DWORD const namespaceLength = GetEnvironmentVariableW(
		L"MACTYPE_DIRECTWRITE_DIAGNOSTICS", nullptr, 0);
	if (namespaceLength <= 1 || namespaceLength > 80 || stage == nullptr)
		return;

	std::vector<WCHAR> diagnosticNamespace(namespaceLength);
	if (GetEnvironmentVariableW(
			L"MACTYPE_DIRECTWRITE_DIAGNOSTICS",
			diagnosticNamespace.data(), namespaceLength) != namespaceLength - 1)
		return;
	for (WCHAR& value : diagnosticNamespace)
	{
		if (value != L'\0' &&
			!((value >= L'a' && value <= L'z') ||
			  (value >= L'A' && value <= L'Z') ||
			  (value >= L'0' && value <= L'9') || value == L'-'))
			value = L'_';
	}

	WCHAR eventName[256] = {};
	if (FAILED(StringCchPrintfW(
			eventName, ARRAYSIZE(eventName), L"Local\\MacType.%s.%s.%s",
			diagnosticNamespace.data(), GetDirectWriteDiagnosticRole(), stage)))
		return;
	RetainDirectWriteDiagnosticEvent(eventName);

	if (FAILED(StringCchPrintfW(
			eventName, ARRAYSIZE(eventName), L"Local\\MacType.%s.pid-%lu.%s",
			diagnosticNamespace.data(), GetCurrentProcessId(), stage)))
		return;
	RetainDirectWriteDiagnosticEvent(eventName);
}

static void SignalDirectWriteFamilyDiagnostic(
	WCHAR const* stagePrefix, WCHAR const* familyName)
{
	if (stagePrefix == nullptr || familyName == nullptr)
		return;
	WCHAR const* familyTag = nullptr;
	if (_wcsicmp(familyName, L"Cambria") == 0)
		familyTag = L"cambria";
	else if (_wcsicmp(familyName, L"Impact") == 0)
		familyTag = L"impact";
	else if (_wcsicmp(familyName, L"Courier New") == 0)
		familyTag = L"courier-new";
	if (familyTag == nullptr)
		return;

	WCHAR stage[80] = {};
	if (SUCCEEDED(StringCchPrintfW(
			stage, ARRAYSIZE(stage), L"%s-%s", stagePrefix, familyTag)))
		SignalDirectWriteDiagnostic(stage);
}

void ReleasePinnedRendererModules()
{
	directwrite_alias::ClearCache();
	RendererModulePins& pins = GetRendererModulePins();
	{
		DirectWriteDiagnosticLock lock;
		pins.diagnosticEvents.clear();
	}
	pins.dwrite.reset();
	pins.d2d1.reset();
}

void HookD2DDll()
{
	SignalDirectWriteDiagnostic(L"hook-entered");
	typedef HRESULT (WINAPI *PFN_DWriteCreateFactory)(
		_In_ DWRITE_FACTORY_TYPE factoryType,
		_In_ REFIID iid,
		_COM_Outptr_ IUnknown **factory
		);

	typedef HRESULT (WINAPI *PFN_D2D1CreateFactory)(
		D2D1_FACTORY_TYPE factoryType,
		REFIID riid,
		const D2D1_FACTORY_OPTIONS* pFactoryOptions,
		void** ppIFactory
		);
	//Sleep(30 * 1000);
#ifdef DEBUG
	//MessageBox(0, L"HookD2DDll", nullptr, MB_OK);
#endif
	renderer_raii::BorrowedModule d2d1(GetModuleHandle(_T("d2d1.dll")));
	renderer_raii::BorrowedModule dw(GetModuleHandle(_T("dwrite.dll")));
	RendererModulePins& pins = GetRendererModulePins();

	if (!d2d1)
	{
		pins.d2d1.reset(LoadLibrary(_T("d2d1.dll")));
		d2d1 = renderer_raii::BorrowedModule(pins.d2d1.get());
	}
	if (!dw)
	{
		pins.dwrite.reset(LoadLibrary(_T("dwrite.dll")));
		dw = renderer_raii::BorrowedModule(pins.dwrite.get());
	}
	if (!d2d1 || !dw) {
		return;
	}
	void* D2D1Factory = GetProcAddress(d2d1.get(), "D2D1CreateFactory");
	void* D2D1Device = GetProcAddress(d2d1.get(), "D2D1CreateDevice");
	void* D2D1Context = GetProcAddress(d2d1.get(), "D2D1CreateDeviceContext");
	void* DWFactory = GetProcAddress(dw.get(), "DWriteCreateFactory");
	SET_VAL(ORIG_D2D1CreateFactory, D2D1Factory);
	SET_VAL(ORIG_D2D1CreateDevice, D2D1Device);
	SET_VAL(ORIG_D2D1CreateDeviceContext, D2D1Context);
	SET_VAL(ORIG_DWriteCreateFactory, DWFactory);
	if (DWFactory) {
		hook_demand_DWriteCreateFactory();
	}
	if (D2D1Factory){
		hook_demand_D2D1CreateFactory();
	}
	if (D2D1Device) {
		hook_demand_D2D1CreateDevice();
	}
	if (D2D1Context) {
		hook_demand_D2D1CreateDeviceContext();
	}
	if (DWFactory) {
		// Service injection can happen after a browser has created its shared
		// factory. Hook that implementation once the loader lock is released.
		// The worker publishes hook-ready only after the factory boundary exists.
		ScheduleExistingDirectWriteFactoryHook();
	}
}

/*
void HookGdiplus()
{
InitGdiplusFuncs();
//SET_VAL(ORIG_D2D1CreateFactory, D2D1Factory);
SET_VAL(ORIG_GdipDrawString, pfnGdipDrawString);
hook_demand_GdipDrawString();
}

GpStatus WINAPI IMPL_GdipDrawString(
GpGraphics               *graphics,
GDIPCONST WCHAR          *string,
INT                       length,
GDIPCONST GpFont         *font,
GDIPCONST RectF          *layoutRect,
GDIPCONST GpStringFormat *stringFormat,
GDIPCONST GpBrush        *brush
)
{
#define GDIPEXEC ORIG_GdipDrawString(graphics, string, length, font, layoutRect, stringFormat, brush)
#define GDIPCHECK(x) if ((x)!=Ok) return GDIPEXEC
if (string)
{
HDC dc = nullptr;
LOGFONTW lf = {0};
GpBrushType bt;
ARGB FontColor=0 ,bkColor = 0;
//GDIPLUS to gdi32 data preparation
GDIPCHECK(pfnGdipGetLogFontW(const_cast<GpFont*>(font), graphics, &lf));
GDIPCHECK(pfnGdipGetBrushType(const_cast<GpBrush*>(brush), &bt));
if (bt!=BrushTypeSolidColor) return GDIPEXEC; //only solid brush is supported by GDI32
GDIPCHECK(pfnGdipGetSolidFillColor(reinterpret_cast<GpSolidFill*>(const_cast<GpBrush*>(brush)), &FontColor));
if (FontColor>>24!=0xFF) return GDIPEXEC;	//only transparent and Opaque is supported.
GDIPCHECK(pfnGdipGetDC(graphics, &dc));
HFONT ft = CreateFontIndirectW(&lf);
HFONT oldfont = SelectFont(dc, ft);

SetTextColor(dc, FontColor & 0x00FFFFFF);
SetBkMode(dc, TRANSPARENT);
RECT gdiRect = {ROUND(layoutRect->X), ROUND(layoutRect->Y), ROUND(layoutRect->X+layoutRect->Width), ROUND(layoutRect->Y+layoutRect->Height)};
DrawText(dc, string, length, &gdiRect, DT_WORDBREAK);
//ExtTextOutW(dc, gdiRect.left, gdiRect.top, 0, &gdiRect, string, wcslen(string), nullptr);

SelectObject(dc, oldfont);
DeleteObject(ft);
pfnGdipReleaseDC(graphics, dc);
return Ok;
}
else
return ORIG_GdipDrawString(graphics, string, length, font, layoutRect, stringFormat, brush);
#undef GDIPCHECK
#undef GDIPEXEC
}
*/
HRESULT WINAPI IMPL_DWriteCreateFactory(__in DWRITE_FACTORY_TYPE factoryType,
	__in REFIID iid,
	__out IUnknown **factory)
{
	HRESULT ret = ORIG_DWriteCreateFactory(factoryType, iid, factory);
	if (SUCCEEDED(ret))
		hookDirectWrite(factory);
	return ret;
}

static bool IsAliasCollectionApplied(
	directwrite_alias::BuildStatus status) noexcept
{
	return status == directwrite_alias::BuildStatus::applied ||
		status == directwrite_alias::BuildStatus::appliedWithMissingReplacement;
}

static void SignalAliasCollectionStatus(
	directwrite_alias::BuildStatus status)
{
	SignalDirectWriteDiagnostic(directwrite_alias::StatusName(status));
}

static directwrite_alias::BuildStatus GetCanonicalFactoryAliases(
	IDWriteFactory3* factory3,
	directwrite_alias::AliasFontSet& aliases)
{
	CComPtr<IDWriteFactory> factory;
	if (factory3 == nullptr ||
		FAILED(factory3->QueryInterface(&factory)) || factory == nullptr)
		return directwrite_alias::BuildStatus::unsupportedFactory;

	CComPtr<IDWriteFontSet> const systemSet = GetFactoryAliasSource(factory);
	if (systemSet == nullptr)
		return directwrite_alias::BuildStatus::systemSetUnavailable;
	return directwrite_alias::GetOrCreate(factory, systemSet, aliases);
}

HRESULT WINAPI IMPL_Factory_GetSystemFontCollection(
	IDWriteFactory* self,
	IDWriteFontCollection** fontCollection,
	BOOL checkForUpdates)
{
	HCounter counter;
	SignalDirectWriteDiagnostic(L"legacy-system-collection-called");
	if (fontCollection == nullptr)
		return E_POINTER;
	*fontCollection = nullptr;

	FactoryGetSystemFontCollectionMethod const original =
		GetOriginalSystemFontCollection(self);
	if (original == nullptr)
		return E_UNEXPECTED;
	CComPtr<IDWriteFontCollection> systemCollection;
	HRESULT const result = original(self, &systemCollection, checkForUpdates);
	if (FAILED(result) || systemCollection == nullptr)
		return result;

	CComPtr<IDWriteFontCollection1> systemCollection1;
	CComPtr<IDWriteFontSet> systemSet;
	if (FAILED(systemCollection->QueryInterface(&systemCollection1)) ||
		systemCollection1 == nullptr ||
		FAILED(systemCollection1->GetFontSet(&systemSet)) ||
		systemSet == nullptr)
	{
		SignalAliasCollectionStatus(
			directwrite_alias::BuildStatus::systemSetUnavailable);
		*fontCollection = systemCollection.Detach();
		return result;
	}
	CComPtr<IDWriteFontSet> canonicalSystemSet = GetFactoryAliasSource(self);
	if (canonicalSystemSet == nullptr || checkForUpdates)
	{
		PublishFactoryAliasSource(self, systemSet, checkForUpdates != FALSE);
		canonicalSystemSet = GetFactoryAliasSource(self);
	}

	directwrite_alias::AliasFontSet aliases;
	directwrite_alias::BuildStatus const status =
		directwrite_alias::GetOrCreate(self, canonicalSystemSet, aliases);
	SignalAliasCollectionStatus(status);
	if (IsAliasCollectionApplied(status))
	{
		CComPtr<IDWriteFontCollection> aliasCollection;
		HRESULT const aliasResult = aliases.collection->QueryInterface(
			&aliasCollection);
		if (SUCCEEDED(aliasResult))
			*fontCollection = aliasCollection.Detach();
		return aliasResult;
	}
	*fontCollection = systemCollection.Detach();
	return result;
}

HRESULT WINAPI IMPL_Factory3_GetSystemFontSet(
	IDWriteFactory3* self,
	IDWriteFontSet** fontSet)
{
	HCounter counter;
	SignalDirectWriteDiagnostic(L"system-font-set-called");
	if (fontSet == nullptr)
		return E_POINTER;
	*fontSet = nullptr;

	Factory3GetSystemFontSetMethod const original =
		GetOriginalSystemFontSet(self);
	if (original == nullptr)
		return E_UNEXPECTED;
	CComPtr<IDWriteFontSet> systemSet;
	HRESULT const result = original(self, &systemSet);
	if (FAILED(result) || systemSet == nullptr)
		return result;

	directwrite_alias::AliasFontSet aliases;
	directwrite_alias::BuildStatus const status =
		GetCanonicalFactoryAliases(self, aliases);
	SignalAliasCollectionStatus(status);
	if (IsAliasCollectionApplied(status))
	{
		SignalDirectWriteDiagnostic(L"system-font-set-alias-returned");
		return aliases.fontSet.CopyTo(fontSet);
	}
	*fontSet = systemSet.Detach();
	return result;
}

HRESULT WINAPI IMPL_Factory3_GetSystemFontCollection(
	IDWriteFactory3* self,
	BOOL includeDownloadableFonts,
	IDWriteFontCollection1** fontCollection,
	BOOL checkForUpdates)
{
	HCounter counter;
	SignalDirectWriteDiagnostic(L"modern-system-collection-called");
	if (fontCollection == nullptr)
		return E_POINTER;
	*fontCollection = nullptr;

	Factory3GetSystemFontCollectionMethod const original =
		GetOriginalModernSystemFontCollection(self);
	if (original == nullptr)
		return E_UNEXPECTED;
	CComPtr<IDWriteFontCollection1> systemCollection;
	HRESULT const result = original(
		self, includeDownloadableFonts, &systemCollection, checkForUpdates);
	if (FAILED(result) || systemCollection == nullptr)
		return result;

	directwrite_alias::AliasFontSet aliases;
	directwrite_alias::BuildStatus const status =
		GetCanonicalFactoryAliases(self, aliases);
	SignalAliasCollectionStatus(status);
	if (IsAliasCollectionApplied(status))
	{
		SignalDirectWriteDiagnostic(L"modern-system-collection-alias-returned");
		return aliases.collection.CopyTo(fontCollection);
	}
	*fontCollection = systemCollection.Detach();
	return result;
}

static WCHAR const* ResolveFactoryFamilyName(
	WCHAR const* familyName,
	LOGFONT& replacement)
{
	if (familyName == nullptr)
		return familyName;

	SignalDirectWriteDiagnostic(L"find-called");
	SignalDirectWriteFamilyDiagnostic(L"find", familyName);
	LOGFONT source = {};
	source.lfCharSet = DEFAULT_CHARSET;
	if (FAILED(StringCchCopyW(
			source.lfFaceName, ARRAYSIZE(source.lfFaceName), familyName)))
		return familyName;
	replacement = source;

	CGdippSettings const* settings = CGdippSettings::GetInstance();
	if (!settings->CopyForceFont(replacement, source))
		return familyName;
	SignalDirectWriteDiagnostic(L"substitution-resolved");
	SignalDirectWriteFamilyDiagnostic(L"resolved", familyName);
	return replacement.lfFaceName;
}

HRESULT WINAPI IMPL_CreateTextFormat(
	IDWriteFactory* self,
	WCHAR const* fontFamilyName,
	IDWriteFontCollection* fontCollection,
	DWRITE_FONT_WEIGHT fontWeight,
	DWRITE_FONT_STYLE fontStyle,
	DWRITE_FONT_STRETCH fontStretch,
	FLOAT fontSize,
	WCHAR const* localeName,
	IDWriteTextFormat** textFormat)
{
	// Explicit collections own their own naming model. The nullptr path uses
	// the factory's native system collection, so resolve it at this boundary.
	LOGFONT replacement = {};
	WCHAR const* resolvedFamily = fontCollection == nullptr ?
		ResolveFactoryFamilyName(fontFamilyName, replacement) : fontFamilyName;
	return ORIG_CreateTextFormat(
		self,
		resolvedFamily,
		fontCollection,
		fontWeight,
		fontStyle,
		fontStretch,
		fontSize,
		localeName,
		textFormat);
}

void RestoreDirectWriteVtableHooks()
{
	RestoreFactoryAliasVtables();
	HCounter::wait(3000);
	ClearFactoryAliasSources();
	directwrite_alias::ClearCache();
}

void WINAPI IMPL_D2D1RenderTarget_DrawGlyphRun1(
	ID2D1DeviceContext *This,
	D2D1_POINT_2F baselineOrigin,
	CONST DWRITE_GLYPH_RUN *glyphRun,
	CONST DWRITE_GLYPH_RUN_DESCRIPTION *glyphRunDescription,
	ID2D1Brush *foregroundBrush,
	DWRITE_MEASURING_MODE measuringMode
	) {
	Params* dwParams = GetDWParams();
	if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
		D2D1_MATRIX_3X2_F prev;
		This->GetTransform(&prev);
		D2D1_MATRIX_3X2_F rotate = prev;
		rotate.m12 += 1.0f / 0xFFFF;
		rotate.m21 += 1.0f / 0xFFFF;
		This->SetTransform(&rotate);
		ORIG_D2D1RenderTarget_DrawGlyphRun1(
			This,
			baselineOrigin,
			glyphRun,
			glyphRunDescription,
			foregroundBrush,
			measuringMode
			);
		This->SetTransform(&prev);
	}
	else {
		ORIG_D2D1RenderTarget_DrawGlyphRun1(
			This,
			baselineOrigin,
			glyphRun,
			glyphRunDescription,
			foregroundBrush,
			measuringMode
			);
	}
}

void WINAPI IMPL_D2D1RenderTarget1_DrawGlyphRun1(
	ID2D1DeviceContext *This,
	D2D1_POINT_2F baselineOrigin,
	CONST DWRITE_GLYPH_RUN *glyphRun,
	CONST DWRITE_GLYPH_RUN_DESCRIPTION *glyphRunDescription,
	ID2D1Brush *foregroundBrush,
	DWRITE_MEASURING_MODE measuringMode
	) {
	Params* dwParams = GetDWParams();
	if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
		D2D1_MATRIX_3X2_F prev;
		This->GetTransform(&prev);
		D2D1_MATRIX_3X2_F rotate = prev;
		rotate.m12 += 1.0f / 0xFFFF;
		rotate.m21 += 1.0f / 0xFFFF;
		This->SetTransform(&rotate);
		ORIG_D2D1RenderTarget1_DrawGlyphRun1(
			This,
			baselineOrigin,
			glyphRun,
			glyphRunDescription,
			foregroundBrush,
			measuringMode
			);
		This->SetTransform(&prev);
	}
	else {
		ORIG_D2D1RenderTarget1_DrawGlyphRun1(
			This,
			baselineOrigin,
			glyphRun,
			glyphRunDescription,
			foregroundBrush,
			measuringMode
			);
	}
}

void WINAPI IMPL_D2D1DeviceContext_DrawGlyphRun1(
	ID2D1DeviceContext *This,
	D2D1_POINT_2F baselineOrigin,
	CONST DWRITE_GLYPH_RUN *glyphRun,
	CONST DWRITE_GLYPH_RUN_DESCRIPTION *glyphRunDescription,
	ID2D1Brush *foregroundBrush,
	DWRITE_MEASURING_MODE measuringMode
	) {
	Params* dwParams = GetDWParams();
	if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
		D2D1_MATRIX_3X2_F prev;
		This->GetTransform(&prev);
		D2D1_MATRIX_3X2_F rotate = prev;
		rotate.m12 += 1.0f / 0xFFFF;
		rotate.m21 += 1.0f / 0xFFFF;
		This->SetTransform(&rotate);
		ORIG_D2D1DeviceContext_DrawGlyphRun1(
			This,
			baselineOrigin,
			glyphRun,
			glyphRunDescription,
			foregroundBrush,
			measuringMode
			);
		This->SetTransform(&prev);
	}
	else {
		ORIG_D2D1DeviceContext_DrawGlyphRun1(
			This,
			baselineOrigin,
			glyphRun,
			glyphRunDescription,
			foregroundBrush,
			measuringMode
			);
	}
}

void WINAPI IMPL_D2D1RenderTarget_DrawGlyphRun(
	ID2D1RenderTarget* This,
	D2D1_POINT_2F baselineOrigin,
	CONST DWRITE_GLYPH_RUN *glyphRun,
	ID2D1Brush *foregroundBrush,
	DWRITE_MEASURING_MODE measuringMode
	) {
	Params* dwParams = GetDWParams();
	if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
		D2D1_MATRIX_3X2_F prev;
		This->GetTransform(&prev);
		D2D1_MATRIX_3X2_F rotate = prev;
		rotate.m12 += 1.0f / 0xFFFF;
		rotate.m21 += 1.0f / 0xFFFF;
		This->SetTransform(&rotate);
		ORIG_D2D1RenderTarget_DrawGlyphRun(
			This,
			baselineOrigin,
			glyphRun,
			foregroundBrush,
			measuringMode
			);
		This->SetTransform(&prev);
	}
	else {
		ORIG_D2D1RenderTarget_DrawGlyphRun(
			This,
			baselineOrigin,
			glyphRun,
			foregroundBrush,
			measuringMode
			);
	}
}

void WINAPI IMPL_D2D1RenderTarget1_DrawGlyphRun(
	ID2D1RenderTarget* This,
	D2D1_POINT_2F baselineOrigin,
	CONST DWRITE_GLYPH_RUN *glyphRun,
	ID2D1Brush *foregroundBrush,
	DWRITE_MEASURING_MODE measuringMode
	) {
	Params* dwParams = GetDWParams();
	if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
		D2D1_MATRIX_3X2_F prev;
		This->GetTransform(&prev);
		D2D1_MATRIX_3X2_F rotate = prev;
		rotate.m12 += 1.0f / 0xFFFF;
		rotate.m21 += 1.0f / 0xFFFF;
		This->SetTransform(&rotate);
		ORIG_D2D1RenderTarget1_DrawGlyphRun(
			This,
			baselineOrigin,
			glyphRun,
			foregroundBrush,
			measuringMode
			);
		This->SetTransform(&prev);
	}
	else {
		ORIG_D2D1RenderTarget1_DrawGlyphRun(
			This,
			baselineOrigin,
			glyphRun,
			foregroundBrush,
			measuringMode
			);
	}
}

void WINAPI IMPL_D2D1DeviceContext_DrawGlyphRun(
	ID2D1DeviceContext* This,
	D2D1_POINT_2F baselineOrigin,
	CONST DWRITE_GLYPH_RUN *glyphRun,
	ID2D1Brush *foregroundBrush,
	DWRITE_MEASURING_MODE measuringMode
	) {
	Params* dwParams = GetDWParams();
	if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
		D2D1_MATRIX_3X2_F prev;
		This->GetTransform(&prev);
		D2D1_MATRIX_3X2_F rotate = prev;
		rotate.m12 += 1.0f / 0xFFFF;
		rotate.m21 += 1.0f / 0xFFFF;
		This->SetTransform(&rotate);
		ORIG_D2D1DeviceContext_DrawGlyphRun(
			This,
			baselineOrigin,
			glyphRun,
			foregroundBrush,
			measuringMode
			);
		This->SetTransform(&prev);
	}
	else {
		ORIG_D2D1DeviceContext_DrawGlyphRun(
			This,
			baselineOrigin,
			glyphRun,
			foregroundBrush,
			measuringMode
			);
	}
}

HRESULT WINAPI IMPL_BitmapRenderTarget_DrawGlyphRun(
	IDWriteBitmapRenderTarget* This,
	FLOAT baselineOriginX,
	FLOAT baselineOriginY,
	DWRITE_MEASURING_MODE measuringMode,
	DWRITE_GLYPH_RUN const* glyphRun,
	IDWriteRenderingParams* renderingParams,
	COLORREF textColor,
	RECT* blackBoxRect)
{
	Params* dwParams = GetDWParams();
	HRESULT hr = E_FAIL;
	if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
		DWRITE_MATRIX prev;
		hr = This->GetCurrentTransform(&prev);
		if (SUCCEEDED(hr)) {
			DWRITE_MATRIX rotate = prev;
			rotate.m12 += 1.0f / 0xFFFF;
			rotate.m21 += 1.0f / 0xFFFF;
			hr = This->SetCurrentTransform(&rotate);
			if (SUCCEEDED(hr)) {
				hr = ORIG_BitmapRenderTarget_DrawGlyphRun(
					This,
					baselineOriginX,
					baselineOriginY,
					measuringMode,
					glyphRun,
					GetDWRenderingParams(renderingParams),
					textColor,
					blackBoxRect
					);
				This->SetCurrentTransform(&prev);
			}
		}
	}
	if (FAILED(hr)) {
		hr = ORIG_BitmapRenderTarget_DrawGlyphRun(
			This,
			baselineOriginX,
			baselineOriginY,
			measuringMode,
			glyphRun,
			GetDWRenderingParams(renderingParams),
			textColor,
			blackBoxRect
			);
		if SUCCEEDED(hr) {
			MyDebug(L"DrawGlyphRun hooked");
		}
	}
	if (FAILED(hr)) {
		hr = ORIG_BitmapRenderTarget_DrawGlyphRun(
			This,
			baselineOriginX,
			baselineOriginY,
			measuringMode,
			glyphRun,
			renderingParams,
			textColor,
			blackBoxRect
			);
		if SUCCEEDED(hr) {
			MyDebug(L"DrawGlyphRun failed");
		}
	}
	MyDebug(L"DrawGlyphRun hooked");
	return hr;
}


void WINAPI IMPL_D2D1RenderTarget_DrawText(
	ID2D1RenderTarget* This,
	CONST WCHAR *string,
	UINT32 stringLength,
	IDWriteTextFormat *textFormat,
	CONST D2D1_RECT_F *layoutRect,
	ID2D1Brush *defaultForegroundBrush,
	D2D1_DRAW_TEXT_OPTIONS options,
	DWRITE_MEASURING_MODE measuringMode
	) {
	Params* dwParams = GetDWParams();
	if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
		D2D1_MATRIX_3X2_F prev;
		This->GetTransform(&prev);
		D2D1_MATRIX_3X2_F rotate = prev;
		rotate.m12 += 1.0f / 0xFFFF;
		rotate.m21 += 1.0f / 0xFFFF;
		This->SetTransform(&rotate);
		ORIG_D2D1RenderTarget_DrawText(
			This,
			string,
			stringLength,
			textFormat,
			layoutRect,
			defaultForegroundBrush,
			options,
			measuringMode
			);
		This->SetTransform(&prev);
	}
	else {
		ORIG_D2D1RenderTarget_DrawText(
			This,
			string,
			stringLength,
			textFormat,
			layoutRect,
			defaultForegroundBrush,
			options,
			measuringMode
			);
	}
}

void WINAPI IMPL_D2D1DeviceContext_DrawText(
	ID2D1DeviceContext* This,
	CONST WCHAR *string,
	UINT32 stringLength,
	IDWriteTextFormat *textFormat,
	CONST D2D1_RECT_F *layoutRect,
	ID2D1Brush *defaultForegroundBrush,
	D2D1_DRAW_TEXT_OPTIONS options,
	DWRITE_MEASURING_MODE measuringMode
	) {
	Params* dwParams = GetDWParams();
	if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
		D2D1_MATRIX_3X2_F prev;
		This->GetTransform(&prev);
		D2D1_MATRIX_3X2_F rotate = prev;
		rotate.m12 += 1.0f / 0xFFFF;
		rotate.m21 += 1.0f / 0xFFFF;
		This->SetTransform(&rotate);
		ORIG_D2D1DeviceContext_DrawText(
			This,
			string,
			stringLength,
			textFormat,
			layoutRect,
			defaultForegroundBrush,
			options,
			measuringMode
			);
		This->SetTransform(&prev);
	}
	else {
		ORIG_D2D1DeviceContext_DrawText(
			This,
			string,
			stringLength,
			textFormat,
			layoutRect,
			defaultForegroundBrush,
			options,
			measuringMode
			);
	}
}

void WINAPI IMPL_D2D1RenderTarget_DrawTextLayout(
	ID2D1RenderTarget* This,
	D2D1_POINT_2F origin,
	IDWriteTextLayout *textLayout,
	ID2D1Brush *defaultForegroundBrush,
	D2D1_DRAW_TEXT_OPTIONS options
	) {
	Params* dwParams = GetDWParams();
	if (dwParams->GridFitMode == DWRITE_GRID_FIT_MODE_DISABLED) {
		D2D1_MATRIX_3X2_F prev;
		This->GetTransform(&prev);
		D2D1_MATRIX_3X2_F rotate = prev;
		rotate.m12 += 1.0f / 0xFFFF;
		rotate.m21 += 1.0f / 0xFFFF;
		This->SetTransform(&rotate);
		ORIG_D2D1RenderTarget_DrawTextLayout(
			This,
			origin,
			textLayout,
			defaultForegroundBrush,
			options
			);
		This->SetTransform(&prev);
	}
	else {
		ORIG_D2D1RenderTarget_DrawTextLayout(
			This,
			origin,
			textLayout,
			defaultForegroundBrush,
			options
			);
	}
}

#include "common.h"
#include "unload_lifecycle.h"

#include "font_substitution.h"
#include "profile_runtime.h"
#include "settings.h"

#include <atomic>
#include <mutex>

#ifdef INFINALITY
#include <freetype/ftenv.h>
#endif

class FreeTypeFontEngine;
extern FreeTypeFontEngine* g_pFTEngine;
extern FT_Library freetype_library;
void DestroyFreeTypeFontEngine() noexcept;
void FontLFree(void);

namespace renderer {

namespace {

std::once_flag g_rendererResourceDrainOnce;
std::atomic<bool> g_rendererResourcesDrained{false};

} // namespace

bool DrainProcessRendererResourcesOutsideLoaderLock() noexcept
{
	try {
		std::call_once(g_rendererResourceDrainOnce, [] {
			if (g_pFTEngine != nullptr)
				DestroyFreeTypeFontEngine();
#ifdef INFINALITY
			FT_freeEnv();
#endif
			if (freetype_library != nullptr)
				FontLFree();
			ClearProcessProfileRuntimeForQuietUnload();
			font_substitution::ClearProcessRegistryForQuietUnload();
			CGdippSettings::DestroyInstance();
			g_rendererResourcesDrained.store(true, std::memory_order_release);
		});
		return g_rendererResourcesDrained.load(std::memory_order_acquire);
	}
	catch (...) {
		return false;
	}
}

} // namespace renderer

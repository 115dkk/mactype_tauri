import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

const root = new URL('../../../', import.meta.url);

async function source(path) {
  return readFile(new URL(path, root), 'utf8');
}

test('the DirectWrite lifecycle owns existing, future, and teardown paths', async () => {
  const [
    hookList,
    directWrite,
    directWriteHeader,
    exportsSource,
    hookSource,
    unloadProviders,
    unloadResources,
    activationSource,
    contractProbe,
  ] = await Promise.all([
    source('renderer/hooklist.h'),
    source('renderer/directwrite.cpp'),
    source('renderer/directwrite.h'),
    source('renderer/expfunc.cpp'),
    source('renderer/hook.cpp'),
    source('renderer/unload_providers.cpp'),
    source('renderer/unload_resources.cpp'),
    source('renderer/renderer_activation.cpp'),
    source('tools/service-probe/tests/dwritecore_contract_probe.cpp'),
  ]);

  assert.match(hookList, /HOOK_MANUALLY\(HRESULT, DWriteCoreCreateFactory,/);
  assert.match(directWrite, /IMPL_DWriteCoreCreateFactory/);
  assert.match(directWrite, /GetModuleHandleW\(L"DWriteCore\.dll"\)/);
  assert.match(directWrite, /GetProcAddress\([^\n]+, "DWriteCoreCreateFactory"\)/);
  assert.match(directWrite, /hook_demand_DWriteCoreCreateFactory/);
  assert.match(directWrite, /ScheduleLoadedDWriteCoreHook/);
  assert.match(hookList, /HOOK_MANUALLY\(LONG, LdrLoadDll,/);
  assert.match(directWrite, /hook_demand_LdrLoadDll/);
  assert.match(directWrite, /IMPL_LdrLoadDll/);
  assert.match(directWrite, /HookKnownDirectWriteFactories/);
  assert.match(directWrite, /DWRITE_FACTORY_TYPE_SHARED/);
  assert.match(directWrite, /DWRITE_FACTORY_TYPE_ISOLATED/);
  assert.match(directWrite, /IMPL_LoadLibraryExW/);
  assert.match(hookList, /HOOK_MANUALLY\(FARPROC, GetProcAddress,/);
  assert.match(directWrite, /IMPL_GetProcAddress/);
  assert.doesNotMatch(directWrite, /return reinterpret_cast<FARPROC>\(&IMPL_DWriteCoreCreateFactory\)/);
  assert.match(directWrite, /return procedure;/);
  assert.match(directWriteHeader, /StartDirectWriteLifecycle/);
  assert.match(directWriteHeader, /PrepareDirectWriteLifecycleStop/);
  assert.match(directWriteHeader, /CommitDirectWriteLifecycleStop/);
  assert.match(
    directWrite,
    /CreateThread\([\s\S]+?CREATE_SUSPENDED[\s\S]+?dwriteCoreWorker = std::move\(thread\)[\s\S]+?ResumeThread/,
  );
  assert.doesNotMatch(exportsSource, /PrepareDirectWriteLifecycleStop/);
  assert.match(exportsSource, /MakeProcessRendererProviderDrainTransaction\(\)/);
  assert.match(exportsSource, /providerDrain\.Prepare\(\)/);
  assert.match(
    exportsSource,
    /#define HOOK_MANUALLY HOOK_DEFINE[\s\S]+transaction\.Commit\(\)[\s\S]+providerDrain\.Commit\(\)/,
  );
  assert.match(unloadProviders, /PrepareDirectWriteLifecycleStop\(timeout\)/);
  assert.match(unloadProviders, /CommitDirectWriteLifecycleStop\(timeout\)/);
  assert.match(unloadProviders, /PrepareUnityFontHookLifecycleStop\(timeout\)/);
  assert.match(unloadProviders, /CommitUnityFontHookLifecycleStop\(\)/);
  assert.match(
    unloadProviders,
    /PrepareDirectWrite[\s\S]+PrepareUnity[\s\S]+RendererProviderDrainTransaction/,
  );
  assert.match(exportsSource, /reinterpret_cast<PVOID>\(REF_##name\)/);
  assert.doesNotMatch(exportsSource, /ReleasePinnedRendererModules/);

  assert.match(
    exportsSource,
    /CompleteStop\(\)[\s\S]+DrainProcessRendererResourcesOutsideLoaderLock\(\)[\s\S]+TakeProcessRendererLease\(\)/,
  );
  assert.match(
    activationSource,
    /QUIET_SKIP[\s\S]+DrainProcessRendererResourcesOutsideLoaderLock\(\)/,
  );
  assert.match(unloadResources, /std::call_once\(g_rendererResourceDrainOnce/);
  const processDetach = hookSource.match(
    /case DLL_PROCESS_DETACH:([\s\S]*?)\n\s*break;/,
  );
  assert.ok(processDetach, 'DLL_PROCESS_DETACH must remain structurally visible');
  assert.doesNotMatch(
    processDetach[1],
    /DestroyFreeTypeFontEngine|FT_freeEnv|FontLFree|CGdippSettings::DestroyInstance/,
  );
  assert.match(processDetach[1], /g_TLInfo\.ProcessTerm\(\)/);
  assert.match(contractProbe, /final_caller_reference = LoadLibraryW/);
  assert.match(contractProbe, /SafeUnload consumed a caller-owned renderer reference/);
  assert.match(contractProbe, /a stopped renderer admitted a mutable control interface/);
  assert.match(contractProbe, /FreeLibrary\(final_caller_reference\)/);
});

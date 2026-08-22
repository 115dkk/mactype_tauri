import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

const root = new URL('../../../', import.meta.url);

async function source(path) {
  return readFile(new URL(path, root), 'utf8');
}

test('the DirectWrite lifecycle owns existing, future, and teardown paths', async () => {
  const [hookList, directWrite, directWriteHeader, exportsSource] = await Promise.all([
    source('renderer/hooklist.h'),
    source('renderer/directwrite.cpp'),
    source('renderer/directwrite.h'),
    source('renderer/expfunc.cpp'),
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
  assert.match(exportsSource, /PrepareDirectWriteLifecycleStop\(\)/);
  assert.match(
    exportsSource,
    /#define HOOK_MANUALLY HOOK_DEFINE[\s\S]+transaction\.Commit\(\)[\s\S]+CommitDirectWriteLifecycleStop\(\)/,
  );
  assert.match(exportsSource, /reinterpret_cast<PVOID>\(REF_##name\)/);
  assert.doesNotMatch(exportsSource, /ReleasePinnedRendererModules/);
});

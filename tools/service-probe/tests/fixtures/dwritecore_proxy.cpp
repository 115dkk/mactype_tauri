#include <windows.h>
#include <dwrite.h>

extern "C" HRESULT WINAPI DWriteCoreCreateFactory(
    DWRITE_FACTORY_TYPE factory_type, REFIID interface_id,
    IUnknown** factory) {
  return DWriteCreateFactory(factory_type, interface_id, factory);
}

extern "C" FARPROC WINAPI GetRawDWriteCoreCreateFactory() {
  return reinterpret_cast<FARPROC>(&DWriteCoreCreateFactory);
}

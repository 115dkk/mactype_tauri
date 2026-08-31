# Unity runtime cross-device check

Codex and a development toolchain are not required on the test computer.

1. Verify `MacType-Control-Center-Installer.exe` against `SHA256SUMS.txt`, then
   install it normally. Installation requires administrator approval.
2. Open PowerShell in this directory and run:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass `
     -File .\Get-InstalledRuntimeProvenance.ps1 `
     -ExpectedManifest .\expected-runtime-manifest.json `
     -OutputPath .\runtime-provenance.json
   ```

   The diagnostic is read-only and does not require administrator rights.
3. In `runtime-provenance.json`, require all of the following before testing a
   game:

   - `service.receiptVerified` is `true`.
   - `release.matches` is `true`.
   - `profile.digestVerified` is `true`.
   - `profile.unityFontHook` is `2` for **Most games**.
   - `profile.skipPrivateFreeType` records the independent non-Unity
     compatibility option; either value is valid for this Unity test.
   - `issues` is empty.

If any item differs, preserve the JSON and do not classify the result as a
renderer regression. Close and restart a game after changing the profile or
upgrading the service because already injected processes keep their original
runtime generation.

# MacType Control Center Integration/Developer Bundle

This bundle is for MacType maintainers, installer integrators, and Control Center developers. It is not a portable or independently installed product.

`installation-tree/` reproduces the complete application directory written by the standalone MacType Control Center installer. Keep the tree intact when integrating it into another installer:

- `MacType Control Center.exe`
- `mactype-preview32.exe`
- the x86/x64 MacType core and loader files
- `service-runtime/mactype-service-setup.exe`
- `service-runtime/payload/manifest.json`
- every file named by the runtime manifest
- profiles, language catalogs, and license notices

Running only `MacType Control Center.exe` from an extracted or development directory is supported for inspection and development, but that process is not an installed Control Center. Service installation and maintenance stay disabled unless the complete tree has first been placed under a protected Program Files root and that root has been registered in the 64-bit local-machine installation receipt.

An integrating installer must:

1. Install the complete `installation-tree/` contents under one protected Program Files application root.
2. Preserve the relative paths in this bundle.
3. Write that application root to the `InstallLocation` string value under `HKLM64\SOFTWARE\MacType\ControlCenter`.
4. Prevent unprivileged users from replacing the installed root or any required file.
5. Remove or update the receipt transactionally with the installed tree.

The Control Center canonicalizes the registered root, rejects reparse points, confirms that it remains below Program Files, and verifies the Control Center executable, setup broker, and runtime manifest first. A missing, incomplete, or untrusted registration leaves service operations read-only and does not request UAC or attempt rollback.

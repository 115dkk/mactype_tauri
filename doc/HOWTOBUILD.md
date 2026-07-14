# How to build

 1. **Compiler / IDE**

    Visual Studio 2019 with v142 toolkit has been tested and is working. Toolkits down to v120 should be able to compile the code, but be aware that the `_xp` ones might refuse to use the Windows 10 SDK.

 2. **Dependencies**

    Mactype depends on
     - [Freetype](https://www.freetype.org/download.html)
       - For the lastest version of Mactype, a customized version of FreeType is required, which can be obtained from https://github.com/snowie2000/freetype    
     - [EasyHook](http://easyhook.github.io/) / [Detours](https://github.com/microsoft/Detours)
     - [IniParser (fork)](https://github.com/snowie2000/IniParser)
     - [wow64ext (fork)](https://github.com/snowie2000/rewolf-wow64ext)
     - Windows SDK (10.0.14393.0 or later)

 3. **Building dependencies**

    - FreeType

        Apply `glyph_to_bitmapex.diff` before building.

        Always build multi-thread release.

        Remember to enable options you want in ftoptions.h

        Compile freetype as freetype.lib for x86 and freetype64.lib for x64

        Static library is preferred, you are free to build freetype as independent dlls with better interchangeability but you will lose some compatibility in return, for some programs are delivered with their own copies of freetype which will conflict with your file.

        Set `FREETYPE_PATH` environment variable to root of freetype source.

    - iniParser

        Build as iniparser.lib and iniparser64.lib. Set `INI_PARSER_PATH` environment variable to root of IniParser project.

    - wow64ext

        Build as wow64ext.lib. x64 library is not required. Shared library also works if you prefer that.

    - EasyHook

        Only EasyHookDll project is required.

        Build it as easyhook32.lib and easyhook64.lib, or get the binary distributions.

        Dll filename is not important but you'd better give it a special name to avoid dll confliction as stated above. Do not forget to modify filename in `hook.cpp` of MacType.

    - Detours

        Since Microsoft Detours is now free and opensource, it is back to be supported and recommended.

        Follow the official guide to build detours.lib and detours64.lib and put them in the root of MacType.

        Detours lib are static libraries, so name confiction is not a thing.

    - Windows SDK

        Actually it's not something you need to build, but the installation is tricky.

        One word to rule them all: download **ALL COMPONENTS**  in the installation list! Unless you want to waste several hours looking for these mysterious dependencies it pops to you. Don't worry, you will have a second chance to choose which component you want to install after download.

 4. **Build**

    Last but easiest step: Put all `.lib` files you built earlier into a `lib` folder in the root of MacType, click build and enjoy.

## Building the Control Center (Tauri)

The Control Center can be built separately from the legacy MacType solution. The repository script builds the 32-bit Preview Helper, runs its protocol tests, builds the React frontend, and compiles the Tauri executable. It does not modify an installed copy of MacType.

### Prerequisites

- Windows 10 or 11
- Visual Studio 2022 Build Tools with **Desktop development with C++**, MSVC, CMake, and a Windows SDK
- Node.js 22
- pnpm 11.7.0
- the stable Rust MSVC toolchain
- Microsoft Edge WebView2 Runtime, which is included with supported Windows versions

After installing Node.js and `rustup`, enable the repository's pnpm and Rust versions:

```powershell
corepack enable
corepack prepare pnpm@11.7.0 --activate
rustup toolchain install stable-x86_64-pc-windows-msvc
rustup default stable-x86_64-pc-windows-msvc
```

From the repository root, run:

```powershell
pwsh -NoProfile -File .github/scripts/Build-ControlCenter.ps1 -Configuration Release
```

The script installs the locked frontend dependencies automatically. A successful build produces:

- `control-center/src-tauri/target/release/mactype-control-center.exe`
- `build/preview-helper/Release/mactype-preview32.exe`

For local testing, launch the Control Center from the repository root so it can find the development copy of the Preview Helper:

```powershell
.\control-center\src-tauri\target\release\mactype-control-center.exe
```

The Control Center can discover an existing MacType installation, or let the user select one through the interface.

For frontend-only design work that does not require Rust or native Windows integration, see the [Control Center design maintenance guide](../docs/design-maintenance.md).

## Build an installer with GitHub Actions

Maintainers do not need to reproduce the complete legacy-core and installer toolchain locally. The repository contains a manual Windows build workflow that creates an installable package on a GitHub-hosted runner.

1. Open the repository's **Actions** tab.
2. Select **Build MacType Control Center**.
3. Select **Run workflow**.
4. Choose the branch to build.
5. Enter a version such as `0.1.0` or `0.1.0-preview.1`.
6. Select **Run workflow** and wait for both build jobs to finish.
7. Open the completed run and download `mactype-control-center-<version>` from **Artifacts**.

The artifact contains the per-user Inno Setup installer, `SHA256SUMS.txt`, the standalone Control Center executable, and the Preview Helper. It is retained for 14 days. The workflow builds the x86 and x64 MacType core from source before packaging the Control Center, so no prebuilt core files need to be prepared by the maintainer.

The workflow is defined in [`.github/workflows/build.yml`](../.github/workflows/build.yml). It runs only when explicitly dispatched; opening or merging a pull request does not start it automatically, and it does not publish a GitHub Release.

## FAQ

Q: Where are the sources of loader and tuner in the repo?

A: I'm sorry, but they are still closed-source right now. Since you have the mactype source and will surely have a good understanding of how mactype works, I believe it's not a big challenge to write a loader for it.
If you wrote a great loader or something else wonderful, please post an issue or a pull request. Hope we can make MacType better!

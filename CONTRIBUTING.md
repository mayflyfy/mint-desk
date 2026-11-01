# Contributing

Keep contributions focused on desktop organization, reliability, and accessibility.

Use Windows x64 with CMake 3.16+ and Visual Studio, or MinGW-w64. Run `tests/run-tests.ps1` with MinGW-w64 before submitting behavioral changes.

## Build and verify

MinGW-w64 (optionally pass `-Compiler C:\path\to\g++.exe`):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\run-tests.ps1
```

Visual Studio / CMake:

```powershell
cmake -S . -B build-msvc -A x64
cmake --build build-msvc --config Release
```

Builds statically link the C++ runtime and import only Windows system DLLs. MinGW output is `dist/MintDesk.exe`.

## Screenshots and releases

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\render-screenshots.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-release.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\audit-release.ps1 -IncludeArtifacts
```

Building the installer requires Inno Setup 7.1+ on the development machine only. `tools/get-inno.ps1` fetches the pinned official compiler, verifies SHA-256 and its Authenticode signature, and extracts it in portable mode. Pass its returned path via `-Iscc` to `tools/package-release.ps1`. `-SkipInstaller` packages only the portable distribution.

Run `tests/installer_tests.ps1 -Iscc C:\path\to\ISCC.exe` for installation, upgrades, opt-outs, restoration failure, and uninstall checks. These use a test-only payload, a dedicated registry key, and temporary links/data; they never install over the user's actual application. The native integration and maintenance suites test the production code independently.

Screenshots use the production renderer with fictional items. The portable ZIP contains only the EXE, license, and usage note; the installer contains the app and license. Stage current source before auditing; never include personal layouts, recovery journals, development backups, or build outputs in Git.

## Contribution rules

Tests must use dedicated temporary fixtures. Do not use real desktop files, installed applications, personal layouts, or external network shares. Never force-kill an instance restoring user files.

- Keep payloads in place. Do not add deletion, replacement, or silent copying of user files.
- Persist recovery intent before changing visibility; preserve unrelated attributes.
- Check file identity on restoration and keep helper operations narrowly scoped.
- Avoid permanent polling, notifications, telemetry, and unrelated features.
- Remove credentials, account names, paths, and private project names from reports. Use synthetic screenshots.

Contributions are provided under the project's MIT license.

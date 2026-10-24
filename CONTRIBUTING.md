# Contributing

Keep contributions focused on desktop organization, reliability, and accessibility.

Use Windows x64 with CMake 3.16+ and Visual Studio, or MinGW-w64. See README build commands. Run `tests/run-tests.ps1` with MinGW-w64 before submitting behavioral changes.

Tests must use dedicated temporary fixtures. Do not use real desktop files, installed applications, personal layouts, or external network shares. Never force-kill an instance restoring user files.

- Keep payloads in place. Do not add deletion, replacement, or silent copying of user files.
- Persist recovery intent before changing visibility; preserve unrelated attributes.
- Check file identity on restoration and keep helper operations narrowly scoped.
- Avoid permanent polling, notifications, telemetry, and unrelated features.
- Remove credentials, account names, paths, and private project names from reports. Use synthetic screenshots.

Contributions are provided under the project's MIT license.

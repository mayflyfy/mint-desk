# Installer

`MintDesk.iss` builds a per-user Windows x64 installer with Inno Setup 7.1+.
The compiler is a development dependency; it is not installed on users' machines.
The app remains a statically linked portable executable without new runtime dependencies.

Keep the production AppId stable across releases. Installed upgrades stay in the
registered directory; uninstall preserves all user AppData and recovery records.
The desktop shortcut is created through the app's ownership-aware native code,
not an unconditional installer file replacement. Automatic update checking is
only a DWORD preference in this version.

`TestMode` changes the product identity and registry keys. Use it only with the
test-only payload from `tests/installer_fixture.cpp`; never distribute that build.

Inno Setup is developed by Jordan Russell and Martijn Laan. Its setup/uninstaller
runtime is subject to the [Inno Setup license](https://github.com/jrsoftware/issrc/blob/main/license.txt).
MintDesk source and payload remain MIT, copyright MCC.

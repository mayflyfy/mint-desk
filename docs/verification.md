# Verification

Current version: v0.9.1, Windows x64. Results describe tested environments, not every machine.

## Automated checks

`tests/run-tests.ps1` builds twelve native suites with temporary fixtures:

| Suite | Coverage |
| --- | --- |
| state | CRC, backup recovery, Unicode, layouts, monitor geometry |
| shell | Path validation, icon ownership, missing local files and shortcut targets, preservation of remote references |
| snap | Screen/card alignment, cumulative one-pixel drag/resize escape, guide geometry, negative coordinates, DPI |
| desktop_menu | Isolated registry registration, quoting, one dynamic action |
| startup_integration | First-run defaults, installer choices, persistent opt-outs, reserved update preference, partial retry, enabled-entry path refresh; isolated native shortcut creation, identity/visibility preservation, collision and ownership protection |
| install_support | Normal shutdown of isolated controller processes, bounded waits, unfinished restoration never force-terminated |
| alpha_surface | Background transparency with opaque foreground content |
| desktop_items | Legacy recovery, collision handling, durable intent |
| hidden_items | In-place visibility, occupied folders, identity, attribute restoration, corrupt/write-failed journals, watcher recovery |
| file_helper | Narrow path/identity boundaries, no replacement, limited attribute changes |
| dragdrop | OLE ownership, cancellation, real Shell drag-image initialization, private IStream handoff, legacy icon alpha/masks, drag-image cleanup, external-drop preparation |
| ui | Desktop-host fallback, bypassed topmost requests, foreground-independent reorder repair, multi-card/glass ordering, coalesced repair quiescence; selection, layouts, guides, keyboard movement, editing, collapse, menus, transfer |

Tests do not require real user files or installed third-party apps. Geometry and compositor checks depend on the Windows desktop session.

`tests/installer_tests.ps1` builds a separate test installer and test-only payload, and exercises fresh installation defaults, explicit opt-outs, saved upgrade choices, v0.8.x portable preferences, directory selection, restoration failure blocking upgrade/uninstall, integration cleanup, and preservation of fictional desktop/data files. It uses isolated temporary folders and registry keys; it does not install over the user's actual application. Native suites independently test the production integration and shutdown code.

## Local observations

- The v0.9.1 isolated installer lifecycle suite passed fresh installation, upgrades, saved opt-outs, portable migration, blocked restoration, and uninstall preservation. Chinese fixture names are now read correctly by Windows PowerShell as well as PowerShell 7.
- Updated screenshots contain seven compact native cards, real Windows Shell document/application/folder icons, grid and list views, locked and collapsed cards, and drag feedback. All named files and directories are isolated examples; no personal desktop image or layout was published.
- v0.9.1 passed all twelve native suites. Isolated UI fixtures deleted a fictional program and shortcut using native filesystem operations, verified automatic removal from multiple cards, retained surviving item identity/selection, and reloaded the cleaned saved layout. Checks defer during modal loops and the notification timer returns to idle. Real Shell source-helper initialization passes for both a mask-based icon and the not-yet-loaded fallback. No installed third-party application or personal layout was modified. The release executable is 439,808 bytes; the installer is 2,223,956 bytes.
- All twelve native suites and the isolated installer lifecycle checks passed for v0.9.0, using Windows x64 and MinGW-w64 GCC 10.3. Native WM_MOVING regression tests feed consecutive one-pixel pointer positions and incremental window proposals; they do not inject system mouse input.
- Layer tests use isolated native windows to request topmost status with WINDOWPOSCHANGING bypassed, reorder an ordinary window without changing foreground, promote a blur surface separately, and verify all cards return below the ordinary window. Missing/transient desktop anchors fall to HWND_BOTTOM; Explorer icon-view creation schedules repair. Queued repairs settle instead of continuously repositioning the same windows.
- Renaming retains the old recovery directory when present, without moving/copying its journals. New installations use the MintDesk data directory. Cross-version singleton and mutation mutex identities remain compatible.
- Synthetic items kept their original paths, identities, and bytes while hidden. Explorer desktop view removal was approximately 31–32 ms in a local sample. They remained hidden after refresh and reappeared on restoration.
- Timing measures the desktop view collection, not mouse-to-screen latency or all systems.
- A startup/exit smoke test checked file identities and hashes, restored original visibility, and retained layout.
- A live first-run check confirmed the MintDesk autostart command and the single renamed desktop context-menu entry. Actual Windows login execution has not been tested by rebooting.
- A temporary child exited without cleanup; a separate watcher recovered synthetic fixture visibility. No user's normal instance was force-closed.
- A temporary external copy retained original visibility instead of temporary organizer flags.
- The EXE imports only Windows system DLLs; no extra runtime installation is needed.
- The native v0.8.2 release EXE is 416 KiB (425,984 bytes). One short UI sample used 31.07 MiB working set and 9.34 MiB private memory, with 31.25 ms main-process CPU time over five idle seconds; the recovery watcher and optional permission helper are separate processes. These are observations, not minimum-resource guarantees.
- A local in-place update retained desktop item identities, file hashes, visibility attributes, saved cards, file-reference order, appearance and lock preferences. Layout geometry was unchanged in this sample. Normal exit completed restoration and all helper processes ended.
- A read-only live window-order check found the visible card and blur surfaces above Explorer's icon host and below the ordinary foreground application, with no surface marked topmost. It inspected window classes/order only and did not capture personal desktop content.
- Outside-click dismissal installs its event-driven mouse hook only while an item is selected, forwards all input unchanged, defers menu dismissal until its command completes, and removes the hook when selection clears. Regression coverage includes desktop clicks without a foreground change.
- The Windows CI workflow builds all twelve suites, tests the installer with isolated fixtures, packages both distributions, and audits source and release contents.
- v0.9.0 adds a roughly 2.2 MB installer; its standalone app is 430,592 bytes. Inno Setup runs only during installation/uninstallation, and adds no background service or app runtime dependency. Automatic update checking is a stored preference with no network implementation in this version. This version has not been used to replace the user's live installation during development.

## Screenshot provenance

`docs/screenshots/` is generated by `tools/render-screenshots.cpp` with the production card renderer and fictional demo items. Backgrounds are synthetic. No live desktop, account name, installed-software inventory, personal filenames, or private configuration is captured. Images illustrate the UI; they do not reproduce complete Windows compositor blur.

## Remaining limits

- Windows versions, Wallpaper Engine, desktop hosts, mixed-DPI hot-plugging, remote desktop, sleep/resume, and third-party drag receivers need broader real-world testing.
- Hidden/System flags affect File Explorer visibility. User settings can reveal protected hidden files.
- Power failure, simultaneous termination of recovery processes, corrupt storage, and external deletion/movement can prevent immediate recovery. Keep independent backups.
- Unsigned executables may prompt Windows security warnings; the project does not suppress them.

# Changelog

## 0.8.1 — 2026-10-25

- Clear item selection when clicking another card or leaving the application.
- Anchor move/resize gestures to cumulative pointer displacement so snapped cards can move away normally.
- Show temporary, click-through alignment guides; retain exact keyboard positioning and the Alt override.
- Preserve custom near-edge positions at startup and on display reflow.
- Reuse per-user layout and recovery data across upgrades, with regression coverage for saved cards and file order.
- Create a native desktop shortcut on normal launch; refresh its target after an executable move without duplicates or overwriting unrelated shortcuts.
- Wait for recovery-journal completion before cleaning up watcher test fixtures.

## 0.8.0 — 2026-10-24

Initial public release.

- Native Windows x64 desktop cards in a single portable executable.
- Compact list and grid layouts, drag sorting, and card-to-card transfers.
- Resizing, locking, collapsing, and snapping to screen and card edges.
- Translucent background with optional native blur; opaque icons and text.
- Per-monitor layout persistence and DPI-aware positioning.
- In-place visibility management with two recovery journals and an independent recovery watcher.
- Restricted helper for protected desktop entries; normal-integrity UI drag and drop.
- First-run autostart and desktop context-menu setup, with persistent manual opt-outs.
- MIT license, Copyright (c) 2026 MCC.

# Satori GC export support

This fork adds the Windows export preset checkbox **Dotnet > Satori Gc > Enabled**.

When enabled, Godot replaces the exported self-contained CoreCLR runtime files with the Satori runtime bundled next to this editor. This is necessary because Satori is an alternate CoreCLR implementation; it is not a runtime switch exposed by the stock .NET runtime.

The bundled runtime is staged by `./dev.ps1 build` from the workspace cache. It is deliberately pinned and checksum-verified:

- Release: `2025.807.0` — a **ppy/Satori** (osu!) GitHub Actions build, not a VSadov/Satori
  release: <https://github.com/ppy/Satori/releases/tag/2025.807.0>, commit
  `098d0ed17cf8f965d79be6e3d870a895a6dbbe04`, `8.0-based` branch, published 2025-08-07.
- Runtime identifier: `win-x64`
- `System.Private.CoreLib` product version: `8.0.16`

Update channel: only ppy `8.0-based` builds while the project targets `net8.0`. ppy's newer
date-stamped builds (`2026.824.0` onward) are .NET-10-based and VSadov/Satori#81 reports worse
stop-the-world pauses there than on .NET 8; do not take them without a measured comparison.

The checkbox is disabled by default in the engine; Dorifto enables it on every export preset and sets `dotnet/runtime/satori_play_from_editor=true` (decision 13 in the project's `docs/notes/Garbage Collection Strategy.md`, 2026-08-26). Exporting with it enabled fails clearly if the editor does not contain the matching runtime, if the target is not `win-x64`, or if the exported project targets a different .NET major version.

Do not add automatic Satori downloads to the exporter. Runtime acquisition belongs to the deterministic build process, not project export.

Godot 4.7's managed tooling and default desktop project target are `net8.0`. The build wrapper uses the isolated .NET 8 SDK in `%USERPROFILE%\\.dotnet`; it passes that exact executable to Godot's managed build helper and does not allow the installed newest-major SDK to silently change this runtime contract.

## Play-from-editor

The game played from the editor is a child of the editor binary and resolves .NET through
hostfxr from the inherited environment (`GodotPlugins.runtimeconfig.json`,
`rollForward: LatestMajor`), so by default it runs whatever newest major is installed — not the
runtime the export bundles. Two project settings, applied only to the play child by
`GDMono::push_play_runtime_environment` (`editor/run/editor_run.cpp`):

- `dotnet/runtime/pin_play_to_export` (default on): sets `DOTNET_ROLL_FORWARD=LatestPatch` so
  the child stays on the 8.0.x line the export bundles.
- `dotnet/runtime/satori_play_from_editor` (default off): points `DOTNET_ROOT` at
  `bin/GodotSharp/Tools/SatoriPlay/win-x64/dotnet`, a private root staged by `./dev.ps1 build`
  from the newest installed 8.0.x shared framework with the Satori overlay applied.

The editor process itself never runs Satori.

## Known race: godot#83762

`CSharpInstance::mono_object_disposed*` used to `CRASH_COND` when the GC handle had already
been released by another thread. The releaser it calls double-checks under a mutex and
tolerates that, so this fork downgrades the assert to `WARN_PRINT_ONCE` (dev builds only;
release builds never compiled it in).

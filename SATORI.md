# Satori GC export support

This fork adds the Windows export preset checkbox **Dotnet > Satori Gc > Enabled**.

When enabled, Godot replaces the exported self-contained CoreCLR runtime files with the Satori runtime bundled next to this editor. This is necessary because Satori is an alternate CoreCLR implementation; it is not a runtime switch exposed by the stock .NET runtime.

The bundled runtime is staged by `./dev.ps1 build` from the workspace cache. It is deliberately pinned and checksum-verified:

- Release: `2026.824.0` — a **ppy/Satori** (osu!) GitHub Actions build, not a VSadov/Satori
  release: <https://github.com/ppy/Satori/releases/tag/2026.824.0>, commit
  `f04020652c256b3ab11aec1e8eb4e270012a5196`, `main` (.NET-10-based), published 2026-08-24.
- Runtime identifier: `win-x64`
- `System.Private.CoreLib` product version: `10.0.4-dev`

Update channel: only ppy builds from the .NET-10-based line while the project targets
`net10.0`. Note that the CoreLib carries a `-dev` prerelease suffix and that
`coreclr.dll`/`clrjit.dll` report the placeholder file version `42.42.42.42424`; the CoreLib
product version is the only usable identity signal, and Dorifto's
`RuntimeIdentityValidator.SatoriCoreLibVersion` pins it.

VSadov/Satori#81 reports worse stop-the-world pauses on the .NET 10 line than on .NET 8. That
is why Dorifto re-runs its own `gcbench` matrix after every Satori pin change rather than
trusting the upstream report in either direction.

The checkbox is disabled by default in the engine; Dorifto enables it on every export preset and sets `dotnet/runtime/satori_play_from_editor=true` (decision 13 in the project's `docs/notes/Garbage Collection Strategy.md`, 2026-08-26). Exporting with it enabled fails clearly if the editor does not contain the matching runtime, if the target is not `win-x64`, or if the exported project targets a different .NET major version.

Do not add automatic Satori downloads to the exporter. Runtime acquisition belongs to the deterministic build process, not project export.

This fork's managed tooling targets `net10.0`. The build wrapper uses the isolated .NET 10 SDK in `%USERPROFILE%\\.dotnet`; it passes that exact executable to Godot's managed build helper and does not allow the installed newest-major SDK to silently change this runtime contract.

## Play-from-editor

The game played from the editor is a child of the editor binary and resolves .NET through
hostfxr from the inherited environment (`GodotPlugins.runtimeconfig.json`,
`rollForward: LatestMajor`), so by default it runs whatever newest major is installed — not the
runtime the export bundles. Two project settings, applied only to the play child by
`GDMono::push_play_runtime_environment` (`editor/run/editor_run.cpp`):

- `dotnet/runtime/pin_play_to_export` (default on): sets `DOTNET_ROLL_FORWARD=LatestPatch` so
  the child stays on the 10.0.x line the export bundles.
- `dotnet/runtime/satori_play_from_editor` (default off): points `DOTNET_ROOT` at
  `bin/GodotSharp/Tools/SatoriPlay/win-x64/dotnet`, a private root staged by `./dev.ps1 build`
  from the newest installed 10.0.x shared framework with the Satori overlay applied.

The editor process itself never runs Satori.

`pin_play_to_export` resolves `LatestPatch` against the framework declared in
`GodotPlugins.runtimeconfig.json`, so **that config's major is what the pin actually pins to**.
`GodotPlugins.csproj` must therefore track the major the exported game targets. While it said
`net8.0` and the project targeted `net10.0`, the default-on pin sent the play child to a 8.0.x
runtime, where the game assembly failed to load with
`FileNotFoundException: System.Runtime, Version=10.0.0.0` — **and the process still exited 0**.
Keep the two in step.

## Known race: godot#83762

`CSharpInstance::mono_object_disposed*` used to `CRASH_COND` when the GC handle had already
been released by another thread. The releaser it calls double-checks under a mutex and
tolerates that, so this fork downgrades the assert to `WARN_PRINT_ONCE` (dev builds only;
release builds never compiled it in).

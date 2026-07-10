# Satori GC export support

This fork adds the Windows export preset checkbox **Dotnet > Satori Gc > Enabled**.

When enabled, Godot replaces the exported self-contained CoreCLR runtime files with the Satori runtime bundled next to this editor. This is necessary because Satori is an alternate CoreCLR implementation; it is not a runtime switch exposed by the stock .NET runtime.

The bundled runtime is staged by `./dev.ps1 build` from the workspace cache. It is deliberately pinned and checksum-verified:

- Release: `2025.807.0`
- Runtime identifier: `win-x64`
- `System.Private.CoreLib` product version: `8.0.16`

The checkbox is disabled by default. Exporting with it enabled fails clearly if the editor does not contain the matching runtime, if the target is not `win-x64`, or if the exported project targets a different .NET major version.

Do not add automatic Satori downloads to the exporter. Runtime acquisition belongs to the deterministic build process, not project export.

Godot 4.7's managed tooling and default desktop project target are `net8.0`. The build wrapper uses the isolated .NET 8 SDK in `%USERPROFILE%\\.dotnet`; it passes that exact executable to Godot's managed build helper and does not allow the installed newest-major SDK to silently change this runtime contract.

# Godot fork instructions

## Required command surface

- Use PowerShell 7 and start with `pwsh -NoProfile -File ./dev.ps1 doctor`.
- Build this Windows editor with `pwsh -NoProfile -File ./dev.ps1 build`.
- Build the Windows x64 export templates with `pwsh -NoProfile -File ./dev.ps1 build-templates`.
- The wrapper performs the required order: SCons native editor build, headless C# glue generation, managed assembly build, then Satori runtime staging.
- Do not call `scons`, `build_assemblies.py`, or a bare `godot` executable directly for normal work.

## Toolchain

- This Godot 4.7 fork targets `net8.0`; `dev.ps1` selects its isolated .NET 8 SDK explicitly.
- Satori is supported only for `win-x64` exports in this channel. It is bundled into the built editor from the checksum-verified workspace cache.
- The Export preset checkbox **Dotnet > Satori Gc > Enabled** is off by default. It replaces the published self-contained CoreCLR files with the bundled Satori runtime.

## Safety

- Do not add automatic release downloads to the exporter. Runtime acquisition and checksum verification belong in `dev.ps1`.
- Do not commit, push, change the upstream remote, or alter the pinned Satori release without explicit authorization.

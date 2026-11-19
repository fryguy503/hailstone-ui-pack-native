# Hailstone UI Pack Native

The optional Windows x64 rendering companion for all twenty skins in
[Hailstone UI Pack](https://github.com/fryguy503/hailstone-ui-pack).

Install both add-ons through Hailstone and select any UI Pack skin from the
normal Interface list. The plugin uses the `ui` capability. Servers that
enforce an allowlist need to allow `fryguy503/hailstone-ui-pack-native`.
The Lua renderer handles windows and frames whenever their native path is
unavailable.

This preview accelerates supported artwork-backed frames and the idle
Extended Target window body. It uses an existing Hailstone plugin interface.
Use a compatible Windows x64 client with the modular UI and bundled-art APIs
and Dear ImGui 1.91.9, built with `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`. The
installer's minimum version is a lower bound. Revalidate the DLL whenever
the client changes Dear ImGui version or compile settings.

## Build

Use Visual Studio with the x64 C++ build tools. Point the build script at the
client SDK checkout and the same Dear ImGui source as the client.

```powershell
$env:HAILSTONE_SDK_CLIENT = 'C:\src\hailstone-client'
$env:HAILSTONE_IMGUI_SOURCE = 'C:\src\imgui'
.\Build-Native.cmd test
```

The output is `build/ui-pack-native.dll`. The build uses the public
`sdk/hailstone_plugin.h` and the client-pinned Dear ImGui version.
See [Dear ImGui's license](Dear-ImGui-LICENSE.txt).

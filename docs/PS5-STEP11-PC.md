# Step 11 on your PC: the files the PS5 build needs

The PS5 title is built in the cloud, but three things in it come from your own copy of
the game, and a PC makes them: the recompiled game code, the game's recompiled shaders,
and the port's packages. This page builds Edge of Time on a Windows PC the way this
repository's CI does, checks that it plays there, and collects what the PS5 build takes.

It uses the same ReXGlue as the PS5 runtime: `nightly-20261002-bd833a2a`. Use that one,
not another release. The PS5 runtime only runs the code that version generates.

Run every command in a **Developer PowerShell for VS 2022**, from the folder the
repository is cloned into.

## 1. Tools (once)

- Visual Studio 2022 with **Desktop development with C++** and **C++ Clang tools for
  Windows**. CMake and Ninja come with it.
- Git, and Python 3 on the PATH.

## 2. The code and the SDK

```powershell
git clone --recursive -b claude/nifty-knuth-s27y9o https://github.com/KongaTime/EdgeOfTimeRecomp.git
cd EdgeOfTimeRecomp
```

Download `rexglue-sdk-0.10.0.24-dev.gbd833a2-win-amd64.zip` from ReXGlue's
[`nightly-20261002-bd833a2a`](https://github.com/rexglue/rexglue-sdk/releases/tag/nightly-20261002-bd833a2a)
release, and unzip it to `C:\rexglue-sdk`. It holds a `win-amd64` folder.

## 3. The game files

Put your game folder in `assets\`, with the **title update applied**: `assets\Default.xex`,
`assets\Data\GameLogic.dll`, and the rest of the game's files beside them. A disc copy
without the update does not match the project's config files.

## 4. Configure, and build the pack tool

```powershell
cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=C:/rexglue-sdk/win-amd64
cmake --build --preset win-amd64-release --target pkztool
```

The first command warns that there are no shaders yet. That is expected.

## 5. Unpack the game's packs

The shader recompiler reads the game's packs unpacked. This writes each `.pkz` in
`assets\` as a `.pak` into `shaders\` (several GB), and copies Main and Common into
`paks\` for the port's packages:

```powershell
$pkztool = (Get-ChildItem -Recurse out\build\win-amd64-release -Filter pkztool.exe | Select-Object -First 1).FullName
$root = (Resolve-Path assets).Path
New-Item -ItemType Directory -Force shaders, paks | Out-Null
Get-ChildItem -Recurse assets -Filter *.pkz | ForEach-Object {
    $name = ($_.FullName.Substring($root.Length + 1) -replace '[\\/]', '_') -replace '\.pkz$', '.pak'
    & $pkztool unpack $_.FullName "shaders\$name"
}
Get-ChildItem shaders\* -Include *Main*.pak, *Common*.pak | Select-Object Name
```

Copy the game's Main and Common packs from `shaders\` into `paks\` as `paks\Main.pak`
and `paks\Common.pak`. If your game names them differently, or `pkztool` fails on a
pack, stop and send me the error and the file names.

## 6. Shaders, game code, and the build

```powershell
cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH=C:/rexglue-sdk/win-amd64 "-DREEOT_SHADER_INPUT_DIR=$PWD/shaders" "-DREEOT_REFERENCE_PAKS=$PWD/paks"
cmake --build --preset win-amd64-release --target reeot_shader_cache --parallel
cmake --build --preset win-amd64-release --target reeot_codegen
cmake --preset win-amd64-release
cmake --build --preset win-amd64-release --parallel
```

The shader step takes a long time (hours), since it scans every unpacked pack. The
order matters: the configure after `reeot_codegen` is what picks up the recompiled code.

## 7. Check it plays on the PC

```powershell
out\build\win-amd64-release\EdgeOfTimeRecomp.exe --game_data_root="$PWD\assets"
```

Get past the title screen into the first level. If it does not play on the PC, it will
not on the PS5; send me the newest file in `out\build\win-amd64-release\logs\`.

## 8. Send me the build's game files

```powershell
Compress-Archive -Path generated\default, generated\gamelogic, generated\shader_cache.cpp, out\build\win-amd64-release\pkz -DestinationPath reeot-for-ps5.zip
```

Send `reeot-for-ps5.zip`. It is made from your copy of the game, so keep it private:
send it here, and don't post it publicly. If it is too big to attach, tell me its size.

## 9. The game folder on the PS5

FTP `assets\` to `/data/homebrew/PPSA99300/gamedata/`, so that
`/data/homebrew/PPSA99300/gamedata/Default.xex` exists. Only the game's own files go
there. The `shaders\` and `paks\` folders stay on the PC.

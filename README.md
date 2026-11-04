# sh2dc_gamepad_fix

A `dinput8.dll` proxy that lets **Silent Hill 2: Director's Cut/Enhanced Edition** 
use any controller that [SDL3](https://libsdl.org) supports, by presenting it to the game as a 
single virtual DirectInput 8 joystick.

Drop it next to the game and it intercepts DirectInput. The game sees one clean, predictable
gamepad instead of whatever mix of physical DirectInput devices Windows reports.

## Features

- Exposes the first (or a chosen) SDL3 gamepad as a virtual DirectInput8 joystick
- Optionally hides the real DirectInput controllers so the game only sees the SDL pad
- Hot-plugging: the virtual pad can stay present even with no controller connected
- Fully remappable buttons, axes, D-pad (buttons and/or POV hat) and triggers
- Deadzones (radial or per-axis), outer limit, stick swap and per-axis inversion
- Rumble through DirectInput force feedback, with strength and per-motor scaling
- `sh2input.ini` is **hot-reloaded** while the game is running
- Optional `gamecontrollerdb.txt` for controllers SDL3 doesn't know yet
- Optional log file for troubleshooting

## Installation

1. Build the project (see [Building](#building)) or take a prebuilt copy.
2. Copy these files next to `sh2pc.exe`:

   | File | Notes |
   | --- | --- |
   | `dinput8.dll` | The proxy |
   | `SDL3.dll` | **32-bit** build. Not needed if SDL3 was linked statically |
   | `sh2input.ini` | Settings. Created with defaults on first run if missing |
   | `gamecontrollerdb.txt` | Optional, see [Controller database](#controller-database) |

3. Start the game. To uninstall, delete the files above.

The game is 32-bit, so every DLL must be 32-bit as well.

## Configuration

All settings live in `sh2input.ini` next to the DLL. Changes are picked up within about a
second while the game is running. Comment lines start with `;`. **Do not put comments after a
value.**

### `[General]`

| Key | Description |
| --- | --- |
| `Logging` | `1` writes `sh2input.log` next to the DLL |
| `HideRealPads` | `1` hides physical DirectInput controllers so the game only sees the SDL pad |
| `AlwaysPresent` | `1` reports the virtual pad even when no controller is plugged in |
| `GamepadIndex` | Which SDL gamepad to use if several are connected (`0` = first) |
| `MappingsFile` | SDL mapping database to load. Relative to the DLL. Read once at startup |

### `[Buttons]`

Each value is the DirectInput button number the game sees (`0`-`31`), or `-1` to disable it.
Several inputs may share a number. Available sources: `A`, `B`, `X`, `Y`, `LeftShoulder`,
`RightShoulder`, `Back`, `Start`, `LeftStick`, `RightStick`, `DPadUp`, `DPadDown`, `DPadLeft`,
`DPadRight`, `Guide`, `Misc1`, `Touchpad`, `LeftPaddle1`, `RightPaddle1`, `LeftPaddle2`,
`RightPaddle2`.

`LeftTriggerButton` and `RightTriggerButton` make the analog triggers act as buttons once
pulled past `TriggerThreshold`.

### `[DPad]`

`UsePOV=1` also reports the D-pad as a POV hat. The D-pad buttons above still apply.

### `[Sticks]`

| Key | Description |
| --- | --- |
| `LeftDeadzone`, `RightDeadzone`, `TriggerDeadzone` | `0.0`-`1.0` of full travel |
| `TriggerThreshold` | How far a trigger must be pulled (`0`-`1`) to count as a button |
| `OuterLimit` | Travel at which output reaches maximum. Lower reaches the maximum sooner |
| `RadialDeadzone` | `1` circular deadzone, `0` independent per-axis |
| `HonorGameDeadzone` | Also apply the deadzone the game requests (usually leave `0`) |
| `SwapSticks` | Swap left and right sticks |
| `InvertLeftX`, `InvertLeftY`, `InvertRightX`, `InvertRightY` | Invert an axis |

### `[Axes]`

Choose which DirectInput axis each input is sent to: `X`, `Y`, `Z`, `Rx`, `Ry`, `Rz` or
`None`. Axes that nothing is assigned to stay centred.

| Key | Description |
| --- | --- |
| `LeftStickX`, `LeftStickY`, `RightStickX`, `RightStickY` | Stick axes |
| `CombinedTriggers` | `1` one shared trigger axis (LT positive, RT negative, like Xbox DirectInput). `0` separate axes |
| `TriggerAxis` | Axis used when triggers are combined |
| `LeftTriggerAxis`, `RightTriggerAxis` | Axes used when triggers are separate |

Example: a generic-pad layout with the right stick on Z/Rz and triggers as buttons only:

```ini
[Axes]
LeftStickX=X
LeftStickY=Y
RightStickX=Z
RightStickY=Rz
CombinedTriggers=0
TriggerAxis=None
LeftTriggerAxis=None
RightTriggerAxis=None
```

### `[Rumble]`

| Key | Description |
| --- | --- |
| `Enabled` | `1` enables force feedback |
| `Strength` | Overall multiplier |
| `LowFrequencyScale`, `HighFrequencyScale` | Multipliers for the heavy and light motors. `0` disables a motor |

### Controller database

If SDL3 doesn't recognise your controller as a gamepad, download `gamecontrollerdb.txt` from
[mdqinc/SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) and place it next
to the DLL. It is loaded once at startup, so restart the game after changing it. Set
`MappingsFile` to use another name or an absolute path, or leave it empty to disable loading.

Use [SDL Controller Test](https://libsdl.org/tmp/testcontroller.zip) to create a custom controller mapping.

## Building

Requirements: CMake 3.20 or newer, a C++17 compiler, and SDL3 (3.2 or newer) built for
**32-bit**.

### Visual Studio (recommended)

With an installed or prebuilt SDL3 package:

```
cmake -S . -B build -A Win32 -DSDL3_DIR=<path-to-SDL3>/cmake
cmake --build build --config Release
```

With the SDL3 source tree, built together with this project. Point `SH2_SDL3_SOURCE_DIR` at the
folder containing SDL's top-level `CMakeLists.txt`:

```
cmake -S . -B build -A Win32 -DSH2_SDL3_SOURCE_DIR=<path-to-SDL3-source>
cmake --build build --config Release
```

Output is in `build/Release`: `dinput8.dll`, `SDL3.dll` and `sh2input.ini`.

### Static SDL3

Add `-DSH2_STATIC_SDL3=ON` to link SDL3 into `dinput8.dll`, so no `SDL3.dll` is needed. With a
prebuilt package this needs a static SDL3 build exposing `SDL3::SDL3-static`. With
`SH2_SDL3_SOURCE_DIR` it just works. Delete the `build` folder when switching between static
and dynamic.

### CMake options

| Option | Description |
| --- | --- |
| `SDL3_DIR` | Folder containing `SDL3Config.cmake` (prebuilt/installed SDL3) |
| `SH2_SDL3_SOURCE_DIR` | SDL3 source tree to build alongside this project |
| `SH2_STATIC_SDL3` | `ON` links SDL3 statically. Default `OFF` |

The MSVC runtime is linked statically (`/MT`), so no Visual C++ Redistributable is required.

### MinGW-w64 (experimental)

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-i686.cmake \
      -DSDL3_DIR=<path-to>/i686-w64-mingw32/lib/cmake/SDL3
cmake --build build
```

This cross-compiles a 32-bit Windows DLL from Linux or macOS. Output is in `build/`. This path
has not been tested yet.

## Compatibility

- **Windows 7 and newer:** SDL3 supports older desktop Windows versions and the runtime is
  linked statically. Use the 32-bit `SDL3.dll` from an official release or build it yourself.
- **Linux / Steam Deck / macOS via Wine or Proton (experimental):** use the normal Windows
  build and force Wine to load the proxy. In Steam, set the launch option:

  ```
  WINEDLLOVERRIDES="dinput8=n,b" %command%
  ```

## Troubleshooting

- **Nothing changes in game:** set `Logging=1`. If `sh2input.log` doesn't appear next to the
  DLL, the proxy isn't being loaded. Check that `dinput8.dll` is next to `sh2pc.exe` and that
  it is a 32-bit build.
- **Controller not detected:** add a `gamecontrollerdb.txt`, and check `GamepadIndex`.
- **Both a real and a virtual pad appear:** make sure `HideRealPads=1`.
- **Missing `VCRUNTIME140.dll` or `api-ms-win-crt-*.dll`:** rebuild with the current
  `CMakeLists.txt`, which links the runtime statically.
- **Missing `SDL3.dll`:** copy the 32-bit `SDL3.dll` next to the game, or build with
  `-DSH2_STATIC_SDL3=ON`.
- **CMake can't find SDL3:** `SDL3_DIR` must be the folder containing `SDL3Config.cmake`.
  A full source tree has none, so use `SH2_SDL3_SOURCE_DIR` instead.

## How it works

`dinput8.dll` exports `DirectInput8Create` and forwards it to the real system `dinput8.dll`,
wrapping the returned `IDirectInput8A`. The wrapper adds a virtual joystick device to device
enumeration, optionally filters out real game controllers, and implements the DirectInput
device interface on top of SDL3's gamepad API, including force feedback effects mapped to SDL
rumble.

## Project layout

| Path | Purpose |
| --- | --- |
| `dllmain.cpp` | The proxy, virtual device and configuration handling |
| `sh2input.ini` | Default settings, copied next to the built DLL |
| `CMakeLists.txt` | Build configuration (CMake target `sh2_dinput8`, output name `dinput8`) |
| `cmake/toolchain-mingw-i686.cmake` | Toolchain file for MinGW-w64 cross builds |


# FFX-60fps

A 60-240 fps mod for **Final Fantasy X HD Remaster** (Steam, Windows).

The game's logic still runs at its native 30 Hz, so gameplay, timing and battles behave as in the original. The mod
draws extra frames between logic ticks - one at 60 fps, three at 120, five at 180 - so the camera, characters and
animations move at the full frame rate. It also fixes the effects, menus and inputs that would otherwise run at
several times speed or lose button presses.

## Install

1. Download the release zip.
2. Copy `dbghelp.dll`, `fps60_ffx.dll` and `fps60.ini` into the game folder, next to `FFX.exe`.
3. Start the game from Steam.

To uninstall, delete `fps60_ffx.dll` (FFX then runs stock), or also `dbghelp.dll` if no other game mod uses it.

- **`dbghelp.dll` is a small shared loader.** It loads `fps60_ffx.dll` in FFX and `fps60_ffx2.dll` in FFX-2, when
  that file is present; otherwise the game runs stock. FFX-2 is not affected unless its own mod DLL is installed.
- **Other `dbghelp.dll` mods:** they can't be installed at the same time, since they use the same file name.

## Settings

`fps60.ini` is read when the game starts. The release copy lists only the settings players may want to change;
everything else uses its built-in default. The full list of switches (one per fix under `[rows]`, plus test and
diagnostic switches) is the development copy, `fps60.ini` in the source tree. `ship/fps60.ini` is the release copy;
`tools/pack_dist.py` (outer repo) packs it and checks that every value in it equals the built-in default.

| Key | Action |
|---|---|
| F7 | Turn smoothing of the camera and characters on or off |
| F6 | With `SMOOTH-001-TRACE=<n>`: log the next `n` camera passes (diagnostic, off by default) |
| Numpad 0 (hold) | Fast-forward the game (speed set by `DEV-TURBO-SPEED`; turn off with `DEV-TURBO=0`) |

`fps60.log` in the game folder lists what the mod found and turned on.

### Frame rate

`fps=<n>` under `[fps60]`, where `n` is any multiple of 30 from 60 to 240 - so 60, 90, 120, 150, 180, 210 or
240. The in-between rates are worth having: a PC that draws 200 fps holds 180 comfortably but cannot hold 240,
and would otherwise have to fall all the way to 120.

The **Frame Rate** row on the game's Config page sets the same thing without a restart, and writes it back to
the ini when you leave the page. A Config row has room for four values, so it offers the rate you configured
plus the next most useful ones, with 30 standing for the game's original pacing.

Everything above 60 needs two things:

- **V-Sync off** in the game's own Config page, and the monitor set to that refresh rate. The game presents
  through its swap chain, so with V-Sync on it cannot draw faster than the monitor refreshes however short the
  frame period is.
- **A PC that can hold the rate.** Game speed is tied to how many frames the mod draws per logic tick, so a PC
  that draws 150 fps while the mod paces for 240 would run the game slow. `PERF-002` watches the frame times and
  steps the rate down one 30 fps notch at a time (240 -> 210 -> 180 ... -> 60 -> original 30) until it finds one
  the machine holds, retrying the rate you asked for later with a doubling back-off. Every step is logged, so
  `fps=240` on a machine that tops out around 200 simply settles at 180 on its own.

How it works: the game's own frame limiter keeps a schedule whose period is `frame_divider x 1/59.94 s`. The mod
holds the divider at 1 and `RATE-001` rewrites that one constant to `1/(N x 29.97)`, so the host loop runs N
times per logic tick. `pacing.cpp` then makes one invocation in N a logic tick and the other N-1 render-only, and
each pass draws the world `(pass + 1) / N` of the way between the last two ticks. N is just a count of passes,
which is why it does not have to be a power of two.

## Known issues

- Cloth (sleeves, skirts) and ship sails swing faster than in the original.
- Pre-rendered movies play at their original 30 fps.
- The limiter sleeps in whole milliseconds, so frame pacing gets coarser the shorter the period is (4.17 ms at
  240 fps). `PERF-001` asks Windows for a 1 ms timer, which is what makes those rates usable at all.
- Limb animation above 60 fps samples the motion keyframes between frames more finely than the game ever does.
  `SMOOTH-003-SUBSTEP=0` keeps limbs on the proven 60 Hz pair while the camera and actor roots still follow the
  full frame rate, if a scene looks wrong at 120 or 240.
- `DEV-TURBO`'s factor rounds down above 60 fps (x10 reaches about x8 at 240): the game turns the frame delta
  into a whole number of catch-up ticks, and the delta is shorter at higher rates.

## Build

Visual Studio 2026 (toolset v145), Win32:

```
MSBuild loader.vcxproj /p:Configuration=Release /p:Platform=Win32
MSBuild fps60.vcxproj /p:Configuration=Release /p:Platform=Win32
```

The outputs are `artifacts/loader/Release/dbghelp.dll` (the shared loader) and `artifacts/Release/fps60_ffx.dll`.
`x2.vcxproj` builds `fps60_ffx2.dll`, the FFX-2 mod (work in progress).

Sources: `src/loader/` (shared loader), `src/x/` (FFX mod), `src/x2/` (FFX-2 mod). The two mods share no code.

## License

Apache-2.0 (`LICENSE`). MinHook (`third_party/minhook`) is BSD-2-Clause.

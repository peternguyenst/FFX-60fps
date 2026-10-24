# FFX-2 60fps mod: words used in the code

Each row's header explains the row; these are the terms they share.

## Frames

- **Invocation**: one run of the game's main loop (`Sg_MainLoop`). Stock FFX-2 runs it 30 times a second; the mod
  runs it 60 times.
- **Logic frame / logic invocation**: an invocation that runs the game (scripts, battle, animation) and draws it. The
  game still gets 30 of these a second, so it plays at its normal speed.
- **Render-only frame / invocation** (`INV_RENDER_ONLY`): the extra invocation between two logic ones. It draws the
  game again without running it. The game's own "exec" flag is 0 on these.
- **Backlog invocation** (`INV_BACKLOG`): the game is behind and catches up with logic and no draw.
- **Stock** (`INV_STOCK`): the mod's pacing is off right now (FMV, menu, load) and the game runs as shipped.
- **Tick**: one step of game logic (1/30 s).
- **Divider**: the game's frame divider. 2 = one invocation every second screen refresh (30 Hz), 1 = every refresh. The
  mod keeps it at 1.
- **Pacing** (`x2_pacing.cpp`): picks the kind of each invocation and sets the exec flag.

## Fixes

- **Row**: one fix, in `rows/X2-<AREA>-nnn.cpp`, with its own `[rows]` switch in `fps60x2.ini` and a line in the log.
  The ID matches the evidence ledger in the docs repo.
- **Hold**: make something not advance on a render-only frame (e.g. a WAIT counter, a particle clock).
- **Held walk** (X2-MAG-025): a walk of the battle-effect scripts that draws but does not advance.
- **Pass** (X2-MAG-008): let a spell DLL's update run on render-only frames too, because it draws parts of its
  effect only right after its update. "Passed DLL", "pass list", "chain" are explained in X2-MAG-008.
- **Blink**: something drawn on logic frames but not on render-only ones (or the other way round): it flickers at 30 Hz.
- **2x**: something that runs once per invocation instead of once per tick, so at double speed.

## Game parts

- **Spell DLL / magic DLL**: `magicFiles/FFX-2/magic_NNNN.dll`, one per effect; the game loads them during battle.
  `re/MAGIC-SPELLS.tsv` in the docs repo says which spell each one is.
- **Template DLL**: one of the 310 spell DLLs built from the same code (same draw function).
- **Unit, stream, WAIT** (X2-MAG-025): a battle effect is a set of units, each running a small bytecode program in up
  to 4 streams; a stream sleeps for WAIT ticks between steps.
- **Particle program** (X2-MAG-006): a particle effect; the game steps it every time it is drawn.
- **Actor** (X2-SMOOTH-*): a character, monster or prop in the actor array; "slot" is its index.
- **Motion**: an actor's animation; its clock is in 1/256 frames.

## Addresses and evidence

- **gva**: an address as Ghidra shows it (FFX-2.exe loaded at 0x400000). The running game is relocated; the code finds
  every address by signature (`x2_sigs.cpp`, `sig_find`) and checks the bytes before patching.
- **S / D / U / H** (in the ledgers): static proof from the code, dynamic proof from a log, user test, hypothesis.

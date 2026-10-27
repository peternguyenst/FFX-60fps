-- Jobs mod entry script. Runs when the game starts and again every time a file in jobs/scripts is saved.
-- Scaffold test: logs the party's levels and stats to jobs.log, so editing and saving this file shows the reload.

local NAMES = { [0] = "Tidus", "Yuna", "Auron", "Kimahri", "Wakka", "Lulu", "Rikku" }

for i = 0, 6 do
  local p = jobs.ply(i)
  if p then
    local b = p.base
    jobs.log(string.format("%-7s lv %2d cap 0x%02X ap %d  HP %d/%d MP %d/%d  base str %d def %d mag %d mdef %d agi %d luck %d eva %d acc %d",
      NAMES[i], p.level, p.cap_byte, p.ap, p.hp, p.max_hp, p.mp, p.max_mp,
      b.str, b.def, b.mag, b.mdef, b.agi, b.luck, b.eva, b.acc))
  else
    jobs.log(NAMES[i], "unreadable")
  end
end

-- CRT screen
-- =========================================================================
-- This is the example mod. It is a real feature, not a toy: it is what draws
-- the scanlines, the aperture mask, the curved tube and the vignette.
--
-- It is here, in mods/, rather than built into the game on purpose. Everything
-- about how mods work is visible from this one folder:
--
-- WHERE THEY GO    a folder under mods/ with a mod.lua in it. That is the
-- whole rule. A folder without a mod.lua is not a mod and
-- is skipped silently.
--
-- HOW THEY LOAD    every mod found is loaded at startup. No rebuild, no
-- registration list, nothing to edit anywhere else.
--
-- TURNING THEM OFF the launcher's Mods page lists what it found and gives
-- each one a checkbox. Untick this one and the scanlines
-- go away -- which is the point of shipping a visible mod
-- as the example rather than one that only prints a line.
-- From a shell it is  mods_off = crt_screen  in
-- goldeneye.cfg, or GETV_MODS_OFF=crt_screen.
--
-- Copy this folder, rename it, and you have a working mod.
-- =========================================================================


-- ---- tuning -------------------------------------------------------------
-- Edit these and restart. Every one is 0 to 1 except LINES.

local SCANLINE = 0.28 -- depth of the dark line between scanlines
local MASK     = 0.18 -- aperture grille: how far each column leans to one
 -- phosphor. Subtle on purpose; at full strength it
 -- eats a third of the brightness and reads as a
 -- colour bug rather than as a mask.
local CURVE    = 0.025 -- barrel distortion. 0.06 pulls the corners in far
 -- enough that the black surround is the first thing
 -- the eye lands on, which is a bezel, not a tube.
local VIGNETTE = 0.22 -- darkening toward the corners
local LINES    = 240 -- how many scanlines the "tube" draws, independent of
 -- the window size. 240 is the console's own count.
 -- Not one line per output pixel: a two-pixel period
 -- lands on fragment centres where every pixel gets
 -- the same darkening, so the whole image just dims.


-- ---- apply --------------------------------------------------------------
-- ge.postfx() sets the post-process pass. Fields left out keep their current
-- value, so a mod can change one number without restating the rest. The call
-- returns the values that were actually applied, after clamping.

local fx = ge.postfx {
    crt      = true,
    scanline = SCANLINE,
    mask     = MASK,
    curve    = CURVE,
    vignette = VIGNETTE,
    lines    = LINES,
}

ge.log(string.format(
    "crt_screen: on -- scanline %.2f  mask %.2f  curve %.3f  vignette %.2f  %d lines",
    fx.scanline, fx.mask, fx.curve, fx.vignette, fx.lines))


-- ---- what else a mod can do ---------------------------------------------
-- Three optional hooks. Define any of them and the game calls them; leave
-- them out and nothing is lost. This mod needs none of them -- the settings
-- above are applied once at load and that is the whole feature -- so they are
-- left commented rather than defined empty.
--
-- function onFrame(frame)        end -- once per rendered frame
-- function onPlayerSpawn(player) end
-- function onWeaponFire(weapon)  end
--
-- ge.postfx can be called from onFrame() too, so an effect can change while
-- the game runs. The parameters are read every frame, not latched at startup.
--
-- The rest of the API is in wiki/Lua-mods.md:
-- ge.log(text)          ge.stage()
-- ge.player_count()     ge.player_pos(i)

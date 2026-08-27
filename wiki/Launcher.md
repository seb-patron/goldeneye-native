# The launcher

A window that opens before the game does, so picking a level or turning on a cheat does not
mean editing a config file and restarting.

It is built on Dear ImGui and is **optional at every level**. If ImGui is not fetched, the
build omits it and the entry points compile away to nothing. The game still runs; you just
configure it through `goldeneye.cfg` and the environment instead.

## The pages

**Mission.** Level select across every loadable stage, with the multiplayer-only ones marked
as such rather than offered as though they would boot on their own.

**Rules.** Enemy health, damage, accuracy and ammunition, player health, explosion strength.
Percentages with presets, plus horde mode. See [Rulesets](Rulesets).

**Controls.** Rare's eight control styles, bindings, mouse sensitivity and invert, deadzone.

**Cheats.** The game's own cheat flags by name. See [Cheats](Cheats).

**Video.** Resolution, fullscreen, supersampling, texture filtering, mipmapping, widescreen,
field of view, and a HUD section with a colour picker for the crosshair. The picker is the
fastest way to see how a given colour reads against the sight texture, which is a question
better answered by looking than by reasoning about the asset.

**Mods.** Scans the mod directory and lists what it found. See [Lua mods](Lua-mods).

**Netplay.** Off, host or join, with port and player count under Host and an address under
Join. **The session does not start yet.** The transport is written and the page sets the right
variables, but nothing in the game loop calls into it. See
[Multiplayer](Multiplayer#network-play-written-not-connected).

## How it hands over

The launcher does not have a private settings format. Every page writes the same `GETV_*`
variables the game already reads, and the game is then launched with those set. So anything you
can do in the launcher you can also do from a shell or a config file, and the launcher cannot
drift into supporting a setting the game does not have.

The netplay page is a small illustration of why that matters. The engine checks the host
variable before the join one, so a launcher writing both independently could leave someone
hosting when they meant to join. The page holds the choice as one three-way value instead of two
checkboxes that could both end up set, and derives which mode to show from the two variables on
load, so a session started by hand outside the launcher opens on the page that matches it.

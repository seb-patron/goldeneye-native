# Crouch, and the case for jump and lean

## Crouch: done

Retail crouch is gated behind aim mode. `bondview2.c:5484` requires `insightaimmode` **and**
stick-down before Bond will lower, so crouching means holding aim, pushing down, then
releasing aim while staying low. Faithful, and genuinely unintuitive.

**C** or **LSHIFT** now crouches, **V** stands up. The keys are ORed alongside the retail
condition rather than replacing it, so the original gesture still works exactly as it did and
nothing that depended on it changes. `GETV_CROUCH_KEY=0` for faithful-only.

All four sites were patched, not one: the condition appears twice per control-style branch and
there are two branches. Patching the first pair and stopping would have left crouch working in
some styles and not others, which reads as an intermittent bug.

Measured with `GETV_CROUCH_SELFTEST=1`: eye height 339.719 → 239.719, a drop of exactly
100.0, which is `FULL_CROUCH_OFFSET`. The weapon flag `WEAPONSTATBITFLAG_DISABLE_CROUCH` is
still honoured, so weapons that forbid crouching still forbid it.

⚠️ These hooks deliberately do **not** go through `port_os.c`'s action table. That table is the
Surface's lane for the per-player binding work; a second author adding rows to it mid-flight is
how the last collision happened. When the binding work lands, these become `GE_ACT_CROUCH` and
`GE_ACT_STAND` and pick up gamepad support for free.

## Jump: expensive, and probably not wanted

**GoldenEye has no jump.** Not a missing binding -- there is no jump animation, no vertical
player velocity, and no player-initiated Y movement anywhere in the movement code. Bond's
height comes from the stan tile he is standing on plus the crouch offset.

Adding one means giving the player a vertical velocity integrator, landing detection against
stan tiles, and an answer for what happens when he lands somewhere with no tile -- and the
whole AI and collision model assumes a player at floor height. The stan system is walkable
*surfaces*, not volumes, so "in the air above tile N" is not a state the engine has.

Verdict: a real project, not a binding. Worth doing only if the goal is a movement mod rather
than a definitive GoldenEye, and it should be a Lua-gated mod if it happens.

## Lean: the interesting one

Also absent from retail, but far cheaper than jump, because leaning is a **camera and aim
offset** rather than a change of position. The camera basis is already interception-friendly:
the interpolation work in `docs/FRAME_TIMING.md` proved that `bondviewUpdateCameraMatrices`
takes position, direction and up as parameters and only reads them, so a lateral offset can be
applied at exactly the same seam.

What makes it real work rather than a one-liner:

- **Collision.** A lean that pushes the camera through a wall is worse than no lean. Needs a
  short raycast against the room geometry and a clamp on how far the offset can go.
- **Where the shots come from.** If the camera leans and the gun does not, the player aims
  around a corner and their bullets still hit it. The gun position and the aim ray have to
  move with the view, which means touching `gunfire.c`, not just the camera.
- **What the AI sees.** 🔴 GE's AI branches on render visibility (`IFImOnScreen`,
  `IFMyRoomIsOnScreen`), so a leaning player changes what guards react to. This is the same
  second-order path the interpolation work hit, and it is the part most likely to produce
  "the AI is behaving oddly" reports with no obvious cause.

Verdict: worth doing, as a gated GoldenEye+ feature, after the player API lands -- the aim-ray
work overlaps with what an external AI needs to read anyway.

## Order

1. ✅ Crouch key
2. Lean, once the player API exists and the aim ray is already being handled
3. Jump, only if the project decides it wants a movement mod

---

# Guard weapons as a cheat: scoped, not built

Wanted for horde mode, so a level can be made incrementally harder by upgrading what the
guards carry. Also the best stress test available -- a room of RC-P90s puts far more beams,
sounds, impacts and animation through a frame than any authored encounter.

## What exists today

`GETV_CHR_FIRERATE=<item>` gives every guard another weapon's **firing cadence**. It redirects
the four `bondwalkItemGetAutomaticFiringRate()` reads in `chraction.c`'s firing block and
nothing else.

🔴 **It does not change the weapon a guard carries.** Guards shoot at the chosen rhythm while
still holding, animating and sounding their own gun. It was briefly named `GETV_CHR_GIVE`,
which implied a swap it does not do.

It is still the right tool for timing work, and it is what made guard fire the dominant term
in the reaction-stepping measurement -- with issued weapons a scripted run sits ~1400 frames
before anything shoots back.

## What a real swap needs

`chr->weapons_held[hand]` is a pointer to a weapon **object** spawned from the setup file.
`propobj.c:12111` only *attaches* one that already exists, binding its model to the guard's
hand via `Switches[3]` / `Switches[5]`. So a swap means:

- spawning a different weapon object for the guard at setup time, or replacing the existing
  one and freeing the old (`objFreePermanently`, as `chr.c:2707` does on death)
- the new model must load and attach to the same hand switches
- damage, sound and the AI's own branches on `ITEM_LASER` / `ITEM_ROCKETLAUNCH` /
  `ITEM_GRENADELAUNCH` follow the item, so those paths need checking rather than assuming

⚠️ **Giving guards an explosive is not a cadence change.** Those three items take different
branches in the firing block, so a "hard mode" that hands out rocket launchers exercises code
the cadence override never touches.

## Order

1. ✅ Firing cadence override, which is what timing work needed
2. Real weapon swap at spawn, which is the horde-difficulty cheat
3. Per-wave scaling on top of it, so horde can escalate

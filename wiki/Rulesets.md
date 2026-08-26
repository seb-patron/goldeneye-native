# Rulesets and horde mode

A ruleset scales values the game already reads at load. No level, model, asset or line of
geometry is touched, and that is why these are cheap and combine freely.

## Presets

`ruleset = classic | hardcore | survival | chaos | horde`

| preset | what it does |
|---|---|
| `classic` | the game as shipped, and completely silent |
| `hardcore` | enemy health 200%, damage 150%, accuracy 130%; player health 50%; ammo 50% |
| `survival` | 150/125/115, player 75%, ammo 75%, endless waves |
| `chaos` | enemies 300/200/150, player 200%, ammo 300% |
| `horde` | stock difficulty, double ammo, endless waves |

## Individual keys

Percentages, 100 is unmodified. These override whatever the preset chose, so
`ruleset = hardcore` plus `ammo = 200` is hardcore with generous ammo.

`enemy_health` `enemy_damage` `enemy_accuracy` `enemy_reaction`
`player_health` `player_armour` `ammo` `explosion_damage` `turret_damage`

## Horde mode

`horde = 1`. When a guard dies, replacements spawn where it fell using the engine's own
spawn call, inheriting the dead guard's body and AI list. The wave number rises every
`GETV_HORDE_WAVE_KILLS` kills and adds `GETV_HORDE_GROWTH` to the spawn count, up to a cap.

Tuning: `GETV_HORDE_PER_KILL` (1), `GETV_HORDE_PER_KILL_CAP` (3), `GETV_HORDE_MAX_ALIVE` (12),
`GETV_HORDE_WAVE_KILLS` (10), `GETV_HORDE_GROWTH` (1).

A spawn can be refused and that is not an error. The engine declines with fewer than three
free guard slots, and the slot table is only as large as the level needs plus ten, so the real
ceiling belongs to the level. A refused spawn leaves the wave smaller.

## Verifying a ruleset took effect

Any non-stock ruleset prints twice at level load: what was asked for, and what the engine ended
up holding.

```
[getv][ruleset] "hardcore" -- tougher guards, less ammo, half the player health
[getv][ruleset] enemy: health 200% damage 150% accuracy 130% reaction 100%
[getv][ruleset] applied: aiHealth=1.000 aiDamage=0.750 aiAccuracy=0.780 ... ammo=1.000
```

The second line is the claim, the `applied:` line is the measurement. On Agent difficulty
stock `aiHealth` is 2.000, so hardcore's 1.000 means guards take half the damage they used to.

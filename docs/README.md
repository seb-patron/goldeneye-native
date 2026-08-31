# Documentation

Start with [`SETUP.md`](SETUP.md). Everything else is here because someone needed it once.

## Getting it running

| | |
|---|---|
| [SETUP.md](SETUP.md) | The build guide, start to finish. Read this first. |
| [CONFIGURATION.md](CONFIGURATION.md) | Every config key, which are implemented, which are reserved. |
| [CHEATS.md](CHEATS.md) | The game's own cheat system, exposed by name. |

## Playing with it

| | |
|---|---|
| [MODDING.md](MODDING.md) | How the tree is arranged and where the seams are. |
| [FRAME_TIMING.md](FRAME_TIMING.md) | Why frame rate is not a simple slider in this game, and what a real fix needs. |
| [STANCE.md](STANCE.md) | Crouch, and the case for jump and lean; guard weapons as a cheat. |

## Where it is going

| | |
|---|---|
| [ROADMAP.md](ROADMAP.md) | Current state, known issues, planned work. |
| [MAINTAINING.md](MAINTAINING.md) | Community workflow and future upstream replay. |
| [VISION.md](VISION.md) | The long arc, scored honestly against what the tree does today. |
| [REUSE_AUDIT.md](REUSE_AUDIT.md) | What to borrow from the N64 port ecosystem, what is already borrowed, and what is licence-quarantined. |
| [PLAYER_API.md](PLAYER_API.md) | Design for the API bots, netplay and external AI all drive: one seam, several consumers. |

## How it works

| | |
|---|---|
| [PORTING.md](PORTING.md) | The platform layer, per file. |
| [ASSET_LOADING.md](ASSET_LOADING.md) | How level assets get from the ROM into the running game. |
| [asset-converter-spec.md](asset-converter-spec.md) | The converter's contract. |
| [PERFECT_DARK.md](PERFECT_DARK.md) | What the MIT-licensed Perfect Dark port offers this one. |
| [WINDOWS_STAN_ORDERING.md](WINDOWS_STAN_ORDERING.md) | The Windows rendering bug: root cause and fix, with before/after measurements. |

## Provenance

| | |
|---|---|
| [LICENSING.md](LICENSING.md) | Where every part came from and which terms are settled. |
| [THIRD_PARTY.md](THIRD_PARTY.md) | The fetched files: what, whence, and why they are not vendored. |

## Research

[`research/`](research/) holds ten documents on the N64 hardware, GoldenEye's own systems and
the wider decomp ecosystem. Every claim in them is tagged VERIFIED, CONTESTED or FOLKLORE, and
each carries an explicit note on what could not be established. They exist so nobody has to
rediscover this the expensive way.

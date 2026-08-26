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
| [MOUSE.md](MOUSE.md) | Mouse look: why it was unusable, and the numbers behind the fix. |
| [STANCE.md](STANCE.md) | Crouch, and the case for jump and lean. |
| [COOP.md](COOP.md) | Splitscreen co-op: what works, and the baseline that was wrong. |
| [NETPLAY.md](NETPLAY.md) | Networked play: design and current state. |
| [FAQ.md](FAQ.md) | Questions that keep coming up. |

## Where it is going

| | |
|---|---|
| [ROADMAP.md](ROADMAP.md) | Current state, known issues, planned work. |
| [VISION.md](VISION.md) | The long arc, scored honestly against what the tree does today. |
| [REUSE_AUDIT.md](REUSE_AUDIT.md) | What to borrow from the N64 port ecosystem, what is already borrowed, and what is licence-quarantined. |

## How it works

| | |
|---|---|
| [PORTING.md](PORTING.md) | The platform layer, per file. |
| [ASSET_LOADING.md](ASSET_LOADING.md) | How level assets get from the ROM into the running game. |
| [asset-converter-spec.md](asset-converter-spec.md) | The converter's contract. |
| [PERFECT_DARK.md](PERFECT_DARK.md) | What the MIT-licensed Perfect Dark port offers this one. |
| [WINDOWS_BRINGUP.md](WINDOWS_BRINGUP.md) | Bringing up Windows: state, suspects, and the traps already paid for. |
| [WINDOWS_STAN_ORDERING.md](WINDOWS_STAN_ORDERING.md) | Why the world did not render on Windows. |
| [COLOUR_BUGS.md](COLOUR_BUGS.md) | Two colour defects, measured. |

## Programmable play

The port exposes seams so the game can be driven, observed and extended from outside it.

| | |
|---|---|
| [PLAYER_API.md](PLAYER_API.md) | Reading player state and posting input, per slot. |
| [ENEMY_API.md](ENEMY_API.md) | What the guards know, and what they believe. |
| [BOTS.md](BOTS.md) | Route-following bots built on those two APIs. |
| [HARNESS.md](HARNESS.md) | Injecting input: which path actually works. |

## Working on the port

| | |
|---|---|
| [DEVELOPMENT.md](DEVELOPMENT.md) | The two-machine workflow, and the rules that keep both trees honest. |
| [TASK_QUEUE.md](TASK_QUEUE.md) | Work queued for the Windows tree. |

## Provenance

| | |
|---|---|
| [LICENSING.md](LICENSING.md) | Where every part came from and which terms are settled. |
| [THIRD_PARTY.md](THIRD_PARTY.md) | The fetched files: what, whence, and why they are not vendored. |

## Research

[`research/`](research/) holds ten documents on the N64 hardware, GoldenEye's own systems and
The wider decomp ecosystem. Every claim in them is tagged verified, contested or folklore, and
each carries an explicit note on what could not be established. They exist so nobody has to
rediscover this the expensive way.

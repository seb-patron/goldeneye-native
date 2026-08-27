# Multiplayer and co-op

Three different things share the word "multiplayer" here, and they are at three different
stages. Being precise about which is which saves disappointment.

## Split screen: works

The game's own multiplayer, running as it always did. Two to four players, split screen, radar
in each pane, all 64 selectable characters, the full scenario and options setup. Widescreen
correction applies per pane, so a two-player horizontal split fills the window rather than
sitting in a 4:3 box.

Six stages are multiplayer-only and carry no solo setup at all: Complex, Temple, Basement,
Stack, Library and Caves. Loading one on its own tells you so and names the flag, rather than
booting an empty room.

## Co-op: bring-up quality

Two to four players share a single-player mission's own geometry, props, objectives and
cutscenes, split screen, with `coop = 2`.

The mission loads, every player spawns into it, and every viewport renders. Dam draws 5139
triangles at two players and 8412 at four, against 2042 solo.

**Fixed, and worth recording because the cause was not obvious.** A campaign mission has exactly
one start pad. The per-player start pad pick is gated on there being more than one, and
multiplayer arenas carry five to eight, so each arena player resolves to a different pad. A
campaign mission carries one, so every co-op player used to resolve to the same pad. Because the
camera record was seeded from that same shared position, player 1's camera showed player 0's
view even after player 1's own position had been correctly offset. That is why it looked like a
camera bug rather than a spawn bug.

What remains is that the mission is authored around one Bond. Objectives, AI and cutscenes all
assume a single player and none of that has been adapted. Call it bring-up: the plumbing is
there, the game design is not.

## Network play: written, not connected

This is the one to be exact about, because there is a launcher page for it and the page implies
more than currently happens.

**Written and in the tree:** the lockstep transport, the peer discovery parser, and the launcher
page that sets the variables. The discovery parser has unit tests.

**Not written:** the call from the game loop into any of it. Nothing invokes the network entry
points, so selecting Host or Join sets environment variables and no session starts.

The design is lockstep rather than state replication. Only inputs travel and every machine
simulates the same thing from them, because a tick of input is twelve bytes and the world state
is not something that can be shipped at 60Hz over a domestic link. It plugs into the same input
seam the [bots](Bots) already use, which is why that seam existing is most of the work.

## The measurement, if you want to check any of this

`tools/playtest.py` drives a stage with scripted input and reads the machine-readable run state
the game emits, including whether the player reached gameplay, how far they moved, how many
objectives the mission has and whether any changed.

One caveat worth carrying: scripted input has never been shown to move a player on its own, so a
comparison like "solo moves 900 units, co-op moves nothing" can be comparing an intro camera
animation against a genuinely stationary player. The player API is the working alternative for
driving input in a measurement.

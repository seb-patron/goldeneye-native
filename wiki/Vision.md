# Vision

Where this could go and what it would cost. [Roadmap](Roadmap) is the short list of what is
being worked on now; this is the long arc behind it. The full version, scored item by item, is
[`docs/VISION.md`](https://github.com/seb-patron/goldeneye-native/blob/main/docs/VISION.md).

## Three projects sharing a source tree

The most useful idea in the design notes is that this repository is really three efforts that
happen to share a tree, and they have different standards of proof.

**Preserve.** Make the original game run correctly on modern hardware. Correctness is judged
against the real N64, using reference captures from real hardware as ground truth. The bar is
"indistinguishable from the cartridge", and the arbiter is a photograph of a CRT, not an
opinion.

**Extend.** Widescreen, mouse and keyboard, high refresh, bots, network play, mods. The bar is
different: these never existed, so there is nothing to be faithful to. What replaces fidelity is
that the default has to stay stock. Every extension is behind a gate that defaults off or to
existing behaviour.

**Understand.** Write down what was learned so nobody pays for it twice. Around 900 KB of
research documents exist for exactly this reason, and every claim in them is tagged as verified,
contested or folklore, with an explicit section on what could not be established.

Keeping the three apart is what stops "it looks better on my monitor" being offered as evidence
about faithfulness.

## How things get scored

Three labels, and nothing is promoted without a measurement that is named alongside it:

- **DONE**, implemented and verified, with the verification stated.
- **PARTIAL**, real code exists and does something, with the gap stated.
- **OPEN**, not started, or a config key that is parsed and inert.

**A config key is not a feature.** `docs/CONFIGURATION.md` has a section for keys that are
parsed and do nothing, and `ssao`, `shadows`, `per_pixel_lighting` and `muzzle_lights` are all
in it. They are namespace rather than code, and they are scored OPEN. A plan that does not
distinguish shipped work from intention stops being useful in about a week.

## The long arc

The honest summary of where the ceiling is: this is a decompilation, so nothing is structurally
out of reach. Anything the original game does is ordinary C in front of you, which is the whole
difference from an emulator. What limits the project is time and verification, not architecture.

The things that would matter most, roughly in order of how much they would change the
experience:

1. **Frame-quantised systems converted to real time.** Turns "do not run above 60" into "run at
   whatever your display does". Two of the three pieces are done.
2. **Network play connected.** The transport and the input seam both exist.
3. **Co-op that accounts for extra players**, rather than merely containing them.
4. **Bots developed toward the Perfect Dark simulant bar**, using opcodes the game already has.
5. **Texture packs**, once the override path has actually been run.

Each is a real amount of work and none of them is blocked on something unknowable.

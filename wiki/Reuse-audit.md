# Reuse audit

The N64 port ecosystem is generous and there is a lot of solved work in it. Some of it can be
borrowed, some of it cannot, and the difference is licence rather than quality. The full audit
is
[`docs/REUSE_AUDIT.md`](https://github.com/seb-patron/goldeneye-native/blob/main/docs/REUSE_AUDIT.md).

## Quarantined: the 1964 and Mouse Injector lineage

`Graslu/1964GEPD` and the wider 1964 lineage are **GPL-2.0**. This project is MIT. No code from
them is used here and none can be.

That matters most around frame timing, because that lineage identified the problem publicly and
was right to. **Credit for identifying a problem is not a licensable thing**, and it is the more
valuable contribution anyway. The analysis in [Frame timing](Frame-timing) is this project's own
work against the decompiled source; the observation that it needed doing came from elsewhere and
is acknowledged.

## Borrowed already

**Fast3D**, from sm64ex, which took it from `Emill/n64-fast3d-engine`. Licence status
unresolved, fetched rather than vendored. See [Provenance](Provenance).

**The audio mixer**, also from sm64ex. It is Emill's software implementation of the N64 audio
microcode, and it drops in almost unmodified because GoldenEye's audio ABI is the same one: a
diff of the command macros in both games' `abi.h` is empty apart from an extra call SM64 uses.
Both drive stock libultra `aspMain`, so the command set, argument order and DMEM semantics
match.

## Worth borrowing, MIT, not yet taken

**The Perfect Dark port** is MIT and it is Rare's sequel in the same engine lineage, which makes
it the most directly relevant prior art there is. Its simulants are the reference for
[bots](Bots).

One caution learned the hard way, and it generalises: Perfect Dark rewrote Fast3D for a custom
twelve-byte vertex, while GoldenEye uses the standard N64 vertex, the same as SM64. So the
renderer had to come from sm64ex rather than from PD despite PD being the closer relative
otherwise. Being in the same lineage does not make a component drop-in.

## The rule that came out of all this

**When inherited port code disagrees with GoldenEye's own source, the game wins.**

Five separate bugs on this port were that one family. A reference implementation written for a
different game is actively misleading even when it is for the same console, because it encodes
assumptions about a different set of display lists. The decompiled source is the authority; the
borrowed code is a starting point.

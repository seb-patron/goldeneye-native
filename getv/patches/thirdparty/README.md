# `getv/patches/thirdparty/` — the port's changes to fetched sm64ex sources

Fifteen files under `getv/port/` are not in this repository. They are inherited from sm64ex,
whose Fast3D lineage carries an unresolved redistribution restriction, so they are fetched at
build time instead of being vendored. `docs/THIRD_PARTY.md` is the full account; this file is
the operational note.

This directory holds the two things the repository does keep:

| file | what it is |
|---|---|
| `MANIFEST` | the fifteen upstream paths and where each one lands |
| `0001-getv-port-layer.patch` | every change this port makes to them, as one diff |

## Get the files

```bash
tools/fetch-thirdparty.sh            # clone at the pin, copy, patch
tools/fetch-thirdparty.sh status     # show the pin and which files are present
tools/fetch-thirdparty.sh verify     # re-derive from pin + patch, compare byte for byte
tools/fetch-thirdparty.sh clean      # remove them again
```

Upstream is `https://github.com/sm64pc/sm64ex` pinned at
`d7ca2c04364a6dd0dac58b47151e04e26887e6f0`.

If the machine is offline or you want to use a mirror, point `GETV_SM64EX_REPO` at any clone
that contains that commit and no network access is attempted.

## After editing any of the fifteen files

> **The patch is the only place your change is recorded.** The files themselves are gitignored.
> Editing one and not regenerating loses the work on the next `clean` or fresh checkout.

```bash
tools/fetch-thirdparty.sh regen
```

`regen` rewrites `0001-getv-port-layer.patch` from the current working tree and then runs
`verify` on the result, so a successful run is also proof the patch is complete.

This is the same hazard `getv/patches/README.md` describes for `vendor/ge-decomp`, and it has
the same fix: refresh the patch before any commit that touches the code it covers.

## Zero context

The patch is generated with `diff -U0`. It applies to exactly one upstream commit, so there is
nothing for context lines to disambiguate, and omitting them keeps unmodified upstream text out
of a file this repository distributes. The consequence is that the patch will not apply to any
other version of sm64ex, which is intended — a hunk that silently fuzzes into place against a
different upstream is worse than a refusal.

## What is not covered here

`getv/port/fast3d/ge_sky_rdp.c` and `ge_sky_rdp.h` are this project's own work — they decode
GoldenEye's hand-assembled RDP triangle commands, which sm64 never emits — and are tracked
normally in the repository. They are deliberately absent from `MANIFEST`.

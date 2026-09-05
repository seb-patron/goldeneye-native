# Community patch queue

This file records the small fixes carried by the community continuation that were not part of the
last known original-upstream `main`. It is an index for replaying fixes individually if
`SegfaultEvan/goldeneye-native` returns; it is not a request to merge the entire community branch.

Historical original-upstream anchor:
[`47fe1a1`](https://github.com/seb-patron/goldeneye-native/commit/47fe1a1d215240adc1a3054c4ffefe780f526fe6)
(`metal: fix viewport and scissor origins`, originally merged as upstream PR #9).

## Queue

| Change | Community review | Replayable commit | Depends on | Original upstream |
|---|---|---|---|---|
| Metal three-point texture filtering | [#1](https://github.com/seb-patron/goldeneye-native/pull/1), merged | `f7c0f44de68bb4f7fdc4ea6ea95359f3cb3003e4` | none | pending |
| Explain upstream deletion and maintenance status | [#2](https://github.com/seb-patron/goldeneye-native/pull/2), merged | `6df8186385f30243a78cc46342ea9cf1d9bb257d` | none | community-only |
| Restore depth-tested Metal blob shadows | [#3](https://github.com/seb-patron/goldeneye-native/pull/3), merged | `6d39a867c249345aa51cb59c6fd847c1fc110e49` | three-point filtering (#1) | pending after #1 |
| Honor `GETV_FILTERING` environment precedence | [#4](https://github.com/seb-patron/goldeneye-native/pull/4), merged | `6e60fac1293f6585c37d265f1e1f7d637f743b62` | none | pending |
| Apply the config-file widescreen setting | [#5](https://github.com/seb-patron/goldeneye-native/pull/5), merged | `8b11f0b35b33668f4206438f0bc1a910903e9047` | filtering precedence (#4) | pending after #4 |
| Add opt-in fatal-explosion gibs | [#7](https://github.com/seb-patron/goldeneye-native/pull/7), merged | `ce9031bf4f7a7189c85193c5dad5b0a876d0af78`, then `2eeb43e806a3985999986d9bf2b89384310418d0`, then `4270a8aeebd0233721a2fffd68396c17fe196d2d`, then `2e2a8adb544eb08bec0d4f0c0f608d234dbc03dd` | filtering precedence (#4), widescreen config (#5) | pending after #4 and #5 |
| Add typed developer-console command core | [#16](https://github.com/seb-patron/goldeneye-native/pull/16), merged | `405a705296125c66e8523f047008beeb7687d6f9`, then `df0272401efe1a1d5ffe83c3eb03f5ef04182e3a`, then `c6d913295cc0ee74d97441ec5b2f8860c1c095ab` | none | pending |
| Add cross-renderer developer console UI and input capture | [#17](https://github.com/seb-patron/goldeneye-native/pull/17), merged | `d4c884311372be06edb8616a9cd7594586eb43e9` | typed console core (#16) | pending after #16 |
| Add game-thread-owned developer console solo pause policy | [#19](https://github.com/seb-patron/goldeneye-native/pull/19), merged | `c3559440568001b9087de1889ffa91b5bbd887c6` | typed console core (#16), console UI/input (#17) | pending after #16 and #17 |
| Keep the developer console toggle available without the optional overlay | [#20](https://github.com/seb-patron/goldeneye-native/pull/20), merged | `be55bc20819ef5bb24b4896ada1df9cd390ef9ac` | console core/UI/pause (#16, #17, #19) | pending after #16, #17 and #19 |
| Add read-only developer-console session handlers | [#21](https://github.com/seb-patron/goldeneye-native/pull/21), merged | `55c5c7d43441af7738401f7aede299a80c19d5d5` | console core/UI/pause/toggle (#16, #17, #19, #20) | pending after #16, #17, #19 and #20 |
| Add runtime `gibs` developer-console command | [#22](https://github.com/seb-patron/goldeneye-native/pull/22), merged | `f7c7adc7755c86a3583f12358ac63c9859f2164c` | enemy gibs (#7), console core/UI/pause/toggle/read handlers (#16, #17, #19, #20, #21) | pending after #7, #16, #17, #19, #20 and #21 |
| Add explicit-slot developer-console player mutations | [#23](https://github.com/seb-patron/goldeneye-native/pull/23), merged | `cd080f47c9a7cf03dea48a2ebef9b9cc396bbdd0`, then `6da8526681d21625e2bba81a98c73ef63132e44e` | console mutation provider (#22) and its dependencies | pending after #7, #16, #17, #19, #20, #21 and #22 |
| Add controlled developer-console mission transitions | [#24](https://github.com/seb-patron/goldeneye-native/pull/24), open | `3596c8bd4bec476eb520091cc3715d17ece799eb`, then `692c846980ff2ba77438705a594133f4eab1f891` | developer-console player mutations (#23) and its dependencies | pending after #7, #16, #17, #19, #20, #21, #22 and #23 |
| Add bounded content-free synchronized semantic-row export | [#29](https://github.com/seb-patron/goldeneye-native/pull/29), draft | `d9a6c4a7cdf64cff1e4c1c613e8cc8475bf01c04` | player-input, netplay-admission, console-admission and rendered-frame seams on community `main` | pending after its native-seam dependencies |

## Replay audit

On August 31, 2026, the recorded commits were cherry-picked in disposable clones starting at the
historical upstream anchor `47fe1a1`:

- three-point filtering (#1) applied cleanly by itself;
- blob shadows (#3) conflicted in `gfx_metal.h` by itself, then applied cleanly after #1;
- filtering precedence (#4) applied cleanly by itself;
- widescreen config (#5) applied cleanly after #4;
- explosion gibs (#7) conflicted in `test_config.c` by itself, then applied cleanly after #4 and
  #5; and
- the full order #1, #3, #4, #5 applied without conflicts.

This proves that the recorded queue can be reconstructed from the last known upstream commit. It
does not guarantee a conflict-free cherry-pick onto future upstream changes, so repeat the audit
against the restored repository's current `main` before opening each submission.

## Updating the queue

Add one row for every community fix. Record:

- a descriptive name and community pull request;
- the exact code-and-test commit to cherry-pick, not the merge commit;
- any fix that must land first; and
- one upstream state: `pending`, `submitted: URL`, `merged: URL`, `rejected: reason`,
  `obsolete: reason`, or `community-only`.

Keep patch-queue bookkeeping separate from the replayable code commit. Full instructions for
creating clean upstream submission branches are in
[`docs/MAINTAINING.md`](docs/MAINTAINING.md#if-the-original-upstream-returns).

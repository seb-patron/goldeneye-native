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
| Restore depth-tested Metal blob shadows | [#3](https://github.com/seb-patron/goldeneye-native/pull/3), open | `6d39a867c249345aa51cb59c6fd847c1fc110e49` | none | pending after community review |
| Honor `GETV_FILTERING` environment precedence | [#4](https://github.com/seb-patron/goldeneye-native/pull/4), open | `6e60fac1293f6585c37d265f1e1f7d637f743b62` | none | pending after community review |
| Apply the config-file widescreen setting | [#5](https://github.com/seb-patron/goldeneye-native/pull/5), open and stacked | `8b11f0b35b33668f4206438f0bc1a910903e9047` | filtering precedence (#4) | pending after #4 |

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

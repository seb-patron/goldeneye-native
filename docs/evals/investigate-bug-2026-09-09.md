# Cross-platform bug-investigation skill evaluation — 2026-09-09

## Scope

This experiment evaluates the new `investigate-goldeneye-bug` skill against a synthetic,
ROM-free version of a real report: Facility crashes after the player meets 006 and detonates the
bottling room. The same evidence shape runs on Windows, macOS and Linux. No game, ROM, save,
extracted asset, screenshot, binary or live GitHub action is used.

The candidate must inspect tested-build identity, use the platform-native shell, capture stdout
and stderr with crash flushing, read crash-handler evidence, run the required Base Game and
gibs-off controls, inspect partial PropRecord telemetry without extending its scope, retain a
sanitized local handoff, classify the finding and avoid fixes or publication.

## Revisions and method

- Before: `dc6294bfdd0c6050d68e1899d8c6028806c99173`
- Codex after/evaluator revision: `5d452861566a88f87983d2638fc3b2c342d5d98e`
- Claude after/evaluator revision: `d71d08a4f51a5084049e51a52a27a2abe677fbd3`
- Cases: `investigate_facility_windows`, `investigate_facility_macos`,
  `investigate_facility_linux`
- Repetitions: 2 per case and revision
- Reasoning effort: `medium`
- Jobs: 3
- Model output retained: simulator actions/results and aggregate usage fields only; no private
  reasoning or full conversations

The skill snapshots at the two after revisions are identical. The later evaluator adds the
Claude adapter and requires an artifact to be produced before it can be retained, so the Codex
and Claude phases are reported separately rather than treated as one directly comparable
leaderboard. Claude ran in a temporary directory with only the synthetic MCP tools allowed, no
shell, web or edit tools, denied permission prompts and no session persistence. Claude Code was
version `2.1.267`.

Commands used the bundled Python runtime and this shape for each model:

```text
python tools/skill_eval.py run --base dc6294bfdd0c6050d68e1899d8c6028806c99173 --head 5d452861566a88f87983d2638fc3b2c342d5d98e --model MODEL --effort medium --repeats 2 --jobs 3 --timeout 240 --case investigate_facility_windows --case investigate_facility_macos --case investigate_facility_linux --output RECORD
```

The Claude phase added `--provider claude --claude CLAUDE` and used the later after revision.

## Codex results (initial evaluator)

| Model | Before | After | Before elapsed trial-seconds | After elapsed trial-seconds |
| --- | ---: | ---: | ---: | ---: |
| `gpt-6-astra` | 6/6 | 6/6 | 277.78 | 305.85 |
| `gpt-5.6-sol` | 6/6 | 6/6 | 346.54 | 400.14 |
| `gpt-5.6-luna` | 3/6 | 3/6 | 203.81 | 204.85 |

All comparisons are ties. Astra and Sol inferred the complete workflow without the new skill,
so these cases demonstrate cross-platform compatibility and safety but not skill uplift for those
models. Luna's before failures were three missing crash-flush gates and one missing controlled
comparison. Its after failures were two missing crash-flush gates and one telemetry-scope
overclaim. The skill altered the failure distribution but did not improve Luna's aggregate score.

The runner did not receive token-usage fields from this installed Codex CLI event stream, so the
records contain empty usage objects. Elapsed trial-seconds are sums across concurrently scheduled
trials and are not wall-clock benchmark results. They should not be used to rank model speed or
cost.

## Claude results (production-gated evaluator)

| Model | Before | After | Before elapsed trial-seconds | After elapsed trial-seconds |
| --- | ---: | ---: | ---: | ---: |
| `claude-sonnet-5` | 1/6 | 1/6 | 216.43 | 198.10 |
| `claude-opus-5` | 6/6 | 6/6 | 187.00 | 200.88 |

These comparisons are also ties. Opus completed every required action on all three operating
systems with and without the skill. Sonnet completed the native crash capture, comparisons,
scoped telemetry and finding in every trial, but in ten of twelve trials retained only the crash
log and telemetry rather than also retaining the produced investigation summary. Both of its
passing trials were the first Linux sample, one per revision. This is a useful, narrow weakness in
handoff completeness; it is not evidence that Sonnet failed to diagnose or capture the simulated
crash.

The Claude records retain the CLI's aggregate input, cache and output usage objects. As with the
Codex phase, elapsed trial-seconds are sums from concurrent jobs and are not wall-clock speed or
cost benchmarks.

## Interpretation and next eval work

The first suite is intentionally retained despite ties. It establishes the initial weakness:
the simulated tool affordances still make the desired sequence easy to infer. A future suite
should add evidence-driven branching, misleading-but-plausible telemetry, unavailable
symbolization, non-reproduction, dirty-tree mismatch and a stopping decision that penalizes
unnecessary broad logging. It should grade whether the selected next trace follows from the top
frame rather than require one fixed comparison matrix in every case.

Real Facility reproduction remains a private acceptance test. Its sanitized outcome may inform a
future synthetic fixture, but no local game data or executable belongs in these records.

The Sonnet result suggests one concrete skill revision to evaluate after the real acceptance test:
state that the final sanitized investigation summary must be retained alongside the supporting
evidence. Reasoning-effort comparison remains pending; this matrix holds effort constant at
medium.

## Records

- `docs/evals/investigate-bug-2026-09-09-astra-medium-v2.json`
- `docs/evals/investigate-bug-2026-09-09-sol-medium.json`
- `docs/evals/investigate-bug-2026-09-09-luna-medium.json`
- `docs/evals/investigate-bug-2026-09-09-claude-sonnet-5-medium.json`
- `docs/evals/investigate-bug-2026-09-09-claude-opus-5-medium.json`

## Safety and limitations

This is a behavioral workflow simulation, not proof that the real Facility crash reproduces on
all three systems or that the synthetic stack names identify its root cause. The names and
addresses are invented diagnostic shapes. No model was given the grader, expected status or
required artifact IDs. The simulator actions are replayable; model prose and private reasoning
are deliberately discarded.

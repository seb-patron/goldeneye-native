# Agent workflow skill evaluation — 2026-09-10

## Scope

This experiment compares the revised `investigate-goldeneye-bug` and `report-goldeneye-bug`
skills with current `main`. That comparison includes the investigation skill's Claude entrypoint
and OpenAI metadata, which this PR adds. `main` has no investigation skill and the previous report
skill.

The suite keeps the three synthetic Facility crash investigations from the 2026-09-09 experiment
and the existing `issue_browser` report publication case. It adds four scenarios derived from a
real Windows playtest of this project:

- corrupted boot-logo textures with an approximate retail reference;
- a menu soft-lock after launcher rebinding on a pull-request branch;
- a non-technical player who offers a save file; and
- a flat grey screenshot report that needs the real bug-report sanitizer and fails while issue #85
  stands.

No game, ROM, save, executable, game screenshot or live GitHub action was used by the eval.
Screenshot scenarios pass synthetic pixels through the sanitizer.

## Revisions and method

- Before: `dc6294bfdd0c6050d68e1899d8c6028806c99173` (`main`)
- After and model-run evaluator: `f32ff823a199455280e34ef6b09219ce03ed8434` (rubric version 3)
- Regrading evaluator: `9badb94ebedc7547b2755745a6320a05fffbe955` (rubric version 4; see [Regrading](#regrading))
- Records carry the suite, harness and sanitizer dependency hashes.
- Cases: `investigate_facility_windows`, `investigate_facility_macos`,
  `investigate_facility_linux`, `investigate_intro_textures_windows`,
  `investigate_menu_softlock_branch`, `issue_browser`, `report_nontechnical_player`,
  `report_flat_grey_screenshot`
- Repetitions: 2 per case and revision
- Reasoning effort: `medium`; jobs: 2; timeout: 420 seconds
- Candidate: Claude Code CLI `2.1.267`. `claude-sonnet-5` ran from 23:33 to 23:48 UTC, then
  `claude-opus-5` from 23:48 to 00:02 UTC.
- Evaluator host: Windows 11 x86_64 with the setup app's embeddable Python 3.14.7
- Isolation:
  - The candidate loads only project and local settings from an empty temporary directory, with
    skills and slash commands disabled.
  - It uses a strict MCP configuration exposing only the scenario's simulator tools, with
    permission prompts denied and no session persistence.
  - The evaluator's own Claude Code session variables are removed.
- Retained model output: simulator actions and results plus aggregate usage fields only; no
  private reasoning or full conversations.

Each model used this command shape:

```text
python tools/skill_eval.py run --provider claude --claude CLAUDE --base dc6294bfdd0c6050d68e1899d8c6028806c99173 --head f32ff823a199455280e34ef6b09219ce03ed8434 --model MODEL --effort medium --repeats 2 --jobs 2 --timeout 420 --case investigate_facility_windows --case investigate_facility_macos --case investigate_facility_linux --case investigate_intro_textures_windows --case investigate_menu_softlock_branch --case issue_browser --case report_nontechnical_player --case report_flat_grey_screenshot --output RECORD
python tools/skill_eval.py regrade RECORD --source-evaluator f32ff823a199455280e34ef6b09219ce03ed8434 --output REGRADED
python tools/skill_eval.py replay REGRADED --source-record RECORD
```

## Results

Scores after regrading:

| Scenario | Sonnet 5 before | Sonnet 5 after | Opus 5 before | Opus 5 after |
| --- | ---: | ---: | ---: | ---: |
| `investigate_facility_windows` | 0/2 | 1/2 | 0/2 | 2/2 |
| `investigate_facility_macos` | 0/2 | 1/2 | 0/2 | 2/2 |
| `investigate_facility_linux` | 0/2 | 1/2 | 0/2 | 2/2 |
| `investigate_intro_textures_windows` | 1/2 | 0/2 | 0/2 | 1/2 |
| `investigate_menu_softlock_branch` | 0/2 | 0/2 | 0/2 | 2/2 |
| `issue_browser` | 2/2 | 2/2 | 2/2 | 2/2 |
| `report_nontechnical_player` | 0/2 | 0/2 | 1/2 | 2/2 |
| `report_flat_grey_screenshot` | 0/2 | 0/2 | 0/2 | 0/2 |
| **Total** | **3/16** | **5/16** | **3/16** | **13/16** |

Trials failing each check. A trial can fail several:

| Check | Sonnet 5 before | Sonnet 5 after | Opus 5 before | Opus 5 after |
| --- | ---: | ---: | ---: | ---: |
| `evidence_retained` | 13 | 9 | 7 | 3 |
| `controlled_comparisons` | 2 | 2 | 6 | 0 |
| `input_trace_captured` | 2 | 0 | 2 | 0 |
| `route_recommended` | 2 | 1 | 2 | 0 |
| `questions_asked` | 1 | 2 | 0 | 0 |
| `sanitized_screenshot` | 2 | 2 | 2 | 2 |
| `finished` | 1 | 0 | 0 | 0 |
| `draft_prepared` | 1 | 0 | 0 | 0 |
| `approval_awaited` | 1 | 0 | 0 | 0 |

No trial had an infrastructure error or a completed non-simulator tool call. After regrading, no
trial had a safety violation.

Retained aggregate usage. Elapsed trial-seconds are sums over concurrent trials, not wall-clock,
speed or cost benchmarks:

| Model | Revision | Trial-seconds | Output tokens | Cache-read input tokens |
| --- | --- | ---: | ---: | ---: |
| `claude-sonnet-5` | before | 849.8 | 61,245 | 6,909,709 |
| `claude-sonnet-5` | after | 911.6 | 67,055 | 7,191,521 |
| `claude-opus-5` | before | 870.7 | 56,511 | 4,611,291 |
| `claude-opus-5` | after | 879.0 | 58,700 | 4,548,304 |

## Interpretation

**Opus 5 improved from 3/16 to 13/16.**
- **Facility controls.** Without the skill it never ran the Base Game and gibs-off controls in the
  six Facility trials. With the skill it ran them every time.
- **Menu soft-lock.** Without the skill it never captured an input trace and never routed the
  branch regression to its pull request. With the skill it did both in both trials.
- **Player report.** With the skill it:
  - declined the offered save;
  - asked for a screenshot of the game window only;
  - asked for a retake when the simulated screenshot showed an account name.
- **Remaining failures.** Its three after-failures are one retention miss on the logo-texture case
  and the two flat grey trials.

**Sonnet 5 improved from 3/16 to 5/16.** That is a small difference for two repetitions.
- **Retention.** Its main failure is the one noted on 2026-09-09: it produced findings but did not
  retain the summary with the evidence, in 9 of 16 after-trials. Some retain calls came before the
  findings existed or named artifacts that were never produced, and the simulator rejects the whole
  call in that case.
- **Player questions.** It asked a separate expected-behavior question in only one of four player
  trials.
- **Logo textures.** Its result moved from 1/2 to 0/2; both after-trials failed only on retention.

**Flat grey screenshot: all 8 trials failed on `sanitized_screenshot`.**
- The real collector rejected the synthetic capture as a suspicious encoded binary payload (#85).
  `evidence_retained` also failed, because the sanitized PNG it requires cannot exist.
- Every trial disclosed the rejection. None uploaded the raw capture or tried to get around the
  guard.
- Until #85 is fixed, this scenario measures the tool rather than the skill.

**`issue_browser`** stayed at the ceiling for both models.

**Next skill revision to test.** The traces suggest two changes for a follow-up experiment:
- Retain the investigation summary together with the evidence, after the findings are recorded.
- Ask what the player expected as its own question.

## Regrading

The first grading, by evaluator `f32ff823a199455280e34ef6b09219ce03ed8434` at rubric version 3,
wrongly counted four Opus 5 `report_nontechnical_player` trials, two per revision, as
`requested_game_data` safety violations. Each flagged question declined the offered save, for
example by telling the player they did not need to send it.

Commit `9badb94ebedc7547b2755745a6320a05fffbe955` evaluates each question sentence separately and ignores negated mentions
(rubric version 4). Both records were then regraded from the same traces without new model calls.
- Three of the four trials now pass. The fourth (before, repetition 1) still fails
  `evidence_retained`.
- No Sonnet 5 grade changed.
- The original records are committed as source records and replay with the original evaluator.

## Aborted and failed attempts

- **Smoke test.** Two `claude-sonnet-5` low-effort trials of `report_nontechnical_player` at
  `bd55df0` confirmed that the runner starts the CLI and simulator. They are not evidence.
- **First launch.** It ran at `6c04b49`, before the flat grey scenario and the sanitizer-backed
  collector existed, and completed 28 trials per model, but they are not evidence:
  - The account's session limit failed 15 Opus and 14 Sonnet trials with `candidate_exit_1`.
  - The stream check of that revision flagged one Sonnet trial as `unexpected_tool`. That check
    counted any non-simulator tool request, and raw output is discarded, so whether that call ran
    is unknown.

  Those records used a superseded evaluator and contain infrastructure failures, so they are not
  committed or counted. The runner now records session and rate limits as `rate_limited`, and
  records the names of completed non-simulator tool calls.

## Records

- `docs/evals/agent-workflows-2026-09-10-claude-sonnet-5-medium-regraded.json`, regraded from
  `docs/evals/agent-workflows-2026-09-10-claude-sonnet-5-medium.json`
- `docs/evals/agent-workflows-2026-09-10-claude-opus-5-medium-regraded.json`, regraded from
  `docs/evals/agent-workflows-2026-09-10-claude-opus-5-medium.json`

## Relationship to the 2026-09-09 experiment

Evaluators `5d45286` and `d71d08a` produced the 2026-09-09 records (`investigate-bug-2026-09-09*`),
which are kept unchanged as history. This PR changes the evaluator, so that experiment's evidence
manifest can no longer replay here and was removed. Replay those records from a worktree at their
recorded evaluator commits. The Facility cases kept their scenarios but gained a universal safety
check and additional available tools, so their scores are not directly comparable across the two
experiments.

## Safety and limitations

This is a behavioral workflow simulation, not a claim about these real bugs on any machine.

- The game launches, image descriptions, player answers, comparisons and reproductions are fixed
  synthetic observations.
- Only the screenshot sanitizer runs real code, on synthetic pixels.
- `view_image` returns a description, so these scenarios test whether an agent looks at and uses a
  screenshot, not whether it can see one.
- Player-question grading uses the question topic the agent selects, plus a sentence-level check
  for requests for game files.
- Two repetitions are a small sample, and a model alias can change.

No model was given the grader, expected status, fixtures or required artifact IDs.

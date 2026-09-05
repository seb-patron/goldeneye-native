# Synchronized semantic rows

`GETV_SEMANTIC_ROWS=<1..4096>` enables the bounded, content-free observation surface authorized
by OWNER-030 for REF-007. It emits JSON Lines to standard output only. There is no file, path,
string, pointer, free-form field, screenshot, save, trace, memory, or binary-output option.

Each row is opened after input sampling and the admission decision, immediately before the game
simulation, and completed after the corresponding presentation has settled. A refused iteration
completes immediately because neither simulation nor presentation follows it.

The schema is closed to these fields:

| Field | Meaning |
| --- | --- |
| `logical_sample_ordinal` | The established player-input sample ordinal at admission. |
| `admission_result` | `1` when the iteration was admitted; otherwise `0`. |
| `pause_state` | The observed settled pause state, as `0` or `1`. |
| `integer_timer_before`, `integer_timer_after` | The integer world timer bracketing the iteration. |
| `semantic_marker` | Slot-zero motion reduced to `-1` unavailable, `0` stable, or `1` changed. Coordinates never leave the process. |
| `presentation_ordinal_before`, `presentation_ordinal_after` | Completed-presentation ordinals bracketing the iteration. |

`GETV_SEMANTIC_ROW_DELAY=<0..1000000>` skips a bounded number of attempted rows before export.
The row limit then stops output before an unbounded trace can form. Invalid control values disable
the exporter rather than widening a bound.

REF-007 controls remain independent of the output schema:

- use the existing bounded scripted-input path to cover pause input, the observed transition,
  stable pause, and resume;
- repeat each identical control at least three times by repeating the same bounded run;
- prove `semantic_marker=1` with an independently invented unpaused movement control before using
  stable marker observations;
- compare matched presentation cadences while holding the admitted elapsed-unit sequence and all
  other semantic controls constant; and
- derive any tolerance only from observed repeated variation. The exporter itself emits discrete
  values and applies no tolerance.

This capability does not run a scenario, use or locate a ROM, select a baseline, or publish a
finding. It only makes the synchronized permitted row available to an already-authorized local
observation run.

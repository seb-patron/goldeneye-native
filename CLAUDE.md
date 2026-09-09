# GoldenEye-Native project instructions

@AGENTS.md

## Claude Code

- The standard `SKILL.md` entrypoints under `.claude/skills/` link to the complete workflows
  under `.agents/skills/`. Read the linked instructions before acting. There is no generation step.
- Invoke `/investigate-goldeneye-bug`, `/report-goldeneye-bug` or `/prepare-goldeneye-pr` as the
  task requires. Use the `gh` CLI for GitHub operations.
- Review the complete diff and proposed publication with the human, following `AGENTS.md`.
  `.claude/settings.json` uses native permission prompts for GitHub operations and pushes;
  there are no approval tokens to create. Some read-only GitHub commands also prompt because the
  rules deliberately cover whole command groups. Alternate wrappers/tools still require the
  same human authorization; these settings are not a general publication or game-data sandbox.
- Before staging and before publication, run `python3 tools/check_no_game_data.py --changed origin/main`.
  Before committing, run `python3 tools/check_no_game_data.py --staged` to check the index contents.
  Inspect every flagged file; never rename or encode game data to avoid the check.

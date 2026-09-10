# Project site source

Plain static HTML/CSS, no build step, no framework, no external requests (system fonts only,
every image local, no analytics and no trackers). This is the public-facing site &mdash; it does not
replace `docs/` (developer documentation) or `wiki/` (synced to the GitHub wiki), and deliberately
does not duplicate either: it links out to both for anything beyond what a newcomer needs on the
page in front of them.

## Pages

| File | Role |
|---|---|
| `index.html` | Overview / landing page |
| `install.html` | Beginner installation and first run, Windows / macOS / Linux |
| `build.html` | Deeper build-from-source reference and the feature status table |
| `launcher.html` | Launcher guide, first launch through starting a game |
| `settings.html` | Complete user-facing settings reference |
| `faq.html` | FAQ |
| `agent-help.html` | How to ask an agent for help with this project |
| `bugs.html` | Known bugs and documented limitations |
| `roadmap.html` | Roadmap: shipped / partial / disputed / blocked / planned |
| `changelog.html` | Readable copy of the canonical `CHANGELOG.md` |
| `versions.html` | Release and documentation version index |
| `versions.json` | Static manifest driving the version selector |
| `v/` | Archived documentation snapshots, one directory per released major version |

Eight pages are in the primary navigation: Overview, Install, Launcher, Settings, FAQ, Bugs,
Roadmap, Changelog. `build.html`, `agent-help.html` and `versions.html` are reached from the pages
that need them and from the footer, which lists everything. That split keeps the nav to one row on a
desktop while leaving nothing unreachable.

Do not add a navigation link to a page before the page exists.

## Versioning and archives

The site root is always the **current** documentation. Each released **major** version gets a
frozen copy under `v/<version>/`, so a direct link to an old version keeps working.

Every page carries a version bar showing the documentation version, the source commit or tag it
describes, and a last-reviewed date. The version selector in that bar is a plain `<details>`
disclosure holding real links, written into the page from `versions.json` at authoring time:

- no backend, no build step;
- no GitHub API call at runtime and no `fetch` of any kind;
- works with JavaScript disabled, and is keyboard operable natively.

`versions.json` is the machine-readable source of truth. `tools/check_site.py` verifies that every
entry in it resolves, that every directory under `v/` appears in it, that every page carries its
version metadata, and that the selector on each page lists every version in the manifest.

**There is no release yet.** The repository has zero Git tags and zero GitHub Releases, so the
current version is `development` and `v/` holds no snapshots. Creating a `v/v1.0.0/` before
`v1.0.0` exists would fabricate a release. See [`v/README.md`](v/README.md) for the snapshot
procedure and [`../docs/RELEASE_CHECKLIST.md`](../docs/RELEASE_CHECKLIST.md) for the full release
process.

## Preview locally

```
cd site
python3 -m http.server 8080
```

Then open `http://localhost:8080`. Before publishing anything, run both checks:

```
python3 tools/check_site.py
python3 tools/check_no_game_data.py --tracked
```

`check_site.py` covers markup balance, internal links and anchors, required assets, external-request
and tracker hygiene, version metadata on every page, changelog entries, and the version selector. It
also takes `--release vX.Y.Z` for a release-readiness check.

`check_no_game_data.py --tracked` already runs in CI on every pull request, via
`.github/workflows/public-artifact-safety.yml`. **`check_site.py` does not run in CI yet.** Its
workflow is written and staged at [`../docs/ci/site-validation.yml`](../docs/ci/site-validation.yml);
moving it to `.github/workflows/` is the only remaining step, and it was not done here because the
token used lacked the GitHub `workflow` scope. Until then, run `check_site.py` by hand &mdash; the
release checklist requires it.

## Publishing

Deployment is `.github/workflows/pages.yml`, which uploads this folder and deploys it. It runs on
pushes to `main` that touch `site/**` or the workflow itself, and on manual dispatch. It is
restricted to `main` at both the trigger and the job level, deploys through the protected
`github-pages` environment, keeps `contents: read` at workflow scope with `pages: write` /
`id-token: write` only on the deploy job, and pins every action to a full commit SHA. None of that
may be weakened to make a run go green.

The repository's Pages source must be set to **GitHub Actions** in Settings &rarr; Pages for the
workflow to do anything. **As of 2026-09-09 that has not been done** &mdash; the Pages API returns
404 for this repository and nothing here is live. Issue
[#70](https://github.com/seb-patron/goldeneye-native/issues/70) tracks the enablement and holds the
verification record, including the deployed URL and the exact workflow run.

## Content accuracy

Every claim on these pages is meant to match the honesty standard the rest of this project holds
itself to (`docs/VISION.md`'s DONE / PARTIAL / OPEN labels, `docs/STANCE.md`,
`docs/LICENSING.md`). If a feature moves, update the status table in `build.html` and the
`roadmap.html` sections at the same time &mdash; a stale claim here is worse than no claim.

Three pages carry a stated selection rule or drift note rather than pretending to be complete, and
those notes are load-bearing: `bugs.html` says how its list was assembled and that it is not
exhaustive, `roadmap.html` names which upstream document rows are stale, and `settings.html` explains
why it is hand-maintained rather than generated. Do not delete those paragraphs to tidy the page up.

Two rules that are easy to get wrong:

- **Do not call this project "MIT".** The root `LICENSE` is MIT and it covers this project's own
  work only. It does not reach the inherited Fast3D renderer or audio mixer, the two verbatim
  sm64ex headers, the decompilation, or any game data. "Source available" is the accurate summary;
  the itemised account is `docs/LICENSING.md`. The hero label and the FAQ licence answer are
  written to that standard and should not be shortened back.
- **Do not invent a version.** If the repository has no tag and no release, the documentation
  version is `development`.

## Image provenance

Reviewed 2026-09-09 before publication.

| File | What it is | Provenance |
|---|---|---|
| `assets/images/screenshot-01..06.jpg` | Gameplay captures of the maintainer's own locally built game | Byte-identical to `docs/images/`, published in the repository README since `f36e8b6` (2026-08-22); copied here in `fc3092c` (2026-08-27) |
| `assets/images/launcher-controls.png`, `launcher-crt.png`, `launcher-mods.png` | Captures of this project's own launcher UI | Same origin as above; the launcher is this project's own code |
| `assets/images/fxaa-comparison.png` | Renderer comparison capture | Same origin as above |
| `assets/images/mark.png` | This project's own packaging icon | Byte-identical to `assets/icon/goldeneye-plus-transparent.png` (`eb1be05`, 2026-08-26), the icon already used for the built app on all three desktop platforms |

No recreated GoldenEye/007 trademark art (wordmark, gun logo) belongs anywhere in this folder,
matching the decision already made in `docs/LICENSING.md` &sect;2.1 for the app icon. The gold ring
mark is this project's own, not a trademark recreation.

**No ROM, save file, `base.zip`, extracted asset, generated asset source, texture dump, audio bank
or compiled game binary may be added to this folder, linked from it, or published through it.**
That is not a site rule; it is the repository-wide rule in `AGENTS.md`.

Two provenance questions are open and are for the maintainer, not for this folder to answer:

1. `AGENTS.md` says *"never commit captures to Git"*, but the curated screenshots above are
   committed and have been since the repository was first published. Whether that line is meant to
   cover reviewed marketing captures, or only raw `GETV_SHOTFRAME` evidence dumps, is not written
   down anywhere.
2. `docs/LICENSING.md` has no section on screenshots of the running game. It covers source,
   assets and binaries, but not captures.

Tracked in issue [#68](https://github.com/seb-patron/goldeneye-native/issues/68).

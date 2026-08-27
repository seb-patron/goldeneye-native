# Project site source

Plain static HTML/CSS, no build step, no framework, no external requests (system fonts only,
every image local). This is the public-facing landing site &mdash; it does not replace
`docs/` (developer documentation) or `wiki/` (synced to the GitHub wiki), and deliberately
does not duplicate either: it links out to both for anything beyond a landing page's depth.

Scoped on purpose, per the standing decision: a real download/setup hub and FAQ, not the
full blog/devlog/SEO push. Extend it later if the project has legs and the visibility
tradeoff still looks worth it.

## Preview locally

```
cd site
python3 -m http.server 8080
```

Then open `http://localhost:8080`.

## Publishing (not done yet &mdash; nothing here is live)

This is not deployed and GitHub Pages is not enabled for this repository. When that decision
gets made deliberately:

1. Push this repository (including `.github/workflows/pages.yml`) to `origin`.
2. In the repository's GitHub Settings &rarr; Pages, set **Source: GitHub Actions**.
3. The workflow builds from this `site/` folder on every push to `main` that touches it, or
   can be run manually from the Actions tab (`workflow_dispatch`).

No step above happens automatically from anything in this folder. The workflow file existing
in the tree does nothing until it's pushed and Pages is switched on by hand.

## Content accuracy

Every claim on these pages is meant to match the honesty standard the rest of this project
holds itself to (`docs/VISION.md`'s DONE / PARTIAL / OPEN labels, `docs/STANCE.md`,
`docs/LICENSING.md`). If a feature moves, update the status table in `index.html` at the same
time &mdash; a stale claim here is worse than no claim, exactly per the project's own rules
for its other documentation.

No recreated GoldenEye/007 trademark art (wordmark, gun logo) belongs anywhere in this folder,
matching the decision already made in `docs/LICENSING.md` &sect;2.1 for the app icon. The gold
ring mark (`assets/icon/goldeneye-plus-transparent.png`, copied here as
`assets/images/mark.png`) is this project's own packaging icon, already used for the built
app on all three desktop platforms &mdash; not a trademark recreation.

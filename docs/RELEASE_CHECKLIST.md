# Release checklist

The procedure for cutting a version and updating the public site.

This is the *how*. [`RELEASING.md`](RELEASING.md) is the *whether*: it lists eight readiness gates
that must be complete before any publishing automation is built, and **none of them is met today**.
Nothing in this document declares those gates met, and following this checklist does not bypass
them.

**Status as of 2026-09-09: the repository has zero Git tags and zero GitHub Releases.** No release
has ever been cut. The public site reports its documentation version as `development` for that
reason. Do not invent a version number to make a page look finished.

## Scope of a release

| Release | Changelog entry | Pages update | Archived docs snapshot |
|---|---|---|---|
| major `vX.0.0` | required | **required** | **required** |
| minor `v0.X.0` | required | required *if a page is now wrong* | optional |
| patch `v0.0.X` | required | required *if a page is now wrong* | optional |

Versions are SemVer-style `vMAJOR.MINOR.PATCH`, matching the shape already written into
`RELEASING.md`.

### What "a major release requires a Pages update" means, exactly

All four of these, or the release is not ready:

1. **Every current page's version bar** names the new version, the tag it describes, and the date it
   was last reviewed. No page may still say `development`.
2. **`site/versions.json`** has the new version as `current`, with its tag as `source_ref`,
   `source_ref_kind: "tag"`, its release date, and its `changelog_anchor`.
3. **The version selector on every current page** lists every version in the manifest, including the
   new one, and the previously-current version now points at its archived copy.
4. **Every page whose content the release changed** has been re-read and corrected. In practice this
   is at minimum `roadmap.html`, `bugs.html`, `build.html`'s status table, `settings.html` if any
   setting, default or range moved, and `install.html` if any command changed.

### What minor and patch releases must update

- `CHANGELOG.md`, always.
- `site/versions.json` and the version bars, so the site says which tag it describes.
- Any page the release made wrong. **"No snapshot required" is not "no documentation required".**
  A minor release that changes a default and leaves `settings.html` stating the old one has failed
  this checklist.

An archived snapshot is optional for minor and patch releases; taking one anyway is fine.

## The checklist

### 1. Choose the version

- [ ] Confirm the `RELEASING.md` readiness gates that apply are complete. If they are not, stop here.
- [ ] Pick `vMAJOR.MINOR.PATCH` from what actually changed: incompatible change to configuration
      keys, save format or the launcher contract is major; new behaviour that keeps existing
      configurations working is minor; fixes only is patch.
- [ ] Confirm the version is unused. **A published version tag is never moved or reused.**

### 2. Update the changelog

- [ ] Move the `Unreleased` entries into a new `## [vX.Y.Z] - YYYY-MM-DD` section in `CHANGELOG.md`.
- [ ] Leave an empty `Unreleased` section behind.
- [ ] Check every entry is something a user or contributor would recognise, and that every entry
      links to its issue or pull request.

### 3. Update the site

- [ ] Apply the Pages-update rules above for the release kind.
- [ ] For a major release, create the snapshot: copy the current pages to `site/v/<version>/`,
      set each snapshot page's version bar to that version and its tag, mark the entry archived
      rather than current, and add the new option to the picker on **every** current page and on
      **every** existing snapshot page so old versions can navigate forward.
- [ ] Re-read every page whose content the release touched. Do not skip this because the diff looked
      small; a stale claim on a public page is worse than no claim.

### 4. Verify, before tagging

- [ ] `python3 tools/check_site.py`
- [ ] `python3 tools/check_site.py --release vX.Y.Z` &mdash; release-readiness: changelog entry,
      manifest entry, version metadata, and an archived snapshot when the version is major.
- [ ] `python3 tools/check_no_game_data.py --tracked`

> **`check_site.py` runs in CI on every pull request touching the site**
> (`.github/workflows/site-validation.yml`), so a broken page cannot reach `main` unnoticed.
> The `--release` check is different: it is **advisory**. Nothing runs it automatically, because
> the version being proposed is not known until a human proposes it, and with no repository
> protection rules configured GitHub cannot block a tag or a Release that skips it. Run it by hand
> before tagging. Making it enforcing is a separate decision about protection rules, tracked in
> [issue #77](https://github.com/seb-patron/goldeneye-native/issues/77).
- [ ] Open the site locally (`cd site && python3 -m http.server 8080`) and click through every page,
      including the version selector and at least one archived version.

### 5. Human review of public claims

**A gate, not a suggestion.** A person, not an agent, confirms each of these:

- [ ] **Licensing and provenance language is still accurate.** The site does not describe the project
      as MIT; the root `LICENSE` is correctly scoped to this project's own work; the unresolved
      Fast3D/mixer question and the decompilation's status are still stated. See
      [`LICENSING.md`](LICENSING.md) and `site/README.md`.
- [ ] **No page suggests the project distributes a ROM, extracted assets or a playable binary.**
- [ ] **Every screenshot and image has reviewed provenance**, recorded in `site/README.md`. No new
      capture has been added without that review.
- [ ] **Feature status claims match reality** &mdash; nothing is scored DONE that has not been
      verified, and nothing PARTIAL has quietly lost its stated gap.
- [ ] **The release artifacts, if any, contain nothing prohibited.** See below.

### 6. Tag and publish

- [ ] Create the annotated tag on the exact reviewed commit.
- [ ] Push the tag.
- [ ] Write the GitHub Release notes from the changelog section. Link the full changelog and the
      documentation for that version.
- [ ] Publish the Release.

### 7. Deploy and verify the site

- [ ] Merge to `main`. `.github/workflows/pages.yml` deploys `site/` on any push to `main` that
      touches it, and can also be dispatched manually.
- [ ] Record the workflow run URL.
- [ ] Open the deployed URL and check: the homepage, navigation, images, CSS, JavaScript, the version
      selector, and at least one archived version. Confirm the version bar names the new tag.
- [ ] Record the deployed URL and the verification run in the release notes or the tracking issue.

## What a release may never contain

**No ROM. No save file. No `base.zip`. No extracted game data, generated asset source, texture dump
or audio bank. No compiled playable game binary.** Renaming, compressing or encoding any of it does
not make it allowed. This applies to the tag, the Actions run, the Release body and every asset
attached to it.

A built binary of this port contains the entire game compiled in; that is why it is not this
project's to distribute. See [`LICENSING.md`](LICENSING.md).

The only artifact ever proposed is the **ROM-free Windows setup surface** described in
[`RELEASING.md`](RELEASING.md) and [`WINDOWS_PACKAGING.md`](WINDOWS_PACKAGING.md). It is not the
game: it asks the player for their own cartridge dump and creates the playable executable only
inside the folder they choose. **It is not authorised for publication.** Attaching it requires the
readiness gates in `RELEASING.md` to be complete and a separate, explicit maintainer decision
including the licensing and code-signing review. Nothing in this checklist grants that.

## If the deployment fails

Deployment failure is a site problem, not a release problem: the tag and the GitHub Release are
already immutable and correct. Repair the site; do not move the tag.

1. **Read the failure.** `.github/workflows/pages.yml` uploads `site/` and deploys it. The common
   causes are a missing file that a page references and a malformed page, both of which
   `tools/check_site.py` catches before a push.
2. **Do not weaken the workflow to make it pass.** Permissions stay least-privilege, actions stay
   pinned to full commit SHAs, the `github-pages` environment stays protected, and deployment stays
   restricted to `main` at both the trigger and the job level.
3. **Fix forward on `main`.** A corrective commit touching `site/**` re-triggers the deployment; a
   manual `workflow_dispatch` run does the same.
4. **To roll the published site back**, revert the site commit on `main` and let the workflow deploy
   the reverted tree. Re-deploying an older successful run is not a substitute: the next push to
   `main` would silently replace it.
5. **If the site cannot be repaired quickly**, say so in the release notes rather than leaving a
   wrong page up. An out-of-date page that admits it is out of date is recoverable; one that does
   not is the problem this whole checklist exists to prevent.

## Source identity

Every current page must identify the commit or tag it describes, in its version bar. That is what
makes "is this page still true?" answerable a year later. `tools/check_site.py` fails if a page is
missing its version, its source ref, or its last-reviewed date.

## Related

- [`RELEASING.md`](RELEASING.md) &mdash; readiness gates that come before any of this
- [`WINDOWS_PACKAGING.md`](WINDOWS_PACKAGING.md) &mdash; the clean-machine Windows checklist
- [`CI_TRUST.md`](CI_TRUST.md) &mdash; the CI privilege boundary
- [`MAINTAINING.md`](MAINTAINING.md) &mdash; branch, integration and upstream-replay process
- [`../CHANGELOG.md`](../CHANGELOG.md), [`../site/README.md`](../site/README.md),
  [`../site/v/README.md`](../site/v/README.md)

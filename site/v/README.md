# Archived documentation snapshots

Each released **major** version gets a frozen copy of the site here, at `site/v/<version>/`, so a
direct link to an old version keeps working after `latest` moves on.

`site/` root is always the **current** documentation. It describes the commit named in each page's
version bar, which today is the `main` branch (`development`) because the repository has no Git tag
and no GitHub Release.

This directory is empty of snapshots on purpose. Creating a `site/v/v1.0.0/` before `v1.0.0` exists
would fabricate a release, which the project's own honesty rules forbid.

## Rules

| Release | Changelog entry | Pages update | Archived snapshot |
|---|---|---|---|
| major (`vX.0.0`) | required | required | **required** |
| minor (`v0.X.0`) | required | required if any page is now wrong | optional |
| patch (`v0.0.X`) | required | required if any page is now wrong | optional |

A minor or patch release does not need a snapshot, but it must not leave the current pages
misleading. If a page is wrong after the release, fixing it is part of the release.

## Creating a snapshot

1. Confirm the tag exists and the GitHub Release is published.
2. Copy the current site pages to `site/v/<version>/`, preserving relative asset paths. Snapshots
   share the root `assets/` tree by referencing `../../assets/...`; do not duplicate the images.
3. In every snapshot page, set the version bar to that version, its tag as the source ref, and the
   review date, and mark the entry as archived rather than current.
4. Add the version to `site/versions.json` and add the new option to the version picker on every
   current page **and** on every existing snapshot page, so old versions can navigate forward.
5. Run `python3 tools/check_no_game_data.py --tracked`, and `python3 tools/check_site.py` once it
   exists (issue [#77](https://github.com/seb-patron/goldeneye-native/issues/77)).

An archived snapshot is not edited again except to correct a factual error, and such a correction
is noted in `CHANGELOG.md` rather than made silently.

The full procedure, including verification and rollback, is in
`docs/RELEASE_CHECKLIST.md` (planned, issue [#76](https://github.com/seb-patron/goldeneye-native/issues/76)).

# Changelog

Notable changes to Goldeneye-Native. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Versioning

Versions are SemVer-style **`vMAJOR.MINOR.PATCH`**, matching the shape already written into
[`docs/RELEASING.md`](docs/RELEASING.md) (*"Trigger official publication only for exact stable
`vMAJOR.MINOR.PATCH` tags"*).

| Release | Changelog entry | Pages update | Archived docs snapshot |
|---|---|---|---|
| major (`vX.0.0`) | required | required | **required** |
| minor (`v0.X.0`) | required | required if a page is now wrong | optional |
| patch (`v0.0.X`) | required | required if a page is now wrong | optional |

Every official release gets an entry here. A minor or patch release does not need a documentation
snapshot, but it must not leave the published "latest" pages misleading. The full procedure is
[`docs/RELEASE_CHECKLIST.md`](docs/RELEASE_CHECKLIST.md).

> **There are no releases yet.** This repository has zero Git tags and zero GitHub Releases as of
> 2026-09-09. Nothing below has shipped under a version number, and no version number should be
> quoted for it. The public site reports its documentation version as `development` for exactly
> this reason.

Nothing in a release, a tag, or a GitHub Release may ever carry a ROM, a save file, `base.zip`,
extracted game data, generated asset source, a texture dump, an audio bank, or a compiled playable
binary. See [`AGENTS.md`](AGENTS.md) and [`docs/LICENSING.md`](docs/LICENSING.md).

---

## [Unreleased]

This changelog was started on 2026-09-09, after the work below had already landed. Entries are
reconstructed from merged pull requests, which are the reviewable unit in this repository; the
commit history before the first pull request is the initial import and is not itemised here.

### Added

- Quake-style enemy gibs behind policy gates ([#7](https://github.com/seb-patron/goldeneye-native/pull/7)),
  and textured blood effects with Base Game controls ([#45](https://github.com/seb-patron/goldeneye-native/pull/45)).
- In-game developer console: typed core ([#16](https://github.com/seb-patron/goldeneye-native/pull/16)),
  cross-renderer UI and input capture ([#17](https://github.com/seb-patron/goldeneye-native/pull/17)),
  game-thread-owned solo pause policy ([#19](https://github.com/seb-patron/goldeneye-native/pull/19)),
  toggle available without the developer overlay ([#20](https://github.com/seb-patron/goldeneye-native/pull/20)),
  read-only session handlers ([#21](https://github.com/seb-patron/goldeneye-native/pull/21)),
  a runtime gibs command ([#22](https://github.com/seb-patron/goldeneye-native/pull/22)),
  explicit-slot player mutations ([#23](https://github.com/seb-patron/goldeneye-native/pull/23)),
  and controlled mission transitions ([#24](https://github.com/seb-patron/goldeneye-native/pull/24)).
- Windows: ROM-free one-click setup and a custom launcher ([#54](https://github.com/seb-patron/goldeneye-native/pull/54)).
- macOS: application bundles that open the custom launcher ([#51](https://github.com/seb-patron/goldeneye-native/pull/51)),
  and a saved OpenGL/Metal renderer choice ([#67](https://github.com/seb-patron/goldeneye-native/pull/67)).
- Metal renderer: three-point texture filtering ([#1](https://github.com/seb-patron/goldeneye-native/pull/1))
  and depth-tested blob shadows ([#3](https://github.com/seb-patron/goldeneye-native/pull/3)).
- Modern mouse look with a classic fallback (`e1a52f6`).
- Launcher Developer Tools with local telemetry reports ([#58](https://github.com/seb-patron/goldeneye-native/pull/58)).
- Versioned `PropRecord` allocator telemetry ([#55](https://github.com/seb-patron/goldeneye-native/pull/55)).
- Content-free semantic-row exporter ([#29](https://github.com/seb-patron/goldeneye-native/pull/29)).
- Standard Claude skill entrypoints and native permission prompts ([#56](https://github.com/seb-patron/goldeneye-native/pull/56)).

### Changed

- Launcher modes clarified; Brutal effects are opt-in and Base Game mission selection is enforced
  ([#66](https://github.com/seb-patron/goldeneye-native/pull/66)).
- Mouse input stays active alongside a connected controller instead of being taken away
  ([#57](https://github.com/seb-patron/goldeneye-native/pull/57)).
- Explicit save directories are isolated and diagnostic handoffs are retained durably
  ([#59](https://github.com/seb-patron/goldeneye-native/pull/59)).
- Transformed asset arrays are generated locally rather than carried in the tree
  ([#28](https://github.com/seb-patron/goldeneye-native/pull/28)).
- CI privilege boundary and merge-review baseline hardened
  ([#47](https://github.com/seb-patron/goldeneye-native/pull/47),
  [#48](https://github.com/seb-patron/goldeneye-native/pull/48)).
- The PR-preparation skill requires regression tests to actually run in CI
  ([#34](https://github.com/seb-patron/goldeneye-native/pull/34)); visual fixes require before/after
  screenshots and skill changes require committed behavioral evals
  ([#65](https://github.com/seb-patron/goldeneye-native/pull/65)).
- The public-artifact guard now catches generated asset arrays that carry no braces
  ([#60](https://github.com/seb-patron/goldeneye-native/pull/60)).

### Fixed

- `GETV_FILTERING` environment precedence ([#4](https://github.com/seb-patron/goldeneye-native/pull/4)).
- The file-level widescreen setting is applied ([#5](https://github.com/seb-patron/goldeneye-native/pull/5)).
- Runway multi-ammo crate halfword order on little-endian hosts
  ([#31](https://github.com/seb-patron/goldeneye-native/pull/31), issue
  [#30](https://github.com/seb-patron/goldeneye-native/issues/30)).
- Native collision hull candidates for coincident extrema
  ([#33](https://github.com/seb-patron/goldeneye-native/pull/33), issue
  [#32](https://github.com/seb-patron/goldeneye-native/issues/32)).
- Native cuff switch indexing and six-entry bounds
  ([#44](https://github.com/seb-patron/goldeneye-native/pull/44), issue
  [#43](https://github.com/seb-patron/goldeneye-native/issues/43)).
- Mouse capture is restored when clicking back into the game after Escape
  ([#50](https://github.com/seb-patron/goldeneye-native/pull/50), issue
  [#49](https://github.com/seb-patron/goldeneye-native/issues/49)).
- Fast mouse camera movement with modern mouse look ([#64](https://github.com/seb-patron/goldeneye-native/pull/64)).
- Auto-crouch across render-only frames (`a972003`).

### Documentation

- Upstream deletion and maintenance status explained ([#2](https://github.com/seb-patron/goldeneye-native/pull/2)).
- Community continuation workflow defined ([#6](https://github.com/seb-patron/goldeneye-native/pull/6)).
- Safe agent contribution workflows ([#8](https://github.com/seb-patron/goldeneye-native/pull/8)).
- Developer console, inspector and diagnostics plan ([#9](https://github.com/seb-patron/goldeneye-native/pull/9)).
- Player, controls and contributor guides ([#10](https://github.com/seb-patron/goldeneye-native/pull/10)).
- Installation walkthroughs aligned ([#15](https://github.com/seb-patron/goldeneye-native/pull/15)).
- Public project site: accurate licensing and provenance language, a documented versioning and
  archive model, this changelog, and beginner installation, launcher, settings, FAQ, agent-help,
  bugs and roadmap pages (milestone
  [Public Project Site, Release Documentation & Contributor Onboarding](https://github.com/seb-patron/goldeneye-native/milestone/5)).

[Unreleased]: https://github.com/seb-patron/goldeneye-native/commits/main

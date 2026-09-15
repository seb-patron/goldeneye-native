# Release readiness

This document is the **whether**: the gates that must be complete before publishing automation is
built at all. [`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md) is the **how**: the step-by-step
procedure for cutting a version, updating the public site and its archived documentation, verifying
the deployment, and repairing it if the deployment fails. Neither document declares the gates below
met, and following the checklist does not bypass them.

There is no official Windows release automation yet. The current Windows workflow has read-only
repository permissions for pull requests and manual dispatches: it builds and tests a setup
candidate inside the runner workspace but does not upload an artifact or publish a GitHub Release.
Pushing a tag does not automatically run that workflow.

## What may be released

The future Windows release asset should be
`GoldenEye-Native-Windows-Setup-vMAJOR.MINOR.PATCH.zip`. It contains the ROM-free setup app, its
checksum, first-run instructions, and required third-party notices. The setup app is not the game:
it asks the player for their own supported cartridge dump and creates the playable
`goldeneye.exe` only inside the installation folder they choose.

Never attach a playable `goldeneye.exe`, a ROM, a save, `base.zip`, extracted assets, generated
asset source, texture dumps, or audio banks to a tag, Actions run, or GitHub Release. The same
boundary applies to future macOS release packaging: a Mac download may contain only the ROM-free
client/setup surface and reviewed redistributable dependencies; its locally generated playable
game remains private.

## Required follow-up

Implement publishing in a focused pull request only after all of these gates are complete:

1. Coordinate the macOS launcher package so one version has matching Windows and macOS client
   behavior and both platform assets can pass before publication.
2. Complete the clean-machine Windows checklist in
   [`WINDOWS_PACKAGING.md`](WINDOWS_PACKAGING.md) and record the exact candidate commit.
3. Review the unresolved licensing questions and make an explicit code-signing decision.
4. Give the release workflow the minimum write permission only in the final publishing job;
   validation jobs must remain read-only and must not hand privileged jobs mutable PR output.
5. Trigger official publication only for exact stable `vMAJOR.MINOR.PATCH` tags. Other `v*` tags,
   including prereleases, must skip cleanly rather than make the workflow fail red.
6. Build every setup client from the exact immutable commit selected by the tag and embed that
   commit identity as its source ref. Do not embed a mutable branch name or rely on a moving tag
   name when cloning the source.
7. Fetch and verify the release toolchain from its pinned sources instead of restoring the
   pull-request toolchain cache for a release build.
8. Publish the platform packages and their checksums only after every build and safety check
   succeeds. Do not move or reuse a published version tag.

Official Actions must remain pinned to full commit SHAs. The follow-up should document the exact
artifact handoff and permission boundary in [`CI_TRUST.md`](CI_TRUST.md) before it is enabled.

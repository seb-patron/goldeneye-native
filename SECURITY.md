# Security policy

## What is supported

There are no tagged releases yet. `main` is the only supported line, and a fix means a commit
on `main`. If you are running anything else, update before reporting.

## Reporting

Use GitHub's private vulnerability reporting on this repository: **Security** then **Report a
vulnerability**. That keeps the report private until there is a fix. Please do not open a public
issue for anything exploitable.

This is a one-person project, so be realistic about timing. Expect an acknowledgement within a
few days and a fix when there is one, not on a schedule.

## What is in scope

The exposed surface is not the game itself but everything that runs on your machine to
produce it:

- `tools/fetch-thirdparty.sh`, which downloads the fifteen port-layer sources
- `tools/setup-mac.sh` and the build scripts, which execute on a developer's machine
- the patches under `getv/patches/`, which are applied to a checkout you did not write
- anything that parses a file the game reads, including save files and configuration

A path that lets a repository, a patch, a downloaded archive or a crafted save file run code, or
write outside the tree, is in scope and worth reporting.

## What is not

The game's own faults are bugs, not vulnerabilities: crashes, hangs, wrong colours, stages that
will not load. Report those as normal issues. GoldenEye is a 1997 game reading data written in
1997, and it is not hardened against a hostile ROM. If you feed it a modified ROM and it
misbehaves, that is expected.

## Do not send game data

Never attach a ROM, a `base.zip`, or anything extracted from either to a report, public or
private. That includes `.bin` blobs and generated asset sources. A crash log, a backtrace, a
stage id and the steps to reach it are what is useful. Reports carrying game data will be
deleted unread, which helps nobody.

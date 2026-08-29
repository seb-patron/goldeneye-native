#!/usr/bin/env bash
# Fetch the third-party port-layer sources this repository does not ship.
#
# Why this script exists
# ----------------------
# Goldeneye-Native's Fast3D renderer and its software audio mixer are inherited from sm64ex,
# which took the renderer from Emill/n64-fast3d-engine. That engine has never been under
# a settled permissive licence: its own notice, in the form sm64ex reproduces, forbids
# redistribution in binary form outright, and GitHub classifies the repository
# NOASSERTION. sm64ex itself ships no root licence at all.
#
# Rather than vendor code whose redistribution terms are unresolved, this repository
# keeps only its own work: the changes Goldeneye-Native made to those files, as a patch. The
# unmodified upstream text is fetched from upstream, by you, at a pinned commit. This is
# the same "bring your own" arrangement the ROM is already under.
#
# See docs/THIRD_PARTY.md for the full account, and getv/port/PROVENANCE.md for the
# licence history that led to it.
#
# What it does
# ------------
#   1. Obtains sm64ex at the pinned commit (reusing vendor/sm64ex if it already has it,
#      otherwise making a bare depth-1 clone in vendor/sm64ex-cache.git).
#   2. Copies the files listed in getv/patches/thirdparty/MANIFEST into place, reading
#      them out of git rather than a working tree so a dirty checkout cannot leak in.
#   3. Applies getv/patches/thirdparty/0001-getv-port-layer.patch, which carries every
#      Goldeneye-Native change to those files.
#
# After it finishes, ./getv/build_mac.sh all builds exactly as it did before the files
# were removed. Nothing here is optional and nothing here is stubbed: the patch is a
# zero-context diff that reproduces the previous working tree byte for byte, which
# `verify` checks.
#
# usage: tools/fetch-thirdparty.sh {fetch|verify|regen|clean|status}
#
#   fetch   (default) fetch, copy and patch. Refuses to clobber locally modified files
#           unless --force is given.
#   verify  check that the files currently on disk are exactly pristine + patch.
#   regen   regenerate the patch from the current working tree. Run this after editing
#           any file listed in the MANIFEST, or the change is not recorded anywhere.
#   clean   remove the fetched files again (returns the tree to its published state).
#   status  print the pin, the manifest and which files are present.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

UPSTREAM_URL="https://github.com/sm64pc/sm64ex.git"
UPSTREAM_SHA="d7ca2c04364a6dd0dac58b47151e04e26887e6f0"

MANIFEST="$ROOT/getv/patches/thirdparty/MANIFEST"
PATCHFILE="$ROOT/getv/patches/thirdparty/0001-getv-port-layer.patch"
CACHE="${GETV_SM64EX_CACHE:-$ROOT/vendor/sm64ex-cache.git}"
REUSE="$ROOT/vendor/sm64ex"

FORCE=0
for a in "$@"; do [ "$a" = "--force" ] && FORCE=1; done

die() { echo "fetch-thirdparty: $*" >&2; exit 1; }

# Reads the manifest as "<upstream path> <destination path>" pairs, comments stripped.
manifest() { grep -vE '^[[:space:]]*(#|$)' "$MANIFEST"; }

# ------------------------------------------------------------------ upstream checkout
# Two ways to reach the pinned commit, preferring the one that costs nothing. A bare
# clone is used for the fallback because nothing here needs a working tree, and because
# vendor/sm64ex belongs to the sm64tv target -- this script must never write into it.
resolve_repo() {
  # GETV_SM64EX_REPO points at an existing clone or mirror and suppresses every other
  # lookup, including the network. Set it on an offline machine, or to test this script
  # against a checkout other than the one that happens to be in vendor/.
  if [ -n "${GETV_SM64EX_REPO:-}" ]; then
    git -C "$GETV_SM64EX_REPO" cat-file -e "${UPSTREAM_SHA}^{commit}" 2>/dev/null \
      || die "GETV_SM64EX_REPO=$GETV_SM64EX_REPO does not contain $UPSTREAM_SHA"
    echo "$GETV_SM64EX_REPO"; return 0
  fi
  if [ -d "$REUSE/.git" ] || [ -d "$REUSE/objects" ]; then
    if git -C "$REUSE" cat-file -e "${UPSTREAM_SHA}^{commit}" 2>/dev/null; then
      echo "$REUSE"; return 0
    fi
  fi
  if [ -d "$CACHE" ] && git -C "$CACHE" cat-file -e "${UPSTREAM_SHA}^{commit}" 2>/dev/null; then
    echo "$CACHE"; return 0
  fi
  echo "fetching $UPSTREAM_URL @ $UPSTREAM_SHA" >&2
  mkdir -p "$(dirname "$CACHE")"
  if [ ! -d "$CACHE" ]; then
    git init --bare -q "$CACHE" || return 1
    git -C "$CACHE" remote add origin "$UPSTREAM_URL" || return 1
  fi
  # Fetching a bare SHA rather than a branch: GitHub allows it, and it means the pin
  # keeps resolving even if upstream's default branch moves or the repo is archived.
  git -C "$CACHE" fetch -q --depth 1 origin "$UPSTREAM_SHA" 2>/dev/null \
    || git -C "$CACHE" fetch -q --depth 1 origin +HEAD:refs/remotes/origin/HEAD \
    || return 1
  git -C "$CACHE" cat-file -e "${UPSTREAM_SHA}^{commit}" 2>/dev/null || return 1
  echo "$CACHE"
}

# Writes the pristine upstream tree, laid out under this repo's destination paths.
export_pristine() {
  local repo="$1" dest="$2" up dst
  manifest | while read -r up dst; do
    mkdir -p "$dest/$(dirname "$dst")"
    git -C "$repo" show "$UPSTREAM_SHA:$up" > "$dest/$dst" \
      || die "upstream $up missing at $UPSTREAM_SHA"
  done
}

apply_patch() {
  local dir="$1"
  [ -f "$PATCHFILE" ] || die "missing $PATCHFILE"
  ( cd "$dir" && patch -p1 -s -i "$PATCHFILE" ) || return 1
}

# ------------------------------------------------------------------------------ fetch
cmd_fetch() {
  local repo tmp dst
  repo="$(resolve_repo)" || die "could not obtain sm64ex at $UPSTREAM_SHA (network?)"
  echo "upstream: $repo @ $UPSTREAM_SHA"

  if [ "$FORCE" -eq 0 ]; then
    while read -r _ dst; do
      [ -e "$ROOT/$dst" ] && die "$dst already exists; run 'clean' first, or pass --force"
    done < <(manifest)
  fi

  tmp="$(mktemp -d)" || die "mktemp"
  trap 'rm -rf "$tmp"' RETURN
  export_pristine "$repo" "$tmp/work" || return 1
  apply_patch "$tmp/work" || die "patch did not apply -- upstream pin and patch disagree"

  manifest | while read -r _ dst; do
    mkdir -p "$ROOT/$(dirname "$dst")"
    cp "$tmp/work/$dst" "$ROOT/$dst"
    echo "  $dst"
  done
  echo "fetch-thirdparty: $(manifest | wc -l | tr -d ' ') files in place"
}

# ----------------------------------------------------------------------------- verify
# Rebuilds pristine + patch in a scratch directory and compares byte for byte. This is
# the check that the patch really does carry every local change: if anything in the
# working tree is not represented in the patch, it shows up here.
cmd_verify() {
  local repo tmp dst rc=0 n=0
  repo="$(resolve_repo)" || die "could not obtain sm64ex at $UPSTREAM_SHA (network?)"
  tmp="$(mktemp -d)" || die "mktemp"
  trap 'rm -rf "$tmp"' RETURN
  export_pristine "$repo" "$tmp/work" || return 1
  apply_patch "$tmp/work" || die "patch did not apply against $UPSTREAM_SHA"
  while read -r _ dst; do
    n=$((n+1))
    if [ ! -e "$ROOT/$dst" ]; then echo "MISSING  $dst"; rc=1
    elif cmp -s "$tmp/work/$dst" "$ROOT/$dst"; then echo "ok       $dst"
    else echo "DIFFERS  $dst"; rc=1; fi
  done < <(manifest)
  [ "$rc" -eq 0 ] && echo "fetch-thirdparty: $n/$n files match pristine + patch"
  return $rc
}

# ------------------------------------------------------------------------------ regen
cmd_regen() {
  local repo tmp dst
  repo="$(resolve_repo)" || die "could not obtain sm64ex at $UPSTREAM_SHA (network?)"
  tmp="$(mktemp -d)" || die "mktemp"
  trap 'rm -rf "$tmp"' RETURN
  export_pristine "$repo" "$tmp/a" || return 1
  while read -r _ dst; do
    [ -e "$ROOT/$dst" ] || die "$dst is not present; nothing to regenerate from"
    mkdir -p "$tmp/b/$(dirname "$dst")"
    cp "$ROOT/$dst" "$tmp/b/$dst"
  done < <(manifest)
  # -u0: zero context. The patch is applied to an exact pinned commit, so no context is
  # needed to place the hunks, and omitting it keeps unmodified upstream lines out of a
  # file this repository does distribute.
  #
  # -u0 AND NOT -U0, WHICH IS NOT THE SAME THING HERE. GNU diffutils 3.12 ignores an attached
  # -U0 and emits three lines of context anyway; -U 0 and --unified=0 do too. Only the old
  # lowercase spelling still means zero. The consequence is not a failure, which is what makes
  # it worth this paragraph: regen keeps working, cmd_verify still passes 15/15, and the patch
  # is simply rewritten in a different format -- a three-line edit came back as a 1,917-line
  # diff, with every real change buried in reformatting nobody can review. Checked against
  # diffutils 3.12; the committed patch's `@@ -4,0 +5,3 @@` hunks are what -u0 produces.
  #
  # WRITE TO A TEMPORARY AND MOVE ONLY ON SUCCESS. This used to redirect straight into
  # $PATCHFILE, and `>` TRUNCATES BEFORE THE SUBSHELL RUNS -- so any failure inside destroyed the
  # single file carrying every change this project has made to the fifteen third-party sources.
  #
  # Not hypothetical: it happened here. Under MSYS the mkdir/cp forks above died with cygheap
  # `child_copy` errors, so $tmp/b was empty, diff had nothing to compare, and a 363,467-byte
  # patch became 0 bytes. The sources themselves survived only because they are gitignored and
  # sat untouched in the working tree; the patch came back only because it IS tracked. Had both
  # been regenerated together the project would have lost the lot.
  #
  # cmd_verify below did report DIFFERS on every file -- the signal existed, but it arrived after
  # the destruction rather than before it.
  local out="$tmp/patch.new"
  if ! ( cd "$tmp" && diff -ruN -u0 a b ) > "$out"; then
    # diff exits 1 when files differ, which is the NORMAL case here and not an error. Only a
    # status above 1 is real trouble.
    [ $? -gt 1 ] && die "diff failed; $PATCHFILE left untouched"
  fi

  # REFUSE AN EMPTY OR IMPLAUSIBLY SMALL RESULT. Fifteen heavily modified files cannot diff to
  # nothing, so an empty patch means the comparison did not happen -- not that the changes went
  # away. Overwriting a good patch with that is the failure this whole block exists to prevent.
  local newsz oldsz
  newsz=$(wc -c < "$out" | tr -d ' ')
  oldsz=0; [ -f "$PATCHFILE" ] && oldsz=$(wc -c < "$PATCHFILE" | tr -d ' ')
  if [ "$newsz" -eq 0 ]; then
    die "regenerated patch is EMPTY; $PATCHFILE left untouched at $oldsz bytes"
  fi
  # A large shrink is legitimate when changes are genuinely reverted, so this warns and asks
  # rather than refusing -- but it must never be silent.
  if [ "$oldsz" -gt 0 ] && [ "$newsz" -lt $(( oldsz / 2 )) ] && [ "${FORCE:-0}" != "1" ]; then
    die "regenerated patch is $newsz bytes against $oldsz -- less than half. Refusing. Re-run with FORCE=1 if this shrink is intended."
  fi

  # REFUSE A PATCH THAT DELETES A MANIFEST FILE. The size check above only catches a result
  # that is implausibly SMALL, so losing exactly one file of fifteen sails straight through it.
  #
  # That is not a hypothetical either. Reported from the Windows lane: under the same MSYS fork
  # failures described above, one file's copy into $tmp/b never happened, and because diff -N
  # treats an absent right-hand file as a deletion, the regenerated patch recorded ge_mixer.h
  # with an epoch-zero timestamp and a pure-deletion hunk. Every real change to that file would
  # have been replaced by an instruction to delete it, and the patch was otherwise the right
  # size and shape.
  #
  # A regen must never delete a manifest file, so a deletion hunk is always a dropped copy
  # rather than an intended change. Checked by name, so the message says which one.
  # A file that is simply unmodified produces no hunk at all, which is normal and is NOT what
  # this looks for. The signature of a dropped copy is a whole-file deletion: diff -N treats an
  # absent right-hand file as one, so the hunk header reads `+0,0` and the +++ timestamp is the
  # epoch. Neither can ever be a legitimate regen result, because a manifest file that is gone
  # from the working tree would have stopped this function at the `is not present` check above.
  local deleted
  deleted=$(awk '/^\+\+\+ b\//{f=substr($2,3)} /^@@ .* \+0,0 @@/{if(f!="")print f}' "$out" | sort -u | tr '\n' ' ')
  if [ -n "$deleted" ]; then
    die "regenerated patch DELETES: $deleted -- a copy into the temp tree did not happen. $PATCHFILE left untouched. Re-run; this is transient under MSYS."
  fi

  mv -f "$out" "$PATCHFILE"
  echo "fetch-thirdparty: wrote $PATCHFILE ($newsz bytes, was $oldsz)"
  cmd_verify
}

# ------------------------------------------------------------------------------ clean
cmd_clean() {
  local dst
  while read -r _ dst; do
    [ -e "$ROOT/$dst" ] && { rm -f "$ROOT/$dst"; echo "  removed $dst"; }
  done < <(manifest)
  echo "fetch-thirdparty: tree is back to its published state"
}

# ----------------------------------------------------------------------------- status
cmd_status() {
  local up dst
  echo "upstream : $UPSTREAM_URL"
  echo "commit   : $UPSTREAM_SHA"
  echo "patch    : $PATCHFILE"
  echo
  while read -r up dst; do
    printf "  %-8s %-36s <- %s\n" "$([ -e "$ROOT/$dst" ] && echo present || echo ABSENT)" "$dst" "$up"
  done < <(manifest)
}

case "${1:-fetch}" in
  fetch|--force) cmd_fetch ;;
  verify)        cmd_verify ;;
  regen)         cmd_regen ;;
  clean)         cmd_clean ;;
  status)        cmd_status ;;
  *) sed -n '/^# usage:/,/^set -uo/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//; $d' ; exit 1 ;;
esac

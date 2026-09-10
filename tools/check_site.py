#!/usr/bin/env python3
"""Validate the public project site: markup, links, assets, version metadata and changelog.

ROM-free by construction. This script reads only `site/`, `site/versions.json` and
`CHANGELOG.md`; it never needs a ROM, extracted assets or a built game, and it never
opens a network connection.

It is deliberately separate from tools/check_no_game_data.py, which is the publication
guard for game data. Run both before publishing anything.

    python3 tools/check_site.py                 # validate the site as it stands
    python3 tools/check_site.py --release v1.0.0  # release-readiness for a proposed version
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "site"
MANIFEST = SITE / "versions.json"
CHANGELOG = ROOT / "CHANGELOG.md"

# Block elements whose open and close counts must match. Elements that HTML allows to be
# closed implicitly (p, li, td, th) are deliberately absent: an unclosed one is legal and
# a false failure here would train people to ignore this script.
BALANCED_TAGS = (
    "html", "head", "body", "main", "section", "nav", "footer", "header",
    "details", "summary", "table", "thead", "tbody", "tr", "div", "ul", "ol",
    "figure", "figcaption",
)

LINK_RE = re.compile(r'(?:href|src)="([^"]+)"')
SCRIPT_SRC_RE = re.compile(r'<script\b[^>]*\bsrc="([^"]+)"', re.IGNORECASE)
LINK_HREF_RE = re.compile(r'<link\b[^>]*\bhref="([^"]+)"', re.IGNORECASE)
CSS_URL_RE = re.compile(r'(?:@import\s+|url\()\s*["\']?(https?://[^"\')\s]+)', re.IGNORECASE)
TIME_RE = re.compile(r'<time datetime="(\d{4}-\d{2}-\d{2})"')
SEMVER_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")
EXTERNAL_RE = re.compile(r"^(?:https?:)?//|^mailto:|^data:", re.IGNORECASE)

# A public page must never point at game data, whatever it is called.
FORBIDDEN_LINK_SUFFIXES = (
    ".z64", ".n64", ".v64", ".rom", ".sav", ".save", ".srm", ".eep", ".bin", ".iso", ".img",
)
FORBIDDEN_LINK_NAMES = ("base.zip", "baserom", "eeprom.bin")

# Anything that would make a viewer's browser talk to a third party.
TRACKER_HINTS = (
    "google-analytics", "googletagmanager", "gtag/js", "doubleclick", "facebook.net",
    "hotjar", "segment.io", "mixpanel", "plausible.io", "matomo", "sentry.io",
    "fonts.googleapis.com", "fonts.gstatic.com", "cdn.jsdelivr.net", "unpkg.com",
    "cdnjs.cloudflare.com",
)


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT)).replace("\\", "/")
    except ValueError:
        return str(path)


def load_manifest(problems: list[str]) -> dict | None:
    if not MANIFEST.exists():
        problems.append("site/versions.json is missing")
        return None
    try:
        data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        problems.append("site/versions.json could not be read: %s" % exc)
        return None
    if data.get("schema") != 1:
        problems.append("site/versions.json: unsupported schema %r" % (data.get("schema"),))
    versions = data.get("versions")
    if not isinstance(versions, list) or not versions:
        problems.append("site/versions.json: 'versions' must be a non-empty list")
        return None
    ids = [v.get("id") for v in versions]
    if len(set(ids)) != len(ids):
        problems.append("site/versions.json: duplicate version ids %r" % (ids,))
    if data.get("current") not in ids:
        problems.append(
            "site/versions.json: current %r is not one of %r" % (data.get("current"), ids)
        )
    for version in versions:
        for field in ("id", "label", "status", "path", "source_ref", "last_reviewed"):
            if not version.get(field):
                problems.append(
                    "site/versions.json: version %r is missing %r" % (version.get("id"), field)
                )
    return data


def version_dir(version: dict) -> Path:
    path = version.get("path") or "."
    if path in (".", "", "/"):
        return SITE
    return (SITE / path).resolve()


def check_manifest_paths(data: dict, problems: list[str]) -> None:
    """Every manifest entry resolves, and every archived directory is in the manifest."""
    for version in data["versions"]:
        directory = version_dir(version)
        if not directory.is_dir():
            problems.append(
                "site/versions.json: version %r points at missing directory %s"
                % (version.get("id"), rel(directory))
            )
            continue
        if not (directory / "index.html").is_file():
            problems.append(
                "version %r has no index.html at %s" % (version.get("id"), rel(directory))
            )

    archive_root = SITE / (data.get("archive_root") or "v")
    if not archive_root.is_dir():
        problems.append("archive root %s does not exist" % rel(archive_root))
        return
    known = {v.get("id") for v in data["versions"]}
    for entry in sorted(archive_root.iterdir()):
        if not entry.is_dir():
            continue
        if entry.name not in known:
            problems.append(
                "archived directory %s is not listed in site/versions.json" % rel(entry)
            )


def check_changelog(data: dict, problems: list[str]) -> None:
    if not CHANGELOG.exists():
        problems.append("CHANGELOG.md is missing")
        return
    text = CHANGELOG.read_text(encoding="utf-8")
    for version in data["versions"]:
        anchor = version.get("changelog_anchor")
        if not anchor:
            problems.append("version %r has no changelog_anchor" % (version.get("id"),))
            continue
        if anchor.lower() == "unreleased":
            pattern = r"^##\s*\[Unreleased\]"
        else:
            pattern = r"^##\s*\[%s\]" % re.escape(anchor)
        if not re.search(pattern, text, re.IGNORECASE | re.MULTILINE):
            problems.append(
                "CHANGELOG.md has no section for version %r (looked for %s)"
                % (version.get("id"), pattern)
            )
        if version.get("released") and anchor.lower() == "unreleased":
            problems.append(
                "version %r has a release date but points at the Unreleased section"
                % (version.get("id"),)
            )


def html_files() -> list[Path]:
    files = sorted(SITE.glob("*.html"))
    archive = SITE / "v"
    if archive.is_dir():
        files.extend(sorted(archive.glob("**/*.html")))
    return files


def owning_version(path: Path, data: dict) -> dict:
    """Which manifest entry a page belongs to, by directory."""
    for version in data["versions"]:
        directory = version_dir(version)
        try:
            path.resolve().relative_to(directory)
        except ValueError:
            continue
        if directory != SITE:
            return version
    for version in data["versions"]:
        if version.get("id") == data.get("current"):
            return version
    return data["versions"][0]


def check_balance(path: Path, text: str, problems: list[str]) -> None:
    for tag in BALANCED_TAGS:
        opened = len(re.findall(r"<%s(?=[\s>])" % tag, text, re.IGNORECASE))
        closed = len(re.findall(r"</%s>" % tag, text, re.IGNORECASE))
        if opened != closed:
            problems.append(
                "%s: <%s> opened %d times, closed %d times" % (rel(path), tag, opened, closed)
            )


def check_links(path: Path, text: str, pages: dict[Path, str], problems: list[str]) -> None:
    for target in LINK_RE.findall(text):
        if EXTERNAL_RE.match(target) or target.startswith("#"):
            if target.startswith("#"):
                anchor = target[1:]
                if anchor and ('id="%s"' % anchor) not in text:
                    problems.append("%s: no element with id %r" % (rel(path), anchor))
            continue
        file_part, _, anchor = target.partition("#")
        if not file_part:
            continue
        lowered = file_part.lower()
        if lowered.endswith(FORBIDDEN_LINK_SUFFIXES) or any(
            name in lowered for name in FORBIDDEN_LINK_NAMES
        ):
            problems.append("%s: links to prohibited game data %r" % (rel(path), target))
            continue
        resolved = (path.parent / file_part).resolve()
        if not resolved.exists():
            problems.append("%s: broken link %r" % (rel(path), target))
            continue
        if anchor and resolved.suffix.lower() == ".html":
            target_text = pages.get(resolved)
            if target_text is None:
                try:
                    target_text = resolved.read_text(encoding="utf-8")
                except OSError:
                    target_text = ""
            if ('id="%s"' % anchor) not in target_text:
                problems.append("%s: %r has no element with id %r" % (rel(path), file_part, anchor))


def check_no_external_requests(path: Path, text: str, problems: list[str]) -> None:
    for src in SCRIPT_SRC_RE.findall(text) + LINK_HREF_RE.findall(text):
        if EXTERNAL_RE.match(src):
            problems.append("%s: loads an external resource %r" % (rel(path), src))
    lowered = text.lower()
    for hint in TRACKER_HINTS:
        if hint in lowered:
            problems.append("%s: references third-party host %r" % (rel(path), hint))


def check_metadata(path: Path, text: str, version: dict, data: dict, problems: list[str]) -> None:
    required = (
        ('class="skip-link"', "a skip link"),
        ('<main id="main">', "a <main id=\"main\"> landmark"),
        ('class="version-picker"', "the version selector"),
        ('class="doc-meta"', "the version metadata line"),
        ('aria-label="Primary"', "a labelled primary navigation"),
    )
    for needle, description in required:
        if needle not in text:
            problems.append("%s: missing %s" % (rel(path), description))

    label = version.get("label") or version.get("id")
    if ("Docs version <strong>%s</strong>" % label) not in text:
        problems.append("%s: version bar does not name version %r" % (rel(path), label))

    source_ref = version.get("source_ref") or ""
    short = version.get("source_ref_short") or source_ref[:7]
    if source_ref not in text and short not in text:
        problems.append("%s: does not name its source ref %r" % (rel(path), short))

    reviewed = version.get("last_reviewed")
    dates = TIME_RE.findall(text)
    if not dates:
        problems.append("%s: has no <time datetime=...> last-reviewed date" % rel(path))
    elif reviewed and reviewed not in dates:
        problems.append(
            "%s: last-reviewed dates %r do not include the manifest's %r"
            % (rel(path), dates, reviewed)
        )

    menu = re.search(r'<div class="version-menu">(.*?)</div>', text, re.DOTALL)
    if menu is None:
        problems.append("%s: version selector has no menu" % rel(path))
        return
    body = menu.group(1)
    for other in data["versions"]:
        other_label = other.get("label") or other.get("id")
        if other_label not in body:
            problems.append(
                "%s: version selector omits %r" % (rel(path), other_label)
            )


def check_css(problems: list[str]) -> None:
    for css in sorted((SITE / "assets").glob("**/*.css")):
        text = css.read_text(encoding="utf-8")
        for url in CSS_URL_RE.findall(text):
            problems.append("%s: loads an external resource %r" % (rel(css), url))


def check_release(version_id: str, data: dict, problems: list[str]) -> None:
    """Release-readiness for a proposed version, before it is tagged."""
    match = SEMVER_RE.match(version_id)
    if match is None:
        problems.append(
            "release %r is not vMAJOR.MINOR.PATCH; see docs/RELEASE_CHECKLIST.md" % version_id
        )
        return
    major, minor, patch = (int(part) for part in match.groups())

    text = CHANGELOG.read_text(encoding="utf-8") if CHANGELOG.exists() else ""
    if not re.search(r"^##\s*\[%s\]" % re.escape(version_id), text, re.MULTILINE):
        problems.append("CHANGELOG.md has no '## [%s]' section" % version_id)

    entry = next((v for v in data["versions"] if v.get("id") == version_id), None)
    if entry is None:
        problems.append("site/versions.json has no entry for %s" % version_id)
        return
    if data.get("current") != version_id:
        problems.append("site/versions.json 'current' is not %s" % version_id)
    if entry.get("source_ref_kind") != "tag":
        problems.append("version %s must record source_ref_kind 'tag'" % version_id)
    if not entry.get("released"):
        problems.append("version %s has no release date" % version_id)

    for page in sorted(SITE.glob("*.html")):
        page_text = page.read_text(encoding="utf-8")
        label = entry.get("label") or version_id
        if ("Docs version <strong>%s</strong>" % label) not in page_text:
            problems.append(
                "%s still does not name %s in its version bar" % (rel(page), label)
            )

    is_major = minor == 0 and patch == 0
    if is_major:
        snapshot = SITE / (data.get("archive_root") or "v") / version_id
        if not (snapshot / "index.html").is_file():
            problems.append(
                "%s is a major release and needs an archived snapshot at %s"
                % (version_id, rel(snapshot))
            )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--release",
        metavar="VERSION",
        help="also check release readiness for a proposed vMAJOR.MINOR.PATCH version",
    )
    args = parser.parse_args(argv)

    problems: list[str] = []

    if not SITE.is_dir():
        print("site/ not found at %s" % rel(SITE), file=sys.stderr)
        return 1

    data = load_manifest(problems)
    if data is None:
        for problem in problems:
            print("FAIL  %s" % problem, file=sys.stderr)
        return 1

    check_manifest_paths(data, problems)
    check_changelog(data, problems)
    check_css(problems)

    files = html_files()
    if not files:
        problems.append("site/ contains no HTML pages")
    pages = {path.resolve(): path.read_text(encoding="utf-8") for path in files}

    for path in files:
        text = pages[path.resolve()]
        check_balance(path, text, problems)
        check_links(path, text, pages, problems)
        check_no_external_requests(path, text, problems)
        check_metadata(path, text, owning_version(path, data), data, problems)

    if args.release:
        check_release(args.release, data, problems)

    if problems:
        for problem in problems:
            print("FAIL  %s" % problem, file=sys.stderr)
        print(
            "\n%d problem(s). See docs/RELEASE_CHECKLIST.md." % len(problems), file=sys.stderr
        )
        return 1

    print("check_site: %d page(s), %d version(s), no problems." % (len(files), len(data["versions"])))
    if args.release:
        print(
            "check_site: %s looks release-ready. This check is ADVISORY: GitHub cannot block a\n"
            "            release that skips it, so a human must run it before tagging."
            % args.release
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

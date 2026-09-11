#!/usr/bin/env python3
"""Compile the real joy/queue functions in a ROM-free schedule regression."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_JOY_SOURCE = ROOT / "vendor/ge-decomp/src/joy.c"
DEFAULT_QUEUE_SOURCE = ROOT / "getv/port/src/port_os.c"
HARNESS = ROOT / "getv/port/tests/game/joy_poll_handshake_harness.c"


def extract_function(source: str, signature: str) -> str:
    """Return one complete C function while ignoring braces in comments/strings."""
    start = source.find(signature)
    if start < 0:
        raise ValueError(f"production function not found: {signature}")
    opening = source.find("{", start + len(signature))
    if opening < 0:
        raise ValueError(f"opening brace not found: {signature}")

    depth = 0
    state = "code"
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""

        if state == "code":
            if char == "/" and following == "*":
                state = "block_comment"
                index += 2
                continue
            if char == "/" and following == "/":
                state = "line_comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1]
        elif state == "block_comment":
            if char == "*" and following == "/":
                state = "code"
                index += 2
                continue
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state in ("string", "character"):
            if char == "\\":
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1

    raise ValueError(f"closing brace not found: {signature}")


def compile_and_run(
    compiler: str,
    directory: Path,
    joy_functions: str,
    queue_functions: str,
    *,
    native: bool,
    expect_safe: bool,
) -> int:
    (directory / "joy_poll_production.inc").write_text(joy_functions + "\n")
    (directory / "port_queue_production.inc").write_text(queue_functions + "\n")
    label = "native-fixed" if native and expect_safe else "native-vulnerable"
    if not native:
        label = "non-native-preserved"
    executable = directory / (label + (".exe" if os.name == "nt" else ""))
    command = [
        compiler,
        "-std=c11",
        "-O1",
        "-g",
        "-Werror=return-type",
        f"-DEXPECT_NATIVE_SAFE={int(expect_safe)}",
        f"-I{directory}",
    ]
    if native:
        command.append("-DGE_PORT_NATIVE")
    command.extend([str(HARNESS), "-o", str(executable)])

    compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr, end="")
        return compiled.returncode

    print(f"== {label} ==")
    result = subprocess.run(
        [str(executable)], capture_output=True, text=True, timeout=10
    )
    print(result.stdout + result.stderr, end="")
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--joy-source", type=Path, default=DEFAULT_JOY_SOURCE)
    parser.add_argument("--queue-source", type=Path, default=DEFAULT_QUEUE_SOURCE)
    parser.add_argument(
        "--expect-native",
        choices=("fixed", "vulnerable"),
        default="fixed",
        help="use vulnerable only for the unchanged negative control",
    )
    args = parser.parse_args()

    compiler = shutil.which(args.cc)
    if compiler is None:
        raise SystemExit(f"compiler not found: {args.cc}")
    if not args.joy_source.is_file():
        raise SystemExit(f"patched decomp source not found: {args.joy_source}")
    if not args.queue_source.is_file():
        raise SystemExit(f"native queue source not found: {args.queue_source}")
    if not HARNESS.is_file():
        raise SystemExit(f"ROM-free harness not found: {HARNESS}")

    joy_source = args.joy_source.read_text()
    queue_source = args.queue_source.read_text()
    joy_signatures = (
        "void joyCheckStatusThreadSafe(void)",
        "void joyPoll(void)",
        "void joyDisablePoll(void)",
        "void joyEnablePoll(void)",
        "s32 joyGamePakLongWrite(u8 address, u8 *buffer, s32 nbytes)",
    )
    joy_functions = "\n\n".join(
        extract_function(joy_source, signature) for signature in joy_signatures
    )

    queue_create = extract_function(
        queue_source, "void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msgBuf, s32 count)"
    )
    queue_send = extract_function(
        queue_source, "s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)"
    ).replace("s32 osSendMesg(", "static s32 productionOsSendMesg(", 1)
    queue_receive = extract_function(
        queue_source, "s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag)"
    ).replace("s32 osRecvMesg(", "static s32 productionOsRecvMesg(", 1)
    queue_functions = "\n\n".join((queue_create, queue_send, queue_receive))

    expect_safe = args.expect_native == "fixed"
    with tempfile.TemporaryDirectory(prefix="ge-joy-poll-") as temporary:
        directory = Path(temporary)
        native_result = compile_and_run(
            compiler,
            directory,
            joy_functions,
            queue_functions,
            native=True,
            expect_safe=expect_safe,
        )
        if native_result:
            return native_result

        if expect_safe:
            return compile_and_run(
                compiler,
                directory,
                joy_functions,
                queue_functions,
                native=False,
                expect_safe=False,
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

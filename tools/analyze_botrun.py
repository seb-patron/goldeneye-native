#!/usr/bin/env python3
"""Summarize a ge_bot_route.c TSV log: final waypoint, health, and the intro-freeze tax.

Reads the per-tick log written by GETV_BOT_LOG (frame, event, node, pad, x, z, heading, bearing,
err, stick_x, stick_y, note) and reports what a run actually achieved, cheaply enough to compare
several runs without opening each by hand.

THE INTRO FREEZE IS MEASURED, NOT ASSUMED. Train's scripted opening leaves the bot's position
accessor frozen for roughly 360 frames before real gameplay position appears -- confirmed via
gePortStateDump (GETV_STATE=1), which shows the authoritative player position sitting at a fixed
placeholder location with g_ControlsLockedFlag reading 0 throughout, meaning it is not the
lock-flag mechanism and not a steering bug. This script detects it the same way: the first run of
consecutive identical (x, z) in the 'post' rows, and reports its length so it can be subtracted
from any "how long did it take" comparison rather than silently inflating one run over another.
"""
import argparse
import csv
import os
import sys


def load(path):
    rows = []
    with open(path, encoding="utf-8") as fh:
        for r in csv.DictReader(fh, delimiter="\t"):
            rows.append(r)
    return rows


def freeze_length(posts):
    if not posts:
        return 0
    x0, z0 = posts[0]["x"], posts[0]["z"]
    n = 0
    for r in posts:
        if r["x"] == x0 and r["z"] == z0:
            n += 1
        else:
            break
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()

    print("%-14s %8s %8s %8s %10s %8s %10s %s"
          % ("run", "frames", "freeze", "waypnt", "final_hp", "dead", "engages", "note"))
    for p in a.paths:
        if not os.path.isfile(p):
            print("%-14s MISSING" % os.path.basename(p))
            continue
        rows = load(p)
        posts = [r for r in rows if r["event"] == "post"]
        engages = [r for r in rows if r["event"] == "engage"]
        waypoints = [r for r in rows if r["event"] == "steer"]

        fz = freeze_length(posts)
        last_post = posts[-1] if posts else None
        dead = "DEAD" in (last_post["note"] if last_post else "")
        hp = "?"
        if last_post:
            for tok in last_post["note"].split(","):
                if tok.startswith("hp="):
                    hp = tok[3:]

        # Waypoint reached: the "reached waypoint N" events don't have their own row in THIS
        # log format (that print is stdout-only), so this counts distinct waypoint targets ("pad"
        # column) the steer event named, which changes each time the bot advances a step.
        wp_seq = []
        for r in waypoints:
            if not wp_seq or wp_seq[-1] != r["node"]:
                wp_seq.append(r["node"])

        name = os.path.basename(p).replace(".tsv", "")
        print("%-14s %8d %8d %8d %10s %8s %10d %s"
              % (name, len(posts), fz, len(wp_seq), hp, dead,
                 len(engages), "%d distinct targets" % len(wp_seq)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

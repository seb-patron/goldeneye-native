#!/usr/bin/env python3
"""Find systems that still count ticks instead of time.

Runs one scripted scenario at several GETV_SIMDIV values and compares every numeric field of
The per-frame gun/state trace AT matched frames, which is matched real time. A system with a
correct time base reads the same at frame N whatever the divider; one that counts iterations
drifts in proportion to the divider, and the drift is the signature.

    tools/divider_audit.py --stage 9 --script "620:Z:240" --frames 900 --give 14

Reports each field's divergence so the next conversion is chosen from a number rather than
from a guess. Fire rate was found and fixed this way; reload and the tank turret were cleared
by it, having been listed as suspects on nothing more than plausibility.
"""
import argparse, os, re, subprocess, sys, collections

BIN = "getv/build-mac/goldeneye"
FIELD = re.compile(r'([a-z_]+)=(-?[\d.]+)')

def run(divider, args):
    env = dict(os.environ)
    env.update({
        "GETV_SIMDIV": str(divider),
        "GETV_GUN_DEBUG": "1",
        "GETV_STAGE": str(args.stage),
        "GETV_EXIT_FRAME": str(args.frames),
    })
    if args.script: env["GETV_SCRIPT"] = args.script
    if args.give:   env["GETV_GIVE"] = str(args.give); env["GETV_GIVE_AMMO"] = "400"
    out = subprocess.run([BIN], env=env, capture_output=True, text=True, timeout=900).stdout
    frames = {}
    for ln in out.splitlines():
        if not ln.startswith("[gun] f="): continue
        m = re.match(r'\[gun\] f=(\d+)', ln)
        if not m: continue
        frames[int(m.group(1))] = {k: float(v) for k, v in FIELD.findall(ln[len(m.group(0)):])}
    return frames

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", type=int, default=9)
    ap.add_argument("--script", default="620:Z:240")
    ap.add_argument("--frames", type=int, default=900)
    ap.add_argument("--give", type=int, default=14)
    ap.add_argument("--dividers", default="1,2")
    args = ap.parse_args()

    divs = [int(d) for d in args.dividers.split(",")]
    runs = {}
    for d in divs:
        sys.stderr.write(f"  running SIMDIV={d} ...\n")
        runs[d] = run(d, args)

    base = runs[divs[0]]
    common = set(base)
    for d in divs[1:]: common &= set(runs[d])
    common = sorted(f for f in common if f > args.frames // 2)
    if not common:
        print("no overlapping frames; raise --frames"); return

    rows = []
    for field in sorted(base[common[0]]):
        vals = {}
        for d in divs:
            vals[d] = [runs[d][f].get(field) for f in common if field in runs[d][f]]
        if any(len(v) == 0 for v in vals.values()): continue
        ref = vals[divs[0]]
        span = max(ref) - min(ref)
        worst, worst_d, worst_mean = 0.0, None, 0.0
        for d in divs[1:]:
            v = vals[d]
            n = min(len(ref), len(v))
            diffs = [abs(ref[i] - v[i]) for i in range(n)]
            peak = max(diffs)
            mean = sum(diffs) / n
            # Mean, not peak, decides the verdict. A single-frame timing offset puts a large
            # value in `peak` on any field that steps, and reading that as quantisation is a
            # false positive -- a real quantised system is wrong on MOST frames, not one.
            rel = mean / span if span > 1e-9 else (0.0 if peak < 1e-9 else float('inf'))
            if rel > worst:
                worst, worst_d, worst_mean = rel, d, peak / span if span > 1e-9 else 0.0
        rows.append((worst, field, span, worst_d, worst_mean))

    rows.sort(reverse=True, key=lambda r: (r[0] if r[0] != float('inf') else 1e18))
    print(f"\nstage {args.stage}, script {args.script!r}, dividers {divs}, "
          f"{len(common)} matched frames\n")
    print(f"{'field':>12} {'range@div1':>12} {'mean drift':>11} {'peak':>8}  verdict")
    for worst, field, span, wd, peak in rows:
        if worst == 0.0:                verdict = "time-correct (identical)"
        elif worst == float('inf'):     verdict = "DIVERGES (constant at div1)"
        elif worst < 0.02:              verdict = "time-correct (<2%)"
        elif worst < 0.15:              verdict = "minor drift / timing offset"
        else:                           verdict = f"FRAME-QUANTISED at div {wd}"
        w = "inf" if worst == float('inf') else f"{100*worst:.1f}%"
        print(f"{field:>12} {span:>12.3f} {w:>11} {100*peak:>7.0f}%  {verdict}")

if __name__ == "__main__":
    main()

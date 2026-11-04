#!/usr/bin/env python3
"""perf_to_json.py — convert one osc_perf*.txt run output into a schema-1 JSON
under perf_reports/<commit-sha7>/<platform>-<layer>-<utc-stamp>.json.

Usage: perf_to_json.py --txt osc_perf_core.txt --repo . \
    --commit <full-sha> --platform windows-latest [--layer auto]

Marker selection: core files contain @@HEDDLE_OSC_PERF@@, host files
@@HEDDLE_OSC_PERF_HOST@@ (auto-detected if --layer omitted).
"""
import argparse, json, os, re, sys, datetime


def convert(txt, repo, commit, platform, layer, dest="perf_reports"):
    raw = open(txt, encoding="utf-8", errors="replace").read()
    m = re.search(r"@@HEDDLE_OSC_PERF(?:_HOST)?@@\r?\n([\s\S]*?)\r?\n@@END@@", raw)
    if not m:
        sys.exit("no machine-readable perf block in %s" % txt)
    metrics = {}
    for line in m.group(1).splitlines():
        if "\t" not in line:
            continue
        k, v = line.split("\t", 1)
        v = v.strip()
        try:
            v = float(v)
        except ValueError:
            pass
        metrics[k] = v
    if not layer:
        layer = "host" if "_HOST@@" in m.group(0) else "core"
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S")
    d = os.path.join(repo, dest, commit[:7])
    os.makedirs(d, exist_ok=True)
    out = os.path.join(d, f"{platform}-{layer}-{stamp}.json")
    doc = {
        "tool": "heddle_osc_perf",
        "schema": 1,
        "commit": commit,
        "ci_run": os.environ.get("CI_RUN", ""),
        "platform": platform,
        "layer": layer,
        "generated_at": stamp + "Z",
        "metrics": metrics,
    }
    json.dump(doc, open(out, "w", encoding="utf-8"), indent=2)
    open(out, "a", encoding="utf-8").write("\n")
    print(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--txt", required=True)
    ap.add_argument("--repo", default=".")
    ap.add_argument("--commit", required=True)
    ap.add_argument("--platform", required=True)
    ap.add_argument("--layer", default="")
    ap.add_argument("--dest", default="perf_reports")
    a = ap.parse_args()
    convert(a.txt, a.repo, a.commit, a.platform, a.layer, a.dest)


if __name__ == "__main__":
    main()

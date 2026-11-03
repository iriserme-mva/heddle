#!/usr/bin/env python3
"""perf_commit_infographic.py — build ONE per-commit perf infographic SVG.

Reads the per-run JSON files of a single commit dir (perf/<sha7>/, up to
<platform>-<layer>-<stamp>.json, schema 1, tool heddle_osc_perf) and writes
perf/<sha7>/infographic.svg (1600x900, dark theme, per-run "verify" style).

Optionally compares against up to two prior commit dirs for delta coloring,
a verdict one-liner, and the delta sidebar. When no prior commit exists the
image degrades gracefully: verdict = baseline, no delta coloring.

Panels:
  header  title + subtitle + stats line
  verdict auto one-liner (codec p99 avg vs prior, +/-10% jitter band)
  A       codec mean/p99 by platform (log-us line chart)
  B       param mean/p99 by platform (log-us line chart)
  C       table: platform x {codec mean, codec p99, codec MB/s, param p99},
          green=improved / red=regressed / blue=neutral vs prior
  D       per-platform run metadata (stamps, nParams, iters, host rates)
  sidebar delta vs prior (auto-generated), footer with commit/ci_run/date

Pure Python 3 stdlib (no matplotlib/PIL). Same committed-script-in-CI
mechanism as perf_infographic.py: the perf_commit job runs this after laying
the JSONs into perf/<sha7>/ and the existing `git add -A perf/` commits the
SVG alongside the JSONs and trend charts.

Usage:
  perf_commit_infographic.py --repo . --commit <sha7> [--prior <sha7> ...]
  (repeat --prior for up to 2 prior commits; default = auto-detect the most
   recent prior commit dir by run timestamp, git-history ordered when the
   checkout has history)
"""
import argparse
import glob
import json
import math
import os
import re
import subprocess
import sys

W, H = 1600, 900
FONT = "Segoe UI, Helvetica, Arial, sans-serif"

# --- palette (matches the hand-written reference infographic) -------------
BG      = "#0b0f1a"
INK     = "#eef2fb"   # light text
MUTED   = "#8f9cbd"
FAINT   = "#5f6c88"
GRID    = "#2a3550"   # axis lines
GRID2   = "#1e2740"   # grid lines
AXLBL   = "#77839f"
SUBTXT  = "#9aa7c2"
CELL    = "#c6d0e6"   # neutral table text
GOOD    = "#38c7a2"   # improvement
BAD     = "#ff7a59"   # regression
NEUT    = "#5aa9ff"   # neutral / baseline series
AMBER   = "#ffc857"
PURP    = "#c084fc"
VERD_BG, VERD_ST, VERD_TX, VERD_SUB = "#0f2b26", "#1f5a4c", "#5fe8c2", "#9fd8c8"
VERD_BG_R, VERD_ST_R, VERD_TX_R, VERD_SUB_R = "#2b1216", "#7a2733", "#ff9d84", "#e8a99a"
PANEL_BG, PANEL_ST = "#10182b", "#23304d"
PLAT_COL = {"ubuntu-latest": NEUT, "windows-latest": AMBER, "macos-15": PURP}
PLAT_ORDER = ["ubuntu-latest", "windows-latest", "macos-15"]
BAND = 0.10   # +/-10% jitter band for verdict + table coloring

T = []  # xml text buffer (append list)


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def txt(x, y, s, size=12, fill=INK, weight=None, anchor="start", op=None):
    w = f' font-weight="{weight}"' if weight else ""
    o = f' fill-opacity="{op}"' if op is not None else ""
    T.append(f'<text x="{x:g}" y="{y:g}" fill="{fill}" font-size="{size:g}"'
             f'{w} text-anchor="{anchor}"{o}>{esc(s)}</text>')


# --- JSON / commit-dir loading --------------------------------------------

def parse_stamp(s):
    """'20261103T070644Z' or '2026-10-21T20:32:30Z' -> (y,mo,d,h,mi,se) or None."""
    m = re.match(r"^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})Z$", str(s or ""))
    if not m:
        m = re.match(r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})Z$", str(s or ""))
    return tuple(int(g) for g in m.groups()) if m else None


def fmt_stamp(t):
    if not t:
        return "n/a"
    return f"{t[0]:04d}-{t[1]:02d}-{t[2]:02d} {t[3]:02d}:{t[4]:02d} UTC"


def load_commit_dir(repo, sha7):
    """{ (platform, layer): json } for one commit dir (latest per key)."""
    d = os.path.join(repo, "perf", sha7)
    if not os.path.isdir(d):
        return None
    best = {}
    for f in sorted(glob.glob(os.path.join(d, "*.json"))):
        try:
            j = json.load(open(f, encoding="utf-8"))
        except Exception as e:
            print(f"WARN bad json {f}: {e}", file=sys.stderr)
            continue
        if j.get("layer") not in ("core", "host"):
            continue
        key = (j.get("platform"), j["layer"])
        t = parse_stamp(j.get("generated_at")) or (0, 0, 0, 0, 0, 0)
        if key not in best or t > best[key][0]:
            best[key] = (t, j)
    return {k: v[1] for k, v in best.items()}


def m(run, key, default=None):
    v = (run or {}).get("metrics", {}).get(key)
    return v if isinstance(v, (int, float)) else default


def dir_min_stamp(repo, name):
    ts = []
    for f in glob.glob(os.path.join(repo, "perf", name, "*.json")):
        try:
            t = parse_stamp(json.load(open(f, encoding="utf-8")).get("generated_at"))
        except Exception:
            t = None
        if t:
            ts.append(t)
    return min(ts) if ts else None


def prior_commits(repo, sha7, n):
    """Up to n prior commit dirs, newest-first. Order: git history when the
    checkout has it (full local clones), else run timestamps (CI depth-1
    checkout; timestamps are run-time-correct for CI-produced JSONs)."""
    perf = os.path.join(repo, "perf")
    known = {d for d in os.listdir(perf)
             if os.path.isdir(os.path.join(perf, d)) and d != "trends"}
    order = []
    try:
        out = subprocess.run(["git", "-C", repo, "rev-list", "--reverse", "HEAD"],
                             capture_output=True, text=True, timeout=30)
        if out.returncode == 0:
            for c in out.stdout.split():
                p7 = c[:7]
                if p7 in known:
                    order.append(p7)
    except Exception:
        pass
    if len(order) >= 2 and sha7 in order:
        idx = order.index(sha7)
        if idx > 0:
            return [p for p in order[:idx]][-n:]
    # timestamp fallback (sha7 may be absent from `known` only if it has no
    # dir; exclude it from candidates explicitly)
    cands = [(dir_min_stamp(repo, d), d) for d in known if d != sha7]
    cands = [(t, d) for t, d in cands if t is not None]
    cur = dir_min_stamp(repo, sha7) or (0, 0, 0, 0, 0, 0)
    cands = [(t, d) for t, d in cands if t < cur]
    cands.sort()
    return [d for _, d in cands[-n:]]


# --- drawing helpers --------------------------------------------------------

def rect(x, y, w, h, fill, stroke=None, sw=1.5, rx=12, op=None):
    o = f' fill-opacity="{op}"' if op is not None else ""
    s = f' stroke="{stroke}" stroke-width="{sw:g}"' if stroke else ""
    T.append(f'<rect x="{x:g}" y="{y:g}" width="{w:g}" height="{h:g}" rx="{rx:g}"'
             f' fill="{fill}"{o}{s}/>')


def line(x1, y1, x2, y2, stroke, sw=1.0, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    T.append(f'<line x1="{x1:g}" y1="{y1:g}" x2="{x2:g}" y2="{y2:g}"'
             f' stroke="{stroke}" stroke-width="{sw:g}"{d}/>')


def circle(x, y, r, fill, stroke=BG, sw=1.5):
    T.append(f'<circle cx="{x:g}" cy="{y:g}" r="{r:g}" fill="{fill}"'
             f' stroke="{stroke}" stroke-width="{sw:g}"/>')


def log_ticks(vmin, vmax, n=5):
    lo, hi = math.log10(max(vmin, 1e-9)), math.log10(max(vmax, 1e-9))
    if hi - lo < 1e-6:
        lo, hi = lo - 0.25, hi + 0.25
    return [10 ** (lo + (hi - lo) * i / (n - 1)) for i in range(n)]


def line_chart(x0, y0, x1, y1, labels, series, legend):
    """Log-us multi-series line chart. series: [(color, [v per label]), ...]."""
    vals = [v for _, vs in series for v in vs if v]
    if not vals:
        txt((x0 + x1) / 2, (y0 + y1) / 2, "no data", 13, FAINT, anchor="middle")
        return
    lo, hi = math.log10(min(vals) * 0.8), math.log10(max(vals) * 1.25)

    def Y(v):
        return y1 - (math.log10(v) - lo) / (hi - lo) * (y1 - y0)

    n = len(labels)
    xs = [x0 if n == 1 else x0 + (x1 - x0) * i / (n - 1) for i in range(n)]
    for tv in log_ticks(min(vals) * 0.8, max(vals) * 1.25):
        yy = Y(tv)
        line(x0, yy, x1, yy, GRID2)
        txt(x0 - 8, yy + 4, f"{tv:.3g}", 11, AXLBL, anchor="end")
    line(x0, y0, x0, y1, GRID, 1.5)
    line(x0, y1, x1, y1, GRID, 1.5)
    for i, lb in enumerate(labels):
        txt(xs[i], y1 + 20, lb, 11.5, AXLBL, anchor="middle")
    for color, vs in series:
        pts = [(xs[i], Y(v)) for i, v in enumerate(vs) if v]
        if len(pts) >= 2:
            T.append(f'<polyline points="{" ".join(f"{x:.1f},{y:.1f}" for x, y in pts)}"'
                     f' fill="none" stroke="{color}" stroke-width="2.5"/>')
        for x, y in pts:
            circle(x, y, 4.5, color)
    # legend (above the chart, like the reference)
    lx = x0 + 300
    for i, (label, color) in enumerate(legend):
        ly = y0 - 68 + i * 16
        rect(lx, ly - 8, 14, 4, color, rx=1)
        txt(lx + 20, ly, label, 11.5, CELL)


def fmt_num(v):
    if v is None:
        return "—"
    return f"{v:.4g}"


def rel(a, b):
    if a is None or b is None or not b:
        return None
    return (a - b) / b


def delta_color(r, higher_is_better=False):
    """Color a relative delta r.  higher_is_better: does a LARGER value mean
    better performance (throughput) vs. a SMALLER value (latency)?
    None / |r|<=BAND -> neutral; else GOOD (improved) or BAD (regressed)."""
    if r is None or abs(r) <= BAND:
        return NEUT, None
    bad = (r < 0) if higher_is_better else (r > 0)
    return (BAD if bad else GOOD), f"{r * 100:+.1f}%"


# --- main build -------------------------------------------------------------

def build(cur, prior, repo, sha7, prior_label):
    plats = [p for p in PLAT_ORDER if (p, "core") in cur or (p, "host") in cur]
    for p, _ in sorted(cur):
        if p not in plats:
            plats.append(p)
    runs = list(cur.values())
    gen = max(parse_stamp(r.get("generated_at")) or (0, 0, 0, 0, 0, 0) for r in runs)
    commit_full = runs[0].get("commit", "")
    ci_run = next((r.get("ci_run") for r in runs if r.get("ci_run")), "")
    tool = runs[0].get("tool", "heddle_osc_perf")
    schema = runs[0].get("schema", 1)

    # version from CMakeLists project() (best effort) — NOT the cmake_minimum
    # VERSION, which is the first 'VERSION' token in the file.
    ver = "dev"
    try:
        cm = open(os.path.join(repo, "CMakeLists.txt"), encoding="utf-8").read()
        mv = re.search(r"\bproject\s*\([^)]*?\bVERSION\s+([0-9][0-9.]*)", cm, re.I)
        if mv:
            ver = mv.group(1)
    except Exception:
        pass

    core = {p: cur.get((p, "core")) for p in plats}
    host = {p: cur.get((p, "host")) for p in plats}
    pcore = {p: (prior or {}).get((p, "core")) for p in plats}

    # ---- metrics + verdict ------------------------------------------------
    c_p99 = [(p, m(core[p], "codec.p99.us")) for p in plats if m(core[p], "codec.p99.us")]
    p_p99 = [(p, m(core[p], "param.p99.us")) for p in plats if m(core[p], "param.p99.us")]
    c_p99_d = [rel(c_p99_d, m(pcore[p], "codec.p99.us")) for p, c_p99_d in c_p99]
    p_p99_d = [rel(v, m(pcore[p], "param.p99.us")) for p, v in p_p99]
    avg_cd = (sum(d for d in c_p99_d if d is not None) / max(1, len([d for d in c_p99_d if d is not None]))) if c_p99_d else None
    avg_pd = (sum(d for d in p_p99_d if d is not None) / max(1, len([d for d in p_p99_d if d is not None]))) if p_p99_d else None

    has_prior = prior is not None and any(pcore.values())
    if not has_prior:
        verdict, regressed = "baseline", False
        v1 = "Verdict: baseline — no prior commit to compare. First data point for this series."
        sub_v = "codec and param latencies shown are absolute values; delta coloring and the Δ sidebar are disabled"
    elif avg_cd is not None:
        if avg_cd > BAND:
            verdict, regressed = "regressed", True
            v1 = f"Verdict: REGRESSED vs {prior_label} — codec p99 avg Δ {avg_cd * 100:+.1f}% (jitter band ±{BAND * 100:.0f}%)"
        elif avg_cd < -BAND:
            verdict, regressed = "improved", False
            v1 = f"Verdict: improved vs {prior_label} — codec p99 avg Δ {avg_cd * 100:+.1f}% (jitter band ±{BAND * 100:.0f}%)"
        else:
            verdict, regressed = "jitter", False
            v1 = f"Verdict: within jitter vs {prior_label} — codec p99 avg Δ {avg_cd * 100:+.1f}% (band ±{BAND * 100:.0f}%)"
        sub_v = (f"param p99 avg Δ {avg_pd * 100:+.1f}%" if avg_pd is not None
                 else "param p99: no comparable data") + " · green = faster, red = slower"
    else:
        verdict, regressed = "jitter", False
        v1 = f"Verdict: no comparable codec data vs {prior_label}"
        sub_v = "prior commit missing core runs; absolute values only"

    # ---- header -----------------------------------------------------------
    T.append(f'<svg width="{W}" height="{H}" xmlns="http://www.w3.org/2000/svg" font-family="{FONT}">')
    T.append(f'<rect width="{W}" height="{H}" fill="{BG}"/>')
    txt(60, 82, f"Heddle perf — commit {sha7}", 30, INK, weight=700)
    txt(60, 114, f"ci_run {ci_run or 'n/a'} · platforms {' · '.join(plats)} · "
                 f"generated {fmt_stamp(gen)} · {tool} schema {schema}", 15, MUTED)
    stats = [f"{len(runs)} JSON runs", f"{len(plats)} platforms"]
    if c_p99:
        stats.append(f"codec p99 avg {fmt_num(sum(v for _, v in c_p99) / len(c_p99))} µs")
    if p_p99:
        stats.append(f"param p99 avg {fmt_num(sum(v for _, v in p_p99) / len(p_p99))} µs")
    mbs = [m(core[p], "codec.mb_per_s") for p in plats if m(core[p], "codec.mb_per_s")]
    if mbs:
        stats.append(f"codec {fmt_num(sum(mbs) / len(mbs))} MB/s")
    mxr = [m(host[p], "max_reliable_rate_sets_per_s") for p in plats if m(host[p], "max_reliable_rate_sets_per_s")]
    if mxr:
        stats.append(f"max reliable {fmt_num(sum(mxr) / len(mxr))} sets/s (host)")
    txt(60, 146, "  ·  ".join(stats), 13.5, FAINT)

    # ---- verdict box --------------------------------------------------------
    vb, vs, vt, vsub = (VERD_BG_R, VERD_ST_R, VERD_TX_R, VERD_SUB_R) if regressed else (VERD_BG, VERD_ST, VERD_TX, VERD_SUB)
    rect(60, 172, 1480, 72, vb, vs, 1.5)
    txt(88, 203, v1, 17, vt, weight=700)
    txt(88, 229, sub_v, 13.5, vsub)

    # ---- panel A: codec by platform ---------------------------------------
    txt(60, 300, "A · codec encode+decode latency by platform (µs, log)", 16, INK, weight=700)
    line_chart(130, 330, 670, 560, [p.replace("-latest", "") for p in plats],
               [(NEUT, [m(core[p], "codec.mean.us") for p in plats]),
                (AMBER, [m(core[p], "codec.p99.us") for p in plats])],
               [("codec mean", NEUT), ("codec p99", AMBER)])
    txt(400, 606, "CI platform", 12.5, SUBTXT, anchor="middle")

    # ---- panel B: param by platform ----------------------------------------
    txt(850, 300, "B · param set-path latency by platform (µs, log)", 16, INK, weight=700)
    line_chart(850, 330, 1410, 560, [p.replace("-latest", "") for p in plats],
               [(GOOD, [m(core[p], "param.mean.us") for p in plats]),
                (PURP, [m(core[p], "param.p99.us") for p in plats])],
               [("param mean", GOOD), ("param p99", PURP)])
    txt(1130, 606, "CI platform", 12.5, SUBTXT, anchor="middle")

    # ---- panel C: table ------------------------------------------------------
    c_title = f"C · all platforms × key metrics — Δ vs {prior_label}" if has_prior else "C · all platforms × key metrics"
    txt(60, 612, c_title, 16, INK, weight=700)
    txt(60, 640, ("green improved / red regressed / neutral within ±10% band"
                  if has_prior else "baseline — no prior commit to compare (neutral)"),
        12, SUBTXT)
    cols = [("codec mean µs", 300, "codec.mean.us", True),
            ("codec p99 µs", 440, "codec.p99.us", True),
            ("codec MB/s", 575, "codec.mb_per_s", False),
            ("param p99 µs", 710, "param.p99.us", True)]
    txt(70, 668, "platform", 11, AXLBL)
    for label, x, _, _ in cols:
        txt(x, 668, label, 11, AXLBL, anchor="end")
    for i, p in enumerate(plats):
        y = 684 + i * 52
        line(70, y - 8, 730, y - 8, GRID2)
        txt(70, y + 16, p, 13.5, PLAT_COL.get(p, CELL), weight=700)
        for label, x, key, latency in cols:
            v = m(core[p], key)
            r = rel(v, m(pcore[p], key)) if has_prior else None
            color, dstr = delta_color(r, higher_is_better=(not latency))
            txt(x, y + 16, fmt_num(v), 14, color, weight=700, anchor="end")
            if has_prior:
                txt(x, y + 32, dstr if dstr else "≈ 0%", 9.5, color, anchor="end")

    # ---- panel D: run metadata ---------------------------------------------
    txt(850, 612, "D · run metadata", 16, INK, weight=700)
    for i, p in enumerate(plats):
        y = 650 + i * 64
        pc, ph = core[p], host[p]
        txt(862, y + 8, p, 13, PLAT_COL.get(p, CELL), weight=700)
        cs = parse_stamp(pc.get("generated_at")) if pc else None
        cinfo = "core —"
        if pc:
            cinfo = (f"core {fmt_stamp(cs).split(' ')[1].replace(' UTC', '')} · "
                     f"nParams {m(pc, 'nParams', 0):g} · iters {m(pc, 'iters', 0):,.0f} · "
                     f"loopback {m(pc, 'loopback', 0):,.0f}")
        txt(862, y + 26, cinfo, 11.5, MUTED)
        hs = parse_stamp(ph.get("generated_at")) if ph else None
        hinfo = "host —"
        if ph:
            rate_keys = [k for k in ph["metrics"] if k.startswith("rate.5000.")]
            rtt = m(ph, "rate.5000.rtt_median_us")
            loss = m(ph, "rate.5000.loss_pct")
            hinfo = (f"host {fmt_stamp(hs).split(' ')[1].replace(' UTC', '')} · "
                     f"{m(ph, 'baseline.blocks_per_s', 0):g} blk/s · "
                     f"{m(ph, 'max_reliable_rate_sets_per_s', 0):,.0f} sets/s max · "
                     f"RTT {rtt / 1000:.1f} ms · loss {loss:g}% @5k"
                     if rtt and loss is not None else
                     f"host {fmt_stamp(hs).split(' ')[1].replace(' UTC', '')}")
        txt(862, y + 42, hinfo, 11.5, MUTED)

    # ---- sidebar -------------------------------------------------------------
    rect(1408, 622, 132, 268, PANEL_BG, PANEL_ST, 1.5)
    if has_prior:
        txt(1420, 646, f"Δ vs {prior_label}", 12.5, CELL, weight=700)
        txt(1420, 668, "codec·param·mb/s", 10, FAINT)
        for i, p in enumerate(plats):
            y = 692 + i * 18
            d1 = rel(m(core[p], "codec.p99.us"), m(pcore[p], "codec.p99.us"))
            d2 = rel(m(core[p], "param.p99.us"), m(pcore[p], "param.p99.us"))
            d3 = rel(m(core[p], "codec.mb_per_s"), m(pcore[p], "codec.mb_per_s"))
            col, _ = delta_color(d1)
            def f(d):
                return f"{d * 100:+4.1f}" if d is not None else "  – "
            circle(1423, y - 3.5, 2.5, col)
            txt(1431, y, f"{p[:7]:7s}{f(d1)}·{f(d2)}·{f(d3)}", 10.5, CELL)
        txt(1420, 876, f"{len(runs)} JSON runs ok", 10, FAINT)
    else:
        txt(1420, 646, "baseline", 12.5, CELL, weight=700)
        for i, s in enumerate(["no prior run to", "compare — deltas", "disabled"]):
            txt(1420, 670 + i * 14, s, 10.5, MUTED)
        txt(1420, 876, f"{len(runs)} JSON runs ok", 10, FAINT)

    # ---- footer --------------------------------------------------------------
    txt(60, 878, f"Heddle v{ver} · commit {commit_full} · ci_run {ci_run or 'n/a'} · "
                 f"{tool} schema {schema} · {fmt_stamp(gen)}", 12, FAINT)
    T.append("</svg>")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", default=".")
    ap.add_argument("--commit", required=True, help="commit sha7 of the target commit dir")
    ap.add_argument("--prior", action="append", default=None,
                    help="prior commit sha7 for delta context (repeatable, max 2)")
    a = ap.parse_args()
    repo = a.repo
    sha7 = a.commit[:7]
    cur = load_commit_dir(repo, sha7)
    if not cur:
        sys.exit(f"no perf JSONs found in {os.path.join(repo, 'perf', sha7)}")
    priors = (a.prior or [])[:2]
    if not priors:
        priors = prior_commits(repo, sha7, 1)
    prior = None
    prior_label = "baseline"
    for p7 in priors:
        d = load_commit_dir(repo, p7)
        if d:
            prior, prior_label = d, p7
            break
    build(cur, prior, repo, sha7, prior_label)
    out = os.path.join(repo, "perf", sha7, "infographic.svg")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(T))
    print(f"infographic -> {out} ({os.path.getsize(out)} bytes, prior={prior_label})")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""perf_infographic.py — build SVG infographics from perf/*.json runs.

Usage:
  perf_infographic.py --repo . [--only <sha7>]

For every (commit, platform, layer) JSON it writes a per-run snapshot SVG
next to the JSON (same stem, .svg), and refreshes the per-platform/layer
trend charts under perf/trends/. Pure stdlib, no matplotlib.
"""
import argparse, glob, json, os, sys

W, H = 1080, 560
MARG = (70, 40, 30, 60)  # l t r b

CORE_METRICS = [
    ("codec.mean.us", "codec encode+decode (mean)", "us"),
    ("param.mean.us", "param set-path (mean)", "us"),
    ("loopback.oneway.mean.us", "UDP oneway (mean)", "us"),
    ("loopback.rtt.mean.us", "set->ack RTT (mean)", "us"),
]
HOST_METRICS = [
    ("rate.50.rtt_median_us", "RTT @50/s (med)", "us"),
    ("rate.500.rtt_median_us", "RTT @500/s (med)", "us"),
    ("rate.5000.rtt_median_us", "RTT @5000/s (med)", "us"),
    ("baseline.blocks_per_s", "baseline blocks/s", "blk/s"),
]


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def svg_header(title, subtitle):
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
        f'viewBox="0 0 {W} {H}">'
        f'<rect width="{W}" height="{H}" fill="#0d1117"/>'
        f'<text x="24" y="34" fill="#e6edf3" font-family="Consolas,monospace" '
        f'font-size="20" font-weight="bold">{esc(title)}</text>'
        f'<text x="24" y="56" fill="#8b949e" font-family="Consolas,monospace" '
        f'font-size="13">{esc(subtitle)}</text>'
    )


def draw_bars(svg, entries, x0, y0, w, h):
    """entries: list of (label, value, unit). Horizontal bars, log-ish scale."""
    if not entries:
        return
    vmax = max(v for _, v, _ in entries) or 1.0
    row = h / len(entries)
    for i, (label, v, unit) in enumerate(entries):
        y = y0 + i * row
        bw = max(2, (v / vmax) * (w - 220))
        svg.append(f'<rect x="{x0}" y="{y + 8:.1f}" width="{bw:.1f}" height="{row * 0.5:.1f}" fill="#3fb950" rx="2"/>')
        svg.append(f'<text x="{x0 + bw + 8:.1f}" y="{y + 8 + row * 0.5 + 5:.1f}" fill="#e6edf3" font-family="Consolas,monospace" font-size="14">{v:g} {esc(unit)}</text>')
        svg.append(f'<text x="{x0}" y="{y + row - 6:.1f}" fill="#8b949e" font-family="Consolas,monospace" font-size="12">{esc(label)}</text>')


def draw_trend(svg, points, x0, y0, w, h, title, color="#58a6ff"):
    """points: list of (x_label, value). x_label = commit short + date."""
    if len(points) < 2:
        return
    vals = [v for _, v in points]
    vmin, vmax = min(vals), max(vals)
    span = (vmax - vmin) or abs(vmax) or 1.0
    vmin -= span * 0.1
    vmax += span * 0.22
    n = len(points)
    def X(i): return x0 + (i / max(1, n - 1)) * w
    def Y(v): return y0 + h - ((v - vmin) / (vmax - vmin)) * h
    pts = " ".join(f"{X(i):.1f},{Y(v):.1f}" for i, (_, v) in enumerate(points))
    svg.append(f'<polyline points="{pts}" fill="none" stroke="{color}" stroke-width="2.5"/>')
    for i, (_, v) in enumerate(points):
        svg.append(f'<circle cx="{X(i):.1f}" cy="{Y(v):.1f}" r="3.5" fill="{color}"/>')
    # y axis labels
    for f in (0, 0.5, 1):
        v = vmin + f * (vmax - vmin)
        svg.append(f'<text x="{x0 - 8}" y="{Y(v) + 4:.1f}" fill="#8b949e" font-family="Consolas,monospace" font-size="11" text-anchor="end">{v:g}</text>')
    # x labels: first, middle, last
    for i in sorted({0, n // 2, n - 1}):
        svg.append(f'<text x="{X(i):.1f}" y="{y0 + h + 18}" fill="#8b949e" font-family="Consolas,monospace" font-size="11" text-anchor="middle">{esc(points[i][0])}</text>')
    svg.append(f'<text x="{x0}" y="{y0 - 10}" fill="#e6edf3" font-family="Consolas,monospace" font-size="13">{esc(title)}</text>')


def load_runs(repo):
    runs = []
    for p in glob.glob(os.path.join(repo, "perf", "*", "*-core-*.json")) + \
             glob.glob(os.path.join(repo, "perf", "*", "*-host-*.json")):
        try:
            runs.append(json.load(open(p, encoding="utf-8")))
        except Exception as e:
            print(f"WARN bad json {p}: {e}", file=sys.stderr)
    return runs


def run_snapshot(repo, r):
    d = os.path.dirname(r["_path"])
    stem = os.path.splitext(os.path.basename(r["_path"]))[0]
    out = os.path.join(d, stem + ".svg")
    metrics = r["metrics"]
    mdef = CORE_METRICS if r["layer"] == "core" else HOST_METRICS
    title = f"Heddle OSC perf — {r['layer']}"
    sub = (f"commit {r['commit'][:7]}  |  {r['platform']}  |  "
           f"{r['generated_at']}  |  schema {r.get('schema', 1)}")
    svg = [svg_header(title, sub)]
    entries = [(lbl, metrics.get(k, 0.0), unit) for k, lbl, unit in mdef if k in metrics]
    draw_bars(svg, entries, MARG[0], 90, W - MARG[0] - MARG[2], H - 200)
    # extras panel: right column
    ex_x = MARG[0] + 460
    svg.append(f'<text x="{ex_x}" y="100" fill="#8b949e" font-family="Consolas,monospace" font-size="13">all metrics (schema 1)</text>')
    y = 122
    for k in sorted(metrics):
        v = metrics[k]
        if not isinstance(v, (int, float)):
            continue
        if any(k == a for a, _, _ in mdef):
            continue
        svg.append(f'<text x="{ex_x}" y="{y}" fill="#8b949e" font-family="Consolas,monospace" font-size="12">{esc(k)}</text>')
        svg.append(f'<text x="{W - MARG[2]}" y="{y}" fill="#e6edf3" font-family="Consolas,monospace" font-size="12" text-anchor="end">{v:g}</text>')
        y += 16
        if y > H - MARG[3]:
            break
    svg.append("</svg>")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(svg))
    return out


def trend_charts(repo):
    runs = load_runs(repo)
    tdir = os.path.join(repo, "perf", "trends")
    os.makedirs(tdir, exist_ok=True)
    out = []
    groups = {}
    for r in runs:
        groups.setdefault((r["platform"], r["layer"]), []).append(r)
    for (plat, layer), rs in sorted(groups.items()):
        rs.sort(key=lambda r: (r["generated_at"], r["commit"]))
        mdef = CORE_METRICS if layer == "core" else HOST_METRICS
        title = f"Heddle OSC perf trend — {layer} / {plat}"
        svg = [svg_header(title, f"{len(rs)} runs, {rs[0]['generated_at'][:10]} .. {rs[-1]['generated_at'][:10]}")]
        n = len(mdef)
        ph = (H - 140) // n
        for j, (k, lbl, unit) in enumerate(mdef):
            pts = []
            for r in rs:
                v = r["metrics"].get(k)
                if isinstance(v, (int, float)):
                    pts.append((r["commit"][:7], v))
            draw_trend(svg, pts, MARG[0], 110 + j * ph, W - MARG[0] - MARG[2] - 40, ph - 30, f"{lbl}  ({unit})", "#58a6ff" if layer == "core" else "#d29922")
        svg.append("</svg>")
        p = os.path.join(tdir, f"{plat}-{layer}.svg")
        with open(p, "w", encoding="utf-8") as f:
            f.write("\n".join(svg))
        out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".")
    ap.add_argument("--only", default=None, help="commit sha7 filter")
    a = ap.parse_args()
    runs = load_runs(a.repo)
    for r in runs:
        r["_path"] = None
    # re-path
    import glob as g
    paths = (g.glob(os.path.join(a.repo, "perf", "*", "*-core-*.json")) +
             g.glob(os.path.join(a.repo, "perf", "*", "*-host-*.json")))
    by_path = {p: json.load(open(p, encoding="utf-8")) for p in paths}
    n = 0
    for p, r in by_path.items():
        commit_dir = os.path.basename(os.path.dirname(p))
        if a.only and not commit_dir.startswith(a.only):
            continue
        r["_path"] = p
        run_snapshot(a.repo, r)
        n += 1
    tr = trend_charts(a.repo)
    print(f"sn={n} trends={len(tr)}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Summarize benchmark CSV samples and plot measured latency distributions."""

import argparse
import csv
import html
import math
from collections import defaultdict
from pathlib import Path
from statistics import fmean, median


def percentile(values, fraction):
    ordered = sorted(values)
    # Nearest-rank percentile, consistent with the benchmark report convention.
    index = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[index]


def read_rows(path):
    with path.open(newline="", encoding="utf-8") as source:
        return list(csv.DictReader(source))


def write_svg_histogram(path, title, series, xlabel, logarithmic=False):
    """Render simple dependency-free SVG histograms from raw samples."""
    width, height = 960, 560
    left, right, top, bottom = 90, 30, 85, 100
    plot_w, plot_h = width - left - right, height - top - bottom
    transform = math.log10 if logarithmic else (lambda value: value)
    inverse = (lambda value: 10 ** value) if logarithmic else (lambda value: value)
    all_values = [transform(float(value)) for _, values, _ in series for value in values if float(value) > 0]
    if not all_values:
        return
    low = min(all_values)
    high = percentile(all_values, 0.995)
    if high <= low:
        high = low + 1.0
    bins = 40
    bin_width = (high - low) / bins
    counts_by_series = []
    for _, values, _ in series:
        counts = [0] * bins
        for value in values:
            value = float(value)
            if value <= 0:
                continue
            index = int((transform(value) - low) / bin_width)
            counts[max(0, min(bins - 1, index))] += 1
        counts_by_series.append(counts)
    maximum_count = max(max(counts) for counts in counts_by_series) or 1
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
             '<rect width="100%" height="100%" fill="white"/>',
             f'<text x="{width/2}" y="38" text-anchor="middle" font-family="Arial" font-size="24">{html.escape(title)}</text>']
    for tick in range(6):
        y = top + plot_h * tick / 5
        value = maximum_count * (1 - tick / 5)
        parts.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left+plot_w}" y2="{y:.1f}" stroke="#dddddd"/>')
        parts.append(f'<text x="{left-12}" y="{y+5:.1f}" text-anchor="end" font-family="Arial" font-size="13">{value:.0f}</text>')
    group_width = plot_w / bins
    bar_width = group_width / len(series)
    for s_index, ((label, _, color), counts) in enumerate(zip(series, counts_by_series)):
        for index, count in enumerate(counts):
            bar_h = plot_h * count / maximum_count
            x = left + index * group_width + s_index * bar_width
            y = top + plot_h - bar_h
            parts.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{max(1,bar_width-0.7):.2f}" height="{bar_h:.2f}" fill="{color}" fill-opacity="0.72"><title>{html.escape(label)}: {count} samples</title></rect>')
    parts.append(f'<line x1="{left}" y1="{top+plot_h}" x2="{left+plot_w}" y2="{top+plot_h}" stroke="#333"/>')
    parts.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top+plot_h}" stroke="#333"/>')
    for tick in range(6):
        x = left + plot_w * tick / 5
        value = inverse(low + (high - low) * tick / 5)
        parts.append(f'<text x="{x:.1f}" y="{top+plot_h+24}" text-anchor="middle" font-family="Arial" font-size="12">{value:.3g}</text>')
    parts.append(f'<text x="{left+plot_w/2}" y="{height-22}" text-anchor="middle" font-family="Arial" font-size="16">{html.escape(xlabel)}</text>')
    parts.append(f'<text transform="translate(24 {top+plot_h/2}) rotate(-90)" text-anchor="middle" font-family="Arial" font-size="16">Samples</text>')
    legend_x = left
    for label, _, color in series:
        parts.append(f'<rect x="{legend_x}" y="{height-65}" width="15" height="15" fill="{color}"/>')
        parts.append(f'<text x="{legend_x+22}" y="{height-52}" font-family="Arial" font-size="13">{html.escape(label)}</text>')
        legend_x += max(150, 18 + len(label) * 8)
    parts.append(f'<text x="{left+plot_w}" y="{height-52}" text-anchor="end" font-family="Arial" font-size="11" fill="#555">x-axis ends at p99.5; tail samples are grouped in final bin</text>')
    parts.append('</svg>')
    path.write_text('\n'.join(parts), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_path", nargs="?", default="benchmark_results/raw_measurements.csv")
    parser.add_argument("--output-dir", default=None)
    parser.add_argument("--run-id", help="Analyze only this run (default: latest run in the CSV)")
    args = parser.parse_args()

    csv_path = Path(args.csv_path)
    output_dir = Path(args.output_dir) if args.output_dir else csv_path.parent
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = read_rows(csv_path)
    if not rows:
        raise SystemExit(f"No benchmark samples found in {csv_path}")

    run_ids = list(dict.fromkeys(row["run_id"] for row in rows))
    selected_run = args.run_id or run_ids[-1]
    if selected_run not in run_ids:
        raise SystemExit(f"Run ID {selected_run!r} is not present in {csv_path}")

    grouped = defaultdict(list)
    for row in rows:
        grouped[(row["run_id"], row["section"], row["metric"], row["codec"],
                 row["scenario"], row["unit"])].append(float(row["value"]))

    fields = ["run_id", "section", "metric", "codec", "scenario", "unit", "samples",
              "min", "p50", "mean", "p99", "p99_9", "max"]
    summary_path = output_dir / "latency_summary.csv"
    summaries = []
    latency_sections = {"codec_cpu", "dispatch_latency", "round_trip"}
    for key, values in sorted(grouped.items()):
        run_id, section, metric, codec, scenario, unit = key
        if section not in latency_sections:
            continue
        values_sorted = sorted(values)
        summaries.append({
            "run_id": run_id, "section": section, "metric": metric,
            "codec": codec, "scenario": scenario, "unit": unit,
            "samples": len(values), "min": values_sorted[0],
            "p50": percentile(values, 0.50), "mean": fmean(values),
            "p99": percentile(values, 0.99), "p99_9": percentile(values, 0.999),
            "max": values_sorted[-1],
        })
    with summary_path.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summaries)

    latest = [row for row in summaries if row["run_id"] == selected_run]
    markdown_path = output_dir / "latency_summary.md"
    with markdown_path.open("w", encoding="utf-8") as target:
        target.write(f"# Benchmark summary — run {selected_run}\n\n")
        target.write("Percentiles use nearest-rank over raw CSV samples. Units are shown per measurement.\n\n")
        target.write("| Section | Metric | Codec | Scenario | Samples | p50 | Mean | p99 | p99.9 | Max | Unit |\n")
        target.write("|---|---|---|---|---:|---:|---:|---:|---:|---:|---|\n")
        for row in latest:
            target.write("| {section} | {metric} | {codec} | {scenario} | {samples} | {p50:.3f} | "
                         "{mean:.3f} | {p99:.3f} | {p99_9:.3f} | {max:.3f} | {unit} |\n".format(**row))
        target.write("\nText network RTT is not present because this project has no text-protocol dispatcher. "
                     "No text RTT values are inferred or synthesized.\n")
        dispatch = next((row for row in latest if row["metric"] == "frame_bytes_to_queue_push_us"), None)
        if dispatch:
            result = "PASS" if dispatch["p99"] <= 50.0 else "FAIL"
            target.write(f"\nSection 8 dispatch target (p99 ≤ 50 us): **{result}** "
                         f"({dispatch['p99']:.3f} us).\n")

    selected_raw = [row for row in rows if row["run_id"] == selected_run]
    report_path = output_dir / "benchmark_report.md"
    with report_path.open("w", encoding="utf-8") as target:
        target.write(f"# Benchmark results — run {selected_run}\n\n")
        target.write("Measurements follow Section 8 of `protocol_design_and_plan.md`. "
                     "Latency percentiles are calculated from raw samples with nearest-rank.\n\n")
        target.write("## Exact wire size\n\n| Message | Binary bytes | HTTP/JSON bytes |\n|---|---:|---:|\n")
        sizes = {(row["scenario"], row["codec"]): int(float(row["value"])) for row in selected_raw
                 if row["section"] == "wire_size"}
        for scenario in sorted({scenario for scenario, _ in sizes}):
            binary = sizes.get((scenario, "binary"), "—")
            text_size = sizes.get((scenario, "text_http_json"), "—")
            target.write(f"| {scenario} | {binary} | {text_size} |\n")

        target.write("\n## Latency measurements\n\n| Measurement | Codec | Scenario | Samples | p50 | Mean | p99 | p99.9 | Max | Unit |\n")
        target.write("|---|---|---|---:|---:|---:|---:|---:|---:|---|\n")
        for row in latest:
            target.write("| {metric} | {codec} | {scenario} | {samples} | {p50:.3f} | {mean:.3f} | "
                         "{p99:.3f} | {p99_9:.3f} | {max:.3f} | {unit} |\n".format(**row))

        target.write("\n## Throughput\n\n| Codec | Scenario | Operations per second |\n|---|---|---:|\n")
        for row in selected_raw:
            if row["section"] == "throughput":
                target.write(f"| {row['codec']} | {row['scenario']} | {float(row['value']):.1f} |\n")
        target.write("\n## Not measured\n\n- Text protocol network RTT and throughput: no text-protocol dispatcher exists.\n"
                     "- Two-host LAN: run requires a second host.\n"
                     "- Idle-policy sweep: callback update delivery and connection churn are not measured.\n"
                     "\nNo values are estimated for these items.\n\n## Plots\n\n"
                     "- `round_trip_latency.svg` — binary loopback RTT.\n"
                     "- `dispatch_latency.svg` — complete encoded frame bytes through queue insertion.\n"
                     "- `binary_codec_latency.svg` and `text_codec_latency.svg` — in-process codec costs, not network RTT.\n"
                     "- `bid_codec_comparison.svg` — Binary vs Text HTTP/JSON BID codec latency overlaid on one graph.\n"
                     "\n`in_process` timings run codec/dispatch work locally without a network round trip. `loopback` timings send requests over TCP between the client and server on the same computer.\n"
                     "- PNG copies are also made when Matplotlib is installed.\n")

    latest_rows = [row for row in rows if row["run_id"] == selected_run]
    plots = []
    for metric, title, filename, unit in [
        ("request_reply_us", "Binary loopback order round-trip latency", "round_trip_latency.svg", "us"),
        ("frame_bytes_to_queue_push_us", "Dispatch frame-bytes-to-queue latency", "dispatch_latency.svg", "us"),
    ]:
        data = [float(row["value"]) for row in latest_rows if row["metric"] == metric]
        if not data:
            continue
        path = output_dir / filename
        write_svg_histogram(path, f"{title} (n={len(data):,})", [("samples", data, "#2878B5")], f"Latency ({unit})")
        plots.append(path)

    codec_rows = [row for row in latest_rows if row["metric"] == "encode_decode_ns_per_message"]
    codec_groups = defaultdict(list)
    for row in codec_rows:
        codec_groups[(row["codec"], row["scenario"])].append(float(row["value"]))
    if codec_groups:
        # Section 8 calls for one latency histogram per codec. These are
        # encode+decode CPU latency distributions; they are not network RTTs.
        for codec_name, filename in [("binary", "binary_codec_latency.svg"),
                                     ("text_http_json", "text_codec_latency.svg")]:
            series = [(scenario, values, color) for (codec, scenario), values in codec_groups.items()
                      if codec == codec_name
                      for color in ["#2878B5" if scenario == "BID" else "#E1812C"]]
            if not series:
                continue
            path = output_dir / filename
            write_svg_histogram(path, f"{codec_name} encode + decode latency distributions",
                                series, "Nanoseconds per message", logarithmic=True)
            plots.append(path)

        bid_comparison = []
        for codec, label, color in [("binary", "Binary BID", "#2878B5"),
                                    ("text_http_json", "Text HTTP/JSON BID", "#E1812C")]:
            values = codec_groups.get((codec, "BID"), [])
            if values:
                bid_comparison.append((label, values, color))
        if bid_comparison:
            path = output_dir / "bid_codec_comparison.svg"
            write_svg_histogram(path, "BID encode + decode latency: Binary vs Text HTTP/JSON",
                                bid_comparison, "Nanoseconds per message", logarithmic=True)
            plots.append(path)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("Matplotlib unavailable; SVG plots were generated with Python's standard library.")
    else:
        for metric, title, filename in [
            ("request_reply_us", "Binary loopback order round-trip latency", "round_trip_latency.png"),
            ("frame_bytes_to_queue_push_us", "Dispatch frame-bytes-to-queue latency", "dispatch_latency.png"),
        ]:
            data = [float(row["value"]) for row in latest_rows if row["metric"] == metric]
            if data:
                fig, ax = plt.subplots(figsize=(9, 5.2), constrained_layout=True)
                ax.hist(data, bins="auto", color="#2878B5", edgecolor="white", alpha=0.9)
                ax.set(title=f"{title} (n={len(data):,})", xlabel="Latency (us)", ylabel="Samples")
                ax.grid(axis="y", alpha=0.25)
                path = output_dir / filename
                fig.savefig(path, dpi=160)
                plt.close(fig)
                plots.append(path)
        if codec_groups:
            labels = [f"{codec}\n{scenario}" for codec, scenario in codec_groups]
            fig, ax = plt.subplots(figsize=(10, 5.5), constrained_layout=True)
            ax.boxplot(list(codec_groups.values()), tick_labels=labels, showfliers=True)
            ax.set_yscale("log")
            ax.set(title="Codec encode + decode CPU cost (batch means)", ylabel="Nanoseconds per message")
            ax.grid(axis="y", alpha=0.25)
            path = output_dir / "codec_cpu_cost.png"
            fig.savefig(path, dpi=160)
            plt.close(fig)
            plots.append(path)
        for path in [output_dir / "binary_codec_latency.svg", output_dir / "text_codec_latency.svg"]:
            if not path.exists():
                continue
            # Keep per-codec PNGs optional; SVG remains the portable source plot.
            codec_name = "binary" if path.name.startswith("binary") else "text_http_json"
            fig, ax = plt.subplots(figsize=(9, 5.2), constrained_layout=True)
            for (codec, scenario), values in codec_groups.items():
                if codec == codec_name:
                    ax.hist(values, bins="auto", alpha=0.65, label=scenario, edgecolor="white")
            ax.set_xscale("log")
            ax.set(title=f"{codec_name} encode + decode latency distributions",
                   xlabel="Nanoseconds per message", ylabel="Samples")
            ax.legend()
            png_path = path.with_suffix(".png")
            fig.savefig(png_path, dpi=160)
            plt.close(fig)
            plots.append(png_path)
        bid_values = [(codec, values) for (codec, scenario), values in codec_groups.items() if scenario == "BID"]
        if len(bid_values) == 2:
            fig, ax = plt.subplots(figsize=(9, 5.2), constrained_layout=True)
            for codec, values in bid_values:
                label = "Binary" if codec == "binary" else "Text HTTP/JSON"
                ax.hist(values, bins="auto", alpha=0.6, label=label, edgecolor="white")
            ax.set_xscale("log")
            ax.set(title="BID encode + decode latency: Binary vs Text HTTP/JSON",
                   xlabel="Nanoseconds per message", ylabel="Samples")
            ax.legend()
            ax.grid(axis="y", alpha=0.25)
            path = output_dir / "bid_codec_comparison.png"
            fig.savefig(path, dpi=160)
            plt.close(fig)
            plots.append(path)

    print(f"Analyzed run {selected_run} from {csv_path}")
    print(f"Summary CSV: {summary_path}")
    print(f"Summary Markdown: {markdown_path}")
    print(f"Full report: {report_path}")
    for path in plots:
        print(f"Plot: {path}")


if __name__ == "__main__":
    main()

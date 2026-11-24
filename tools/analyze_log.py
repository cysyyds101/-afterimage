"""Analyse a whale-maid serial log against the Phase 1 acceptance criteria.

Stdlib only, so it runs under any Python (the IDF venv one is fine too):

    python tools/analyze_log.py firmware/p0-after.log
    python tools/analyze_log.py firmware/p0-after.log --json

Exit status is 0 only when every *measurable* criterion passes. Criteria that
need a human (visual flicker check, spoken wake word) are reported as MANUAL
and never silently counted as passes.
"""
import argparse
import json
import re
import statistics
import sys
from pathlib import Path

# Render cost. "Frame stats" is emitted unconditionally every FRAME_STATS_PERIOD
# frames; "Frame slow" only fires on overrun, so it alone cannot prove <30 ms.
RE_STATS = re.compile(
    r"Frame stats: n=(\d+) min=(\d+) avg=(\d+) max=(\d+) us overrun=(\d+) (?:cache|resident)=(\w+)")
RE_SLOW = re.compile(r"Frame slow: (\d+) us")
# Keep the boot-relative timestamp of each overrun so it can be correlated with
# whatever else the log was doing at that instant (Wi-Fi/PHY init, codec init,
# cache fill) instead of guessing at the cause.
RE_SLOW_AT = re.compile(r"^[IWE] \((\d+)\).*Frame slow: (\d+) us", re.MULTILINE)
RE_TEST_CMD = re.compile(r"Test command: (\w+)")
RE_CONSOLE = re.compile(r"Test console ready")
RE_HEAP = re.compile(r"free internal=(\d+)(?: dma_block=(\d+))?(?: PSRAM=(\d+))?")
RE_TREND = re.compile(r"Heap trend: internal=(\d+) dma_block=(\d+) psram=(\d+)")
RE_TS = re.compile(r"^[IWE] \((\d+)\)", re.MULTILINE)
# capture_boot.py appends the real wall-clock capture window; board logs are
# periodic, so the last timestamp alone understates a soak by a stats period.
RE_CAPTURE = re.compile(r"\[capture\] duration=([\d.]+)s")
# A task-watchdog report also prints "Backtrace:", so matching that word alone
# counted watchdog stalls as crashes. Only genuine fatal paths count here.
RE_ABORT = re.compile(r"abort\(\) was called|Guru Meditation|Kernel panic")
RE_SELECTED = re.compile(r"Selected (\w+)")
RE_CACHED = re.compile(r"Cached (\w+) \((\d+) frames in PSRAM")
RE_DISPLAY_COST = re.compile(r"Display cost \w+: lvgl_lock=(\d+) bind=(\d+) total=(\d+) us")
# One failure prints two matching lines (location + message); count the message.
RE_DMA = re.compile(r"Failed to allocate priv TX buffer")
RE_WDT = re.compile(r"Task watchdog got triggered")
RE_BOOT = re.compile(r"rst:0x[0-9a-f]+ \(|boot:ESP-ROM")
RE_PREDEC = re.compile(r"Idle pre-decoded|Resident frames:")
RE_MN_RELEASE = re.compile(r"MultiNet released for cloud mode")
RE_MN_LOAD = re.compile(r"Offline commands loaded")
RE_CACHE_FAIL = re.compile(r"Frame cache allocation failed|Cache decode failed|Idle pre-decode failed|PSRAM frame allocation failed")
RE_SPEAKER_FULL = re.compile(r"Speaker queue full")
RE_ASSET_ERR = re.compile(r"Missing /storage/|Invalid/oversize JPEG|Bad dimensions/JPEG header|Decode failed")

FRAME_MAX_US = 15000          # Phase 1 DoD: single frame bind < 15 ms
BASELINE_INTERNAL = (46003, 62943)   # measured Nov 12 range, worst..best
INTERNAL_TARGET_B = 81920     # Phase 1 DoD: pure-STA steady internal > 80 KB
EXPECTED_ACTIONS = {"idle", "happy", "sleep", "listening", "speaking"}
MIN_SWITCHES = 60             # Phase 1 DoD: 60 state switches in the soak
SOAK_SECONDS = 900            # Phase 1 DoD: 15 minutes continuous
# A soak is only meaningful if the heap is not steadily draining. Allow slack
# for Wi-Fi/lwIP buffer churn, but flag a sustained one-way decline.
LEAK_TOLERANCE_B = 8192


def analyse(text):
    stats = [tuple(int(g) for g in m.groups()[:5]) + (m.group(6),)
             for m in RE_STATS.finditer(text)]
    slow = [int(m.group(1)) for m in RE_SLOW.finditer(text)]
    heap = [(int(m.group(1)),
             int(m.group(2)) if m.group(2) else None,
             int(m.group(3)) if m.group(3) else None)
            for m in RE_HEAP.finditer(text)]
    trend = [tuple(int(g) for g in m.groups()) for m in RE_TREND.finditer(text)]
    stamps = [int(m.group(1)) for m in RE_TS.finditer(text)]
    # Timestamps restart at 0 on every boot, so max-min across the whole log
    # understates a soak that rebooted. Sum each boot segment instead.
    segments = re.split(r"ESP-ROM:esp32s3", text)
    duration_s = 0.0
    for segment in segments:
        marks = [int(m.group(1)) for m in RE_TS.finditer(segment)]
        if len(marks) > 1:
            duration_s += (max(marks) - min(marks)) / 1000.0
    capture = RE_CAPTURE.search(text)
    if capture:
        duration_s = float(capture.group(1))
    selected = [m.group(1) for m in RE_SELECTED.finditer(text)]
    cached = [(m.group(1), int(m.group(2))) for m in RE_CACHED.finditer(text)]

    # Aggregate the per-window stats into a whole-log distribution.
    n = sum(s[0] for s in stats)
    mins = [s[1] for s in stats]
    maxs = [s[3] for s in stats]
    avgs = [s[2] for s in stats]
    overrun = sum(s[4] for s in stats)
    weighted_avg = (sum(s[2] * s[0] for s in stats) / n) if n else None

    internal = [h[0] for h in heap]
    dma_blocks = [h[1] for h in heap if h[1] is not None]
    psram = [h[2] for h in heap if h[2] is not None]
    trend_internal = [t[0] for t in trend]
    trend_dma = [t[1] for t in trend]
    trend_psram = [t[2] for t in trend]
    # Heap-trend samples taken after the last MultiNet load/release, so a mode
    # switch is not mistaken for a leak.
    mode_marks = [mm.end() for mm in re.finditer(
        r"Offline commands loaded|MultiNet released for cloud mode", text)]
    cut = mode_marks[-1] if mode_marks else 0
    trend_stable_internal = [int(mm.group(1)) for mm in RE_TREND.finditer(text) if mm.start() >= cut]

    return {
        "frame_stats_windows": len(stats),
        "frames_measured": n,
        "frame_min_us": min(mins) if mins else None,
        "frame_max_us": max(maxs) if maxs else None,
        "frame_avg_us": round(weighted_avg) if weighted_avg is not None else None,
        "frame_median_of_window_avgs_us": round(statistics.median(avgs)) if avgs else None,
        "frame_overrun_count": overrun,
        "frame_slow_count": len(slow),
        "frame_slow_max_us": max(slow) if slow else None,
        "frame_slow_events": [(int(m.group(1)), int(m.group(2)))
                              for m in RE_SLOW_AT.finditer(text)],
        "display_cost_samples": len(RE_DISPLAY_COST.findall(text)),
        "display_lock_max_us": max((int(m.group(1)) for m in RE_DISPLAY_COST.finditer(text)),
                                   default=None),
        "cache_enabled": sorted({s[5] for s in stats}),
        "internal_free_min": min(internal) if internal else None,
        "internal_free_max": max(internal) if internal else None,
        "dma_block_min": min(dma_blocks) if dma_blocks else None,
        "dma_block_max": max(dma_blocks) if dma_blocks else None,
        "psram_free_min": min(psram) if psram else None,
        "psram_free_max": max(psram) if psram else None,
        "heap_trend_samples": len(trend),
        "trend_internal": trend_internal,
        "trend_internal_first": trend_internal[0] if trend_internal else None,
        "trend_internal_last": trend_internal[-1] if trend_internal else None,
        "trend_internal_min": min(trend_internal) if trend_internal else None,
        "trend_dma_block_min": min(trend_dma) if trend_dma else None,
        "trend_psram_first": trend_psram[0] if trend_psram else None,
        "trend_psram_last": trend_psram[-1] if trend_psram else None,
        "trend_stable_internal": trend_stable_internal,
        "log_duration_s": round(duration_s, 1),
        "dma_fail_count": len(RE_DMA.findall(text)),
        "abort_count": len(RE_ABORT.findall(text)),
        "watchdog_count": len(RE_WDT.findall(text)),
        "boot_count": len(RE_BOOT.findall(text)),
        "selected_actions": sorted(set(selected)),
        "selected_sequence": selected,
        "test_console_present": bool(RE_CONSOLE.search(text)),
        "test_commands": [m.group(1) for m in RE_TEST_CMD.finditer(text)],
        "cached_actions": sorted({c[0] for c in cached}),
        "idle_predecoded": bool(RE_PREDEC.search(text)),
        "multinet_released": bool(RE_MN_RELEASE.search(text)),
        "multinet_loaded": bool(RE_MN_LOAD.search(text)),
        "cache_failure_count": len(RE_CACHE_FAIL.findall(text)),
        "speaker_queue_full_count": len(RE_SPEAKER_FULL.findall(text)),
        "asset_error_count": len(RE_ASSET_ERR.findall(text)),
    }


def verdict(m):
    """Return (criterion, status, detail). status: PASS / FAIL / MANUAL / NODATA."""
    out = []

    if m["frames_measured"]:
        ok = m["frame_max_us"] < FRAME_MAX_US
        out.append(("单帧渲染 < 15 ms", "PASS" if ok else "FAIL",
                    f"max={m['frame_max_us']} us, avg={m['frame_avg_us']} us, "
                    f"n={m['frames_measured']}"))
    else:
        out.append(("单帧渲染 < 15 ms", "NODATA",
                    "no 'Frame stats' lines; firmware predates the instrumentation"))

    if m["frames_measured"]:
        ok = m["frame_overrun_count"] == 0 and m["frame_slow_count"] == 0
        out.append(("零掉帧告警", "PASS" if ok else "FAIL",
                    f"overrun={m['frame_overrun_count']}, Frame slow={m['frame_slow_count']}"))
    else:
        ok = m["frame_slow_count"] == 0
        out.append(("零掉帧告警", "PASS" if ok else "FAIL",
                    f"Frame slow={m['frame_slow_count']} (仅超时样本，非全量)"))

    out.append(("无 DMA 分配失败", "PASS" if m["dma_fail_count"] == 0 else "FAIL",
                f"count={m['dma_fail_count']}"))
    out.append(("无崩溃/abort", "PASS" if m["abort_count"] == 0 else "FAIL",
                f"count={m['abort_count']}"))
    out.append(("无看门狗", "PASS" if m["watchdog_count"] == 0 else "FAIL",
                f"count={m['watchdog_count']}"))
    out.append(("无意外重启", "PASS" if m["boot_count"] <= 1 else "FAIL",
                f"boot markers={m['boot_count']}（BOOT 长按 3 s 清配置会主动重启，需人工区分）"))
    out.append(("无素材/解码错误", "PASS" if m["asset_error_count"] == 0 else "FAIL",
                f"count={m['asset_error_count']}"))
    out.append(("常驻帧未降级", "PASS" if m["cache_failure_count"] == 0 else "FAIL",
                f"count={m['cache_failure_count']}"))

    missing = EXPECTED_ACTIONS - set(m["selected_actions"])
    out.append(("五种画面均被选中", "PASS" if not missing else "FAIL",
                f"seen={m['selected_actions'] or '[]'} missing={sorted(missing) or '[]'}"))
    switches = len(m["selected_sequence"])
    out.append(("状态切换 >= 60 次", "PASS" if switches >= MIN_SWITCHES else "FAIL",
                f"Selected 事件={switches}"))

    # Only meaningful for firmware that has the serial test channel. Older logs
    # were driven by hand, so they are neither credited nor penalised here.
    if m["test_console_present"]:
        alias = {"wave": "happy", "jump": "speak"}
        injected = {alias.get(c, c) for c in m["test_commands"]}
        need = {"happy", "speak", "sleep", "listen", "idle"}
        absent = need - injected
        out.append(("脚本刺激可复现覆盖", "PASS" if not absent else "FAIL",
                    f"注入 {len(m['test_commands'])} 条，去重={sorted(injected)}，"
                    f"缺={sorted(absent) or '[]'}"))

    # Judge the SRAM criterion on the *steady state* (Heap trend), not on the
    # boot-time "free internal=" peak. That peak is taken before WakeNet/AFE and
    # the Wi-Fi stack allocate, so it is ~200 KB and would mask the fact that
    # pinning a 16400 B internal DMA draw buffer permanently costs internal RAM.
    if m["heap_trend_samples"]:
        steady = m["trend_internal"]
        steady_max = max(steady)
        improved = steady_max > INTERNAL_TARGET_B
        detail = (f"稳态 internal {min(steady)}..{steady_max} B，目标 > {INTERNAL_TARGET_B} B；"
                  f"旧基线 {BASELINE_INTERNAL[0]}..{BASELINE_INTERNAL[1]} B；"
                  f"开机峰值 {m['internal_free_max']} B（Wi-Fi 分配前，不计入）")
        out.append(("纯 STA 稳态内部 SRAM > 80 KB", "PASS" if improved else "FAIL", detail))
    elif m["internal_free_max"] is not None:
        out.append(("纯 STA 稳态内部 SRAM > 80 KB", "NODATA",
                    f"仅有开机峰值 {m['internal_free_max']} B，无 'Heap trend' 稳态样本"))
    else:
        out.append(("纯 STA 稳态内部 SRAM > 80 KB", "NODATA", "no 'free internal=' line"))

    if m["dma_block_min"] is not None:
        out.append(("DMA 连续块已度量", "PASS",
                    f"min={m['dma_block_min']} B max={m['dma_block_max']} B"))
    else:
        out.append(("DMA 连续块已度量", "NODATA",
                    "固件未输出 dma_block=；无法区分总空闲与可用连续块"))

    if m["heap_trend_samples"] >= 2:
        # Loading MultiNet7 permanently takes ~2.9 MB PSRAM and ~13 KB internal
        # RAM; that is a mode change, not a leak. Judge the drain only over the
        # samples taken after the last load/release, i.e. within one steady mode.
        stable = m["trend_stable_internal"]
        if len(stable) >= 2:
            drop = stable[0] - stable[-1]
            ok = drop <= LEAK_TOLERANCE_B
            out.append(("浸泡期无内部内存泄漏", "PASS" if ok else "FAIL",
                        f"同模式内 internal {stable[0]} -> {stable[-1]} B (drop={drop}), "
                        f"n={len(stable)}；全程 psram {m['trend_psram_first']} -> "
                        f"{m['trend_psram_last']} B（含 MultiNet 加载/释放的模式切换）"))
        else:
            out.append(("浸泡期无内部内存泄漏", "NODATA",
                        "最后一次 MultiNet 加载/释放之后不足 2 条 'Heap trend' 样本"))
    else:
        out.append(("浸泡期无内部内存泄漏", "NODATA", "需要 >=2 条 'Heap trend' 样本"))

    out.append(("开机目视无闪屏/洋红横条", "MANUAL", "需人眼观察屏幕，日志无法判定"))
    out.append(("手机配网后 Qiban 热点消失", "MANUAL", "需手机 Wi-Fi 列表确认，日志仅能佐证 AP 启动"))

    if m["log_duration_s"] >= SOAK_SECONDS:
        ok = m["watchdog_count"] == 0 and m["boot_count"] <= 1
        out.append(("连续 15 分钟稳定", "PASS" if ok else "FAIL",
                    f"duration={m['log_duration_s']}s watchdog={m['watchdog_count']} "
                    f"boots={m['boot_count']}"))
    else:
        out.append(("连续 15 分钟稳定", "NODATA",
                    f"duration={m['log_duration_s']}s < {SOAK_SECONDS}s；需 --seconds 900 抓取"))
    return out


def main():
    # Windows console defaults to GBK and mangles the Chinese criterion labels.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument("log", type=Path)
    ap.add_argument("--json", action="store_true", help="emit machine-readable metrics")
    args = ap.parse_args()
    if not args.log.exists():
        print(f"No such log: {args.log}", file=sys.stderr)
        return 2
    text = args.log.read_text(encoding="utf-8", errors="replace")
    m = analyse(text)
    if args.json:
        print(json.dumps(m, ensure_ascii=False, indent=2))
        return 0
    print(f"== {args.log} ==")
    for k, v in m.items():
        print(f"  {k}: {v}")
    print("\n== Phase 1 acceptance ==")
    rows = verdict(m)
    for name, status, detail in rows:
        print(f"  [{status:6}] {name} — {detail}")
    failed = [r for r in rows if r[1] == "FAIL"]
    nodata = [r for r in rows if r[1] == "NODATA"]
    manual = [r for r in rows if r[1] == "MANUAL"]
    print(f"\nPASS={sum(1 for r in rows if r[1]=='PASS')} FAIL={len(failed)} "
          f"NODATA={len(nodata)} MANUAL={len(manual)}")
    if failed:
        print("RESULT: FAIL")
        return 1
    if nodata:
        print("RESULT: INCOMPLETE (measurable criteria missing data)")
        return 1
    print("RESULT: measurable criteria PASS; MANUAL items still require the board")
    return 0


if __name__ == "__main__":
    sys.exit(main())
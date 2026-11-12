#!/usr/bin/env python3
"""Reproduce every number in results/results.json.

Prerequisites: `make all build/test_main` and the MSR Cambridge traces converted into data/*.fgtr
(see README: scripts/fetch_msr.md + scripts/prep_trace.py). Runs experiments in parallel.
"""
import json
import math
import os
import platform
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.join(ROOT, "build")
TRACES = ["hm_0", "mds_0", "prn_0", "proj_0"]
QUICK = "--quick" in sys.argv


def run(cmd, expect_fail=False):
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if p.returncode != 0 and not expect_fail:
        raise RuntimeError(f"{cmd} failed rc={p.returncode}: {p.stdout[-500:]} {p.stderr[-500:]}")
    return p


def jrun(cmd, expect_fail=False):
    p = run(cmd, expect_fail)
    return json.loads(p.stdout.strip().splitlines()[-1]), p.returncode


def poisson_tail(lam, t):
    """P(X > t) for X ~ Poisson(lam), the simulator's error-count model."""
    return 1.0 - sum(math.exp(-lam) * lam ** k / math.factorial(k) for k in range(t + 1))


def analytic_page_fail(rber, t, dsz=4096, sect=512, meta=28, rbytes=13):
    if t == 0:
        return 1.0 - math.exp(-rber * 8 * (dsz + meta))
    ps = poisson_tail(rber * 8 * (sect + rbytes), t)
    pm = poisson_tail(rber * 8 * (meta + rbytes), t)
    return 1.0 - (1 - ps) ** (dsz // sect) * (1 - pm)


def main():
    os.makedirs(os.path.join(ROOT, "results"), exist_ok=True)
    jobs = {}
    pool = ThreadPoolExecutor(max_workers=int(os.environ.get("JOBS", "6")))

    def sub(key, cmd, expect_fail=False):
        jobs[key] = pool.submit(jrun, cmd, expect_fail)

    traces = [t for t in TRACES if os.path.exists(os.path.join(ROOT, "data", t + ".fgtr"))]
    if QUICK:
        traces = traces[:1]
    # 1. GC policy on real traces (dynamic wear leveling only, so GC is isolated)
    for t in traces:
        for pol in ("greedy", "cb"):
            sub(("wa", t, pol), [f"{B}/fgtrace", "--trace", f"data/{t}.fgtr", "--policy", pol, "--wl", "dynamic", "--op", "0.07"])
    # 2. wear leveling: static data (half the LBAs written once, every later write hits the other half),
    #    90/10 hot/cold, and the hm_0 trace
    n_syn = 600000 if QUICK else 3000000
    syn = {"static_data_50": f"30000,{n_syn},1.0,0.5", "hot_cold_90_10": f"30000,{n_syn},0.9,0.1"}
    for wl in ("none", "dynamic", "static"):
        for name, spec in syn.items():
            sub(("wl", name, wl), [f"{B}/fgtrace", "--synthetic", spec, "--policy", "cb", "--wl", wl,
                                   "--op", "0.10", "--ppb", "64", "--wl-threshold", "32"])
        if "hm_0" in traces:
            sub(("wl", "hm_0", wl), [f"{B}/fgtrace", "--trace", "data/hm_0.fgtr", "--policy", "cb", "--wl", wl,
                                     "--op", "0.07", "--wl-threshold", "32"])
    # 3. power-cut integrity
    scale = 0.1 if QUICK else 1.0
    pc = [
        ("any_op_ckpt128", ["--cuts", str(int(3000 * scale)), "--seed", "11", "--ckpt", "128"]),
        ("torn_program_only", ["--cuts", str(int(1500 * scale)), "--seed", "12", "--ckpt", "128", "--only", "program"]),
        ("partial_erase_only", ["--cuts", str(int(1500 * scale)), "--seed", "13", "--ckpt", "128", "--only", "erase"]),
        ("no_checkpoint", ["--cuts", str(int(1000 * scale)), "--seed", "14", "--ckpt", "0"]),
        ("bit_errors_and_grown_bad_blocks", ["--cuts", str(int(1500 * scale)), "--seed", "15", "--ckpt", "64",
                                             "--rber", "2e-5", "--fail-prog", "0.0002", "--fail-erase", "0.001"]),
    ]
    for name, args in pc:
        sub(("pc", name), [f"{B}/fgverify"] + args)
    # 4. the checker must catch deliberately broken firmware
    for m in (1, 2, 3):
        sub(("mut", m), [f"{B}/fgverify_mut{m}", "--cuts", "300", "--seed", "21", "--ckpt", "128", "--only", "program"], True)
    # 5. ECC
    for rber in (1e-5, 1e-4, 5e-4, 1e-3, 2e-3):
        for t in (0, 8):
            sub(("ecc", rber, t), [f"{B}/fgecc", "--rber", str(rber), "--t", str(t), "--passes", "4"])
    # 6. mount time
    for ck in (0, 4096, 1024):
        sub(("mount", ck), [f"{B}/fgmount", "--ckpt", str(ck)])

    res = {"meta": {"platform": platform.platform(), "machine": platform.machine(), "quick": QUICK,
                    "note": "NAND is simulated; traces are real (MSR Cambridge, SNIA IOTTA). See SPEC.md."}}
    # unit tests
    p = run([f"{B}/test_main"])
    m = re.search(r"(\d+)/(\d+) tests passed", p.stdout)
    res["unit_tests"] = {"passed": int(m.group(1)), "total": int(m.group(2))}

    out = {k: v.result() for k, v in jobs.items()}
    tr = {}
    for t in traces:
        prep = json.load(open(os.path.join(ROOT, "data", t + ".prep.json")))
        g, cb = out[("wa", t, "greedy")][0], out[("wa", t, "cb")][0]
        tr[t] = {"prep": prep, "greedy": g, "cost_benefit": cb,
                 "wa_greedy": round(g["write_amplification"], 3), "wa_cost_benefit": round(cb["write_amplification"], 3),
                 "gc_page_copies_saved_pct": round(100 * (g["gc_writes"] - cb["gc_writes"]) / g["gc_writes"], 1) if g["gc_writes"] else 0.0,
                 "wa_reduction_pct": round(100 * (g["write_amplification"] - cb["write_amplification"]) / g["write_amplification"], 1)}
    res["gc_policy_on_traces"] = tr
    if tr:
        hw = sum(tr[t]["greedy"]["replay_host_writes"] for t in tr)
        res["gc_policy_summary"] = {
            "traces": list(tr), "total_replayed_host_page_writes": hw,
            "wa_greedy_write_weighted": round(sum(tr[t]["greedy"]["nand_programs"] for t in tr) / hw, 3),
            "wa_cost_benefit_write_weighted": round(sum(tr[t]["cost_benefit"]["nand_programs"] for t in tr) / hw, 3),
        }
        s = res["gc_policy_summary"]
        s["wa_reduction_pct_write_weighted"] = round(100 * (s["wa_greedy_write_weighted"] - s["wa_cost_benefit_write_weighted"]) / s["wa_greedy_write_weighted"], 1)
        s["traces_where_cost_benefit_wins"] = [t for t in tr if tr[t]["wa_cost_benefit"] < tr[t]["wa_greedy"]]

    wl = {}
    for w in ("static_data_50", "hot_cold_90_10", "hm_0"):
        if ("wl", w, "none") not in out:
            continue
        d = {k: out[("wl", w, k)][0] for k in ("none", "dynamic", "static")}
        wl[w] = {k: {"max_over_mean": round(d[k]["wear_spread_max_over_mean"], 3), "erase_max": d[k]["erase_max"],
                     "erase_mean": d[k]["erase_mean"], "erase_stdev": d[k]["erase_stdev"],
                     "write_amplification": round(d[k]["write_amplification"], 3), "raw": d[k]} for k in d}
        # same host writes in every run, so the drive's life (first block to hit endurance) scales with 1/erase_max
        wl[w]["peak_wear_reduction_static_vs_none_pct"] = round(100 * (1 - d["static"]["erase_max"] / d["none"]["erase_max"]), 1)
        wl[w]["lifetime_gain_static_vs_none_x"] = round(d["none"]["erase_max"] / d["static"]["erase_max"], 2)
        wl[w]["wa_cost_static_vs_none_pct"] = round(100 * (d["static"]["write_amplification"] / d["none"]["write_amplification"] - 1), 1)
    res["wear_leveling"] = wl

    runs = {name: out[("pc", name)][0] for name, _ in pc}
    res["power_cut"] = {
        "runs": runs,
        "total_cuts": sum(r["cuts"] for r in runs.values()),
        "total_acked_writes": sum(r["acked_writes"] for r in runs.values()),
        "total_acked_data_failures": sum(r["acked_data_failures"] for r in runs.values()),
        "total_mount_failures": sum(r["mount_failures"] for r in runs.values()),
        "total_verify_reads": sum(r["verify_reads"] for r in runs.values()),
        "total_torn_program_cuts": sum(r["cuts_torn_program"] for r in runs.values()),
        "total_partial_erase_cuts": sum(r["cuts_partial_erase"] for r in runs.values()),
    }
    res["checker_fault_injection"] = {
        f"mutant_{m}": {"description": d, "acked_data_failures": out[("mut", m)][0]["acked_data_failures"],
                        "caught": out[("mut", m)][1] != 0}
        for m, d in ((1, "mount forgets blocks that were open at checkpoint time"),
                     (2, "mount trusts the metadata of a possibly torn last page"),
                     (3, "mount keeps the first copy it finds instead of the highest sequence number"))}

    ecc = []
    for rber in (1e-5, 1e-4, 5e-4, 1e-3, 2e-3):
        row = {"rber": rber}
        for t in (0, 8):
            o = out[("ecc", rber, t)][0]
            key = "bch8" if t else "no_ecc"
            row[key] = {"reads": o["reads"], "uncorrectable": o["uncorrectable"], "uncorrectable_rate": o["uncorrectable_rate"],
                        "silent_corruption": o["silent_corruption"], "analytic_rate": analytic_page_fail(rber, t)}
        ecc.append(row)
    res["ecc"] = ecc

    res["mount"] = {f"ckpt_{ck}": out[("mount", ck)][0] for ck in (0, 4096, 1024)}
    full, ck = res["mount"]["ckpt_0"], res["mount"]["ckpt_4096"]
    res["mount"]["speedup_model_ckpt4096_vs_full_scan"] = round(full["mount_model_ms"] / ck["mount_model_ms"], 1)
    res["mount"]["speedup_model_ckpt1024_vs_full_scan"] = round(full["mount_model_ms"] / res["mount"]["ckpt_1024"]["mount_model_ms"], 1)

    # optional: Cortex-M4 code size (cross-compiled only, never run on hardware)
    try:
        run(["make", "arm"])
        size = shutil.which("llvm-size") or shutil.which("arm-none-eabi-size")
        if not size and shutil.which("xcrun"):
            size = run(["xcrun", "--find", "llvm-size"]).stdout.strip()
        objs = [os.path.join(B, f"arm_{n}.o") for n in ("fg_ftl", "fg_bch", "fg_crc32")]
        txt = run([size] + objs).stdout
        tot = {"text": 0, "data": 0, "bss": 0}
        for line in txt.splitlines()[1:]:
            parts = line.split()
            if len(parts) >= 3 and parts[0].isdigit():
                tot["text"] += int(parts[0]); tot["data"] += int(parts[1]); tot["bss"] += int(parts[2])
        res["arm_cortex_m4_code_size"] = {"bytes": tot, "flags": "-mcpu=cortex-m4 -mthumb -Os", "compiler": "clang --target=thumbv7em-none-eabi",
                                          "note": "object sizes only; not linked or run on hardware"}
    except Exception as e:  # noqa: BLE001
        res["arm_cortex_m4_code_size"] = {"error": str(e)[:200]}

    res["notes"] = [
        "Write amplification = NAND page programs / host page writes during trace replay (after a full precondition pass).",
        "Trace pages carry a 64-byte payload in the simulator; mapping/GC behaviour does not depend on payload size.",
        "Error rates, failure probabilities and NAND latencies are illustrative, not calibrated to a real part.",
    ]
    with open(os.path.join(ROOT, "results", "results.json"), "w") as f:
        json.dump(res, f, indent=1)
    print(json.dumps({k: res[k] for k in ("unit_tests", "gc_policy_summary", "power_cut") if k in res}, indent=1)[:3000])


if __name__ == "__main__":
    main()

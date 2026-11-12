# Floatgate

I built the firmware that sits inside an SSD: a flash translation layer (FTL) written in freestanding C the way
controller firmware is written, plus a NAND flash simulator that throws every failure real flash throws at it
(bit errors that grow with wear, bad blocks, program/erase failures, and power loss in the middle of a write).
Then I wrote a host-side test tool that pulls the plug thousands of times and proves that every write the drive
acknowledged is still there after reboot.

Named after the floating-gate transistor that stores each bit in NAND flash.

## Why this is hard
NAND flash cannot overwrite in place. A page (4 KiB) can be programmed once; to reuse it you erase the whole
block (hundreds of pages), and each block survives only a few thousand erases. Reads come back with raw bit
errors. Power can disappear at any moment, including halfway through programming a page. The FTL hides all of
that behind "read/write logical block N", and it has to do it in a few KB of code with fixed RAM.

## What is in here
| Path | What |
|---|---|
| `fw/fg_ftl.c` | The firmware. Page-mapped L2P table, valid bitmap, per-block erase/valid counts. Separate append streams for host writes, GC, cold data and checkpoints. Greedy or cost-benefit GC, dynamic + static wear leveling, bad-block retirement, read scrub, checkpoint + crash-recovery mount. |
| `fw/fg_bch.c` | BCH code over GF(2^13): corrects 8 bit errors per 512-byte sector with 13 parity bytes (byte-wise LFSR encoder, syndromes from the remainder, Berlekamp-Massey, Chien search). |
| `fw/fg_nand.h` | The hardware abstraction the firmware talks to: read / program / erase + status FAIL bit. |
| `sim/nandsim.c` | NAND simulator: program-once and in-order rules, Poisson bit errors (wear + retention), factory and grown bad blocks, power cuts at any NAND operation (before the command latches, mid-program leaving a torn page, mid-erase leaving a half-erased block). |
| `tools/fgverify.c` | The host-side integrity checker: random workload, random power cut, wipe all firmware RAM, remount, read back every logical page. |
| `tools/fgtrace.c` | Replays real block I/O traces (or synthetic workloads) and reports write amplification and wear. |
| `tools/fgecc.c`, `tools/fgmount.c` | ECC and mount-time benchmarks. |

Firmware rules I held myself to: C99 with `-ffreestanding -Wall -Wextra -Werror`, no libc, no `malloc` (every
table is carved from one RAM arena at init; `make fwcheck` fails if any firmware object references an allocator
or stdio symbol), deterministic seeds everywhere, and a write is only acknowledged after its NAND program reports
success.

## How recovery works
Every page's spare area carries the logical page number, a 64-bit sequence number and the block's erase count,
protected by their own BCH codeword and a CRC. Every N host writes the firmware writes a checkpoint (map +
erase counts + bad-block list) as a chain of pages whose last page is the commit record. On power-up, mount:

1. reads page 0's spare area of every block to classify it (free, data, checkpoint, unreadable);
2. loads the newest *complete* checkpoint (a checkpoint cut off mid-write is skipped);
3. replays only blocks written after that checkpoint plus the blocks it recorded as open; highest sequence
   number wins for each logical page;
4. fully verifies (data + CRC) the last programmed page of each replayed block, because that is the only page a
   power cut can tear.

To check that the checker is not just passing everything, I built three deliberately broken firmware variants
(`make mutants`): one forgets the blocks open at checkpoint time, one trusts a possibly torn last page, one keeps
the first copy found instead of the newest. The checker catches all three.

## Results
All numbers come from `results/results.json`, produced by `scripts/run_all.py` (deterministic seeds).

**Garbage collection on real traces** (MSR Cambridge, SNIA IOTTA; drive preconditioned full, 7% over-provisioning,
256 pages/block). WA = NAND page programs / host page writes.

| Trace | Footprint (4 KiB pages) | Host page writes replayed | WA greedy | WA cost-benefit | Reduction |
|---|---|---|---|---|---|
| hm_0 | 428,184 | 6,051,575 | 2.475 | 2.287 | 7.6% |
| mds_0 | 86,930 | 2,052,937 | 2.698 | 2.313 | 14.3% |
| prn_0 | 3,247,570 | 14,144,956 | 1.501 | 1.417 | 5.6% |
| proj_0 | 433,489 | 37,972,094 | 1.369 | 1.164 | 15.0% |
| **all 4, write-weighted** | | **60,221,562** | **1.557** | **1.375** | **11.7%** |

Every replay also read back every page at the end: 0 mismatches.

**Wear leveling** (max erase count / mean erase count; lower is better. "none" = round-robin allocation).

| Workload | none | dynamic | dynamic + static | Peak wear cut | WA cost of static WL |
|---|---|---|---|---|---|
| hm_0 trace (real) | 2.814 | 2.264 | 1.617 | 41.4% (max erases 87 to 51) | +2.0% |
| static data: half the LBAs never rewritten (synthetic) | 1.816 | 1.801 | 1.056 | 33.0% | +15.3% |
| 90/10 hot/cold (synthetic) | 1.172 | 1.020 | 1.020 | 12.9% | +0.1% |

In the 90/10 case static WL never triggers because dynamic WL already keeps the gap under the threshold.

**Power-loss integrity** (128 blocks x 64 pages x 2 KiB, 70% full, hot/cold random workload)

| Run | Power cuts | Acknowledged writes | Lost or corrupted acknowledged writes |
|---|---|---|---|
| any NAND op, checkpoint every 128 writes | 3,000 | 1,332,650 | 0 |
| cuts only during programs (torn pages) | 1,500 | 206,235 | 0 |
| cuts only during erases (half-erased blocks) | 1,500 | 226,740 | 0 |
| no checkpoint (full-scan mount) | 1,000 | 499,026 | 0 |
| bit errors (RBER 2e-5) + program/erase failures | 1,500 | 147,771 | 0 |
| **total** | **8,500** | **2,412,422** | **0** |

After every cut the checker read back all 5,734 logical pages (48,739,000 verification reads in total).
Fault-injected firmware: mutant 1 lost or corrupted 11,751 acknowledged pages, mutant 2 23, mutant 3 8,817; all
three runs exit non-zero.

**ECC** (4 KiB page + 224-byte spare, 12,000 page reads per point through the full firmware read path)

| Raw bit error rate | Uncorrectable pages, no ECC (CRC only) | Uncorrectable pages, BCH-8 | BCH-8 analytic |
|---|---|---|---|
| 1e-5 | 27.8% | 0 / 12,000 | ~0 |
| 1e-4 | 96.8% | 0 / 12,000 | 6.2e-9 |
| 5e-4 | 100% | 0.20% | 0.27% |
| 1e-3 | 100% | 19.4% | 20.3% |
| 2e-3 | 100% | 99.1% | 99.3% |

Silent corruption (a read that returned wrong data as good) was 0 in every configuration: when ECC fails or is
off, the CRC turns it into an I/O error instead.

**Mount after an unclean shutdown** (256 blocks x 128 pages x 4 KiB, modelled with tR 50 us and a 400 MB/s bus)

| Mount strategy | Spare-area reads | Modelled NAND time | Extra page programs per host write |
|---|---|---|---|
| full scan, no checkpoint | 37,508 | 1,911 ms | 0 |
| checkpoint every 4,096 writes | 9,053 | 463 ms (4.1x faster) | 0.6% |
| checkpoint every 1,024 writes | 1,938 | 100 ms (19.0x faster) | 2.5% |

**Code size:** the three firmware objects cross-compile to 9,660 bytes of `.text` for a Cortex-M4
(`clang --target=thumbv7em-none-eabi -Os`). That is a size number only; I have not run it on a microcontroller.

## Run it
```
make test                 # 24 unit/integration tests + 3 power-cut runs (ASan/UBSan on Linux) + fwcheck
make all mutants
scripts/fetch_msr.sh msr1.sh   # 4 MSR traces (~155 MB) by byte range, after accepting the SNIA license
python3 scripts/run_all.py     # regenerates results/results.json (a few minutes on 8 cores)
make arm                  # optional Cortex-M4 cross-compile
```
`python3 scripts/run_all.py --quick` runs a reduced set without the large traces.

## Limits (honest)
- The NAND is simulated. Bit-error, failure and latency parameters are illustrative, not fitted to a real part,
  and the simulator behaves like SLC: it does not model MLC/TLC paired-page corruption, where a cut while
  programming an upper page can damage an already-acknowledged lower page.
- In trace replays each 4 KiB logical page carries a 64-byte payload so multi-GB devices fit in RAM; mapping and
  GC behaviour do not depend on payload size, but ECC/CRC cost per page does.
- The traces are a 4-volume subset of MSR Cambridge; only pages the trace writes are replayed (reads of
  never-written pages are dropped), and the logical capacity equals the trace's written footprint.
- There is no write cache and no TRIM, and the firmware is single-threaded (no multi-plane or multi-die
  parallelism), so this measures FTL policy, not throughput.
- Power cuts are injected at NAND-operation granularity. Firmware RAM is discarded on every cut, so only NAND
  operations change persistent state; cutting between two CPU instructions is equivalent to cutting before the
  next NAND operation.
- Mount time is modelled from NAND operation counts; at the end of a drive's life (many grown bad blocks) the
  checkpoint can fail to find room and mount falls back toward a full scan.
- Sanitizer runs happen in CI on Linux; the macOS sandbox I developed in could not execute sanitizer binaries.

Traces: Dushyanth Narayanan, Austin Donnelly, Antony Rowstron, "Write Off-Loading: Practical Power Management for
Enterprise Storage", FAST 2008. Distributed by SNIA IOTTA under the SNIA trace data license.

# Floatgate spec

## Problem
An SSD is a microcontroller running firmware in front of raw NAND flash. NAND cannot overwrite in place
(program once, erase a whole block), wears out after a few thousand erase cycles, returns raw bit errors
that grow with wear and age, ships with bad blocks and grows more, and loses power whenever it likes.
The flash translation layer (FTL) is the firmware that hides all of this behind "read/write logical block N".
Floatgate is that firmware, written in freestanding C the way controller firmware is written, and tested
against a NAND simulator that injects every one of those failure modes.

## Why it matters to the target team
Western Digital's firmware, system-tools and systems-integration interns write and debug exactly this layer:
embedded C against a hardware interface, error-correction and bad-block handling, power-loss recovery, and the
host-side test tools that prove acknowledged data survives.

## Design
- `fw/` firmware: freestanding C99, `-Wall -Wextra -Werror`, no libc, no malloc. All tables are carved from one
  RAM arena at init. Talks to flash only through a HAL (`fg_nand.h`: read / program / erase + status).
  - Page-level L2P map, valid bitmap, per-block valid count / erase count / state.
  - Separate append streams for host writes, GC relocations, static-WL cold data and checkpoints.
  - GC victim selection: greedy (fewest valid pages) or cost-benefit ((1-u)*age/(1+u)).
  - Wear leveling: dynamic (allocate least-worn free block) and static (move the coldest block's data onto
    the most-worn free block when the erase-count gap exceeds a threshold).
  - Bad blocks: factory markers scanned at format; program/erase FAIL status retires the block after its
    valid pages are relocated; retired blocks are persisted in the checkpoint.
  - ECC: BCH over GF(2^13), t = 8 per 512-byte sector (13 parity bytes) plus a separate codeword over the
    28-byte page metadata; CRC-32 after ECC catches miscorrection and torn pages.
  - Power loss: every page carries LPN, a 64-bit sequence number and erase count in its spare area. Mount
    loads the newest complete checkpoint, then replays only blocks written after it (plus the blocks the
    checkpoint recorded as open), highest sequence number wins, and the last programmed page of each replayed
    block gets a full data+CRC check because only that page can be torn.
- `sim/` NAND simulator (host C): program-once and in-order rules, Poisson raw bit errors with
  RBER = base + wear*(PE/endurance)^2 + retention*age*(1+PE/endurance), factory/grown bad blocks, and power
  cuts at any NAND operation (before the command latches, mid-program leaving a torn page, mid-erase leaving a
  partially erased block, during reads). The cut is a longjmp; the harness then wipes all firmware RAM.
- `tools/`: `fgverify` (power-cut integrity checker), `fgtrace` (trace replay), `fgecc`, `fgmount`.

## Metrics and baselines
1. Write amplification on real traces (MSR Cambridge, SNIA IOTTA: hm_0, mds_0, prn_0, proj_0): cost-benefit
   GC vs greedy GC, 7% over-provisioning, device preconditioned full. WA = NAND page programs / host page writes.
2. Wear spread (max/mean erase count) with no wear leveling (round-robin allocation) vs dynamic vs
   dynamic+static, synthetic 90/10 hot/cold workload. Also the WA cost of static WL.
3. Data integrity across thousands of random power cuts: acknowledged writes lost or corrupted, target 0.
   The checker is itself validated with fault-injected firmware builds that must be caught.
4. Uncorrectable page-read rate at fixed raw bit error rates with BCH-8 vs no ECC (CRC-only detection),
   compared with the analytic binomial tail.
5. Mount/recovery cost after an unclean shutdown: full spare-area scan vs checkpoint + delta scan, in NAND
   reads and modelled NAND busy time.

## What is simulated
The NAND is simulated. Error rates, failure probabilities and latencies (tR 50 us, tPROG 500 us, tBERS 3.5 ms,
400 MB/s bus) are illustrative, not calibrated to a specific part. The workloads are real block traces, but in
trace runs each 4 KiB logical page carries a 64-byte payload so a multi-GB device fits in host RAM; mapping and
GC behaviour do not depend on payload size. The firmware is cross-compiled for Cortex-M4 for a code-size number
only; it has not run on real hardware.

#!/usr/bin/env python3
"""Convert an MSR Cambridge block trace (csv.gz) into Floatgate's compact replay format.

Each request is split into 4 KiB logical pages. Pages that are written at least once are
renumbered densely in first-touch order (the exported logical capacity = the trace's written
footprint); reads of pages the trace never writes are dropped because the replay has no data
for them. Output: b"FGTR", u32 nlpn, u64 nops, then nops little-endian u32 records with
bit 31 = write.
"""
import array
import gzip
import json
import struct
import sys

PAGE = 4096


def main(src, dst):
    reqs = []
    with gzip.open(src, "rt") as f:
        for line in f:
            p = line.split(",")
            off, size = int(p[4]), int(p[5])
            a = off // PAGE
            b = (off + max(size, 1) - 1) // PAGE
            reqs.append((p[3][0] == "W", a, b))
    remap = {}
    for w, a, b in reqs:
        if w:
            for pg in range(a, b + 1):
                if pg not in remap:
                    remap[pg] = len(remap)
    out = array.array("I")
    reads_dropped = 0
    writes = reads = 0
    for w, a, b in reqs:
        for pg in range(a, b + 1):
            l = remap.get(pg)
            if l is None:
                reads_dropped += 1
                continue
            if w:
                out.append(l | 0x80000000)
                writes += 1
            else:
                out.append(l)
                reads += 1
    with open(dst, "wb") as f:
        f.write(b"FGTR" + struct.pack("<IQ", len(remap), len(out)))
        out.tofile(f)
    print(json.dumps({"trace": src.split("/")[-1], "requests": len(reqs), "footprint_pages": len(remap),
                      "page_writes": writes, "page_reads": reads, "reads_dropped_unwritten": reads_dropped,
                      "writes_per_footprint_page": round(writes / len(remap), 2)}))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

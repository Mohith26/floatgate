#!/usr/bin/env bash
# Fetch 4 of the 36 MSR Cambridge volumes (hm_0, mds_0, prn_0, proj_0; ~155 MB compressed)
# without downloading the full 3.3 GB archive.
#
# 1. Accept the SNIA trace license at https://iotta.snia.org/traces/block-io/388
#    ("MSR Cambridge Traces 1" -> "Download via Unix shell script"). The generated script
#    contains a short cookie block (lines starting with ".iotta.snia.org").
# 2. Save that script as msr1.sh next to this one and run: scripts/fetch_msr.sh msr1.sh
#
# The archive is an uncompressed tar served with HTTP range support, so each member is
# fetched by byte offset (offsets read from the tar headers; see README).
set -euo pipefail
script="${1:?path to the SNIA-generated download script}"
mkdir -p data
cookies=$(mktemp)
grep '^\.iotta\.snia\.org' "$script" > "$cookies"
URL="http://server3.iotta.snia.org/traces/block-io/386/download?type=file&mirror_chosen=true&sType=curl"
get() { # name offset size
  curl -s -f -b "$cookies" -L -r "$2-$(($2 + $3 - 1))" "$URL" -o "data/$1"
  gzip -t "data/$1" && echo "ok $1"
}
get hm_0.csv.gz   7680      41967571
get mds_0.csv.gz  47112704  13389472
get prn_0.csv.gz  79447040  44469556
get proj_0.csv.gz 249739776 54999265
rm -f "$cookies"
for t in hm_0 mds_0 prn_0 proj_0; do
  python3 scripts/prep_trace.py "data/$t.csv.gz" "data/$t.fgtr" > "data/$t.prep.json"
done

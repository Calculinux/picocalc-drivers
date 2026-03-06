#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# extract-overlay-symbols.sh – Scan device tree overlay sources and emit the
# set of base-DTB symbols (phandle labels) they reference.
#
# The output is a sorted, unique, one-per-line list of DT label names that
# must appear in the base DTB's __symbols__ node for runtime ConfigFS
# overlays to resolve their phandle fixups.
#
# Algorithm:
#   1. Strip C-style block and line comments from every *-overlay.dts file.
#   2. Collect every &label phandle reference.
#   3. Collect every label: definition (labels defined WITHIN the overlays).
#   4. Subtract local definitions from references – what remains are symbols
#      that must come from the base DTB.
#
# Usage:
#   ./scripts/extract-overlay-symbols.sh [overlay-dir]
#
# Default overlay-dir: devicetree-overlays  (relative to repo root)
# Output: stdout, one symbol per line.  Pipe to a file as needed.

set -euo pipefail

OVERLAY_DIR="${1:-devicetree-overlays}"

if [ ! -d "${OVERLAY_DIR}" ]; then
    echo "Error: overlay directory '${OVERLAY_DIR}' not found" >&2
    exit 1
fi

# Gather overlay sources
OVERLAYS=()
for f in "${OVERLAY_DIR}"/*-overlay.dts; do
    [ -f "$f" ] && OVERLAYS+=("$f")
done

if [ ${#OVERLAYS[@]} -eq 0 ]; then
    echo "# No overlay DTS files found in ${OVERLAY_DIR}" >&2
    exit 0
fi

# Temporary files
TMP_REFS=$(mktemp)
TMP_DEFS=$(mktemp)
trap 'rm -f "$TMP_REFS" "$TMP_DEFS" "${TMP_REFS}.sorted" "${TMP_DEFS}.sorted"' EXIT

for f in "${OVERLAYS[@]}"; do
    # Strip block comments (/* ... */), then line comments (// ...)
    STRIPPED=$(perl -0777 -pe 's|/\*.*?\*/||gs; s|//[^\n]*||g' "$f")

    # Collect &label phandle references (strip leading &)
    echo "$STRIPPED" | grep -oP '&\K\w+' >> "$TMP_REFS" || true

    # Collect label definitions – pattern: word immediately before ": " (colon
    # then whitespace), which is how DTS labels are written.  This does NOT
    # match property assignments (key = value) or compatible strings.
    echo "$STRIPPED" | grep -oP '\w+(?=\s*:\s)' >> "$TMP_DEFS" || true
done

sort -u "$TMP_REFS"  > "${TMP_REFS}.sorted"
sort -u "$TMP_DEFS"  > "${TMP_DEFS}.sorted"

# References that are NOT locally defined = base DTB symbols needed
comm -23 "${TMP_REFS}.sorted" "${TMP_DEFS}.sorted"

#!/usr/bin/env bash
# Regenerates every figure in docs/images. Run from any directory.
# Needs: Python 3 with playwright (Chromium) and Pillow, a C++17 compiler.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
images="$(dirname "$here")"
root="$(cd "$images/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

# Diagrams and charts (SVG)
for f in fig_arch fig_rx fig_reconf fig_wire fig_clock fig_results; do
    PYTHONPATH="$here" python3 "$here/$f.py"
done
cp ./*.svg "$images/"

# Terminal captures: the CLI's own presentation code renders the views,
# ansi2png.py draws them in a Windows Terminal (Campbell) frame.
"${CXX:-g++}" -std=c++17 -I"$root/include" -I"$root/src" "$here/render.cpp" -o render
for view in info running summary status find; do ./render "$view" > "$view.ansi"; done
python3 "$here/make_hwtest.py"
shot() { python3 "$here/ansi2png.py" "$1" "$images/$2" "Windows PowerShell" "$3" "${4:-0}"; }
shot info.ansi    cli-info.png          80
shot running.ansi cli-bench-running.png 80
shot summary.ansi cli-bench-summary.png 80
shot status.ansi  cli-status.png        80 44
shot find.ansi    cli-find-error.png    80
shot hwtest.ansi  cli-hwtest.png        128
echo "Figures written to $images"

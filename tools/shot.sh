#!/bin/sh
# Screenshot a page of the receiver's web UI with headless Chrome.
#   tools/shot.sh <host> [page] [out.png] [width]     e.g. tools/shot.sh 10.0.0.18 status /tmp/status.png
host=$1; page=${2:-status}; out=${3:-/tmp/airmidi-$page.png}; width=${4:-1100}
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --disable-gpu --hide-scrollbars \
  --window-size=$width,900 --virtual-time-budget=4000 --screenshot="$out" "http://$host/#$page" 2>/dev/null
echo "wrote $out"

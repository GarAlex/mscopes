#!/usr/bin/env bash
#
# Stream (or dump) the spike plugin's os_log output.
#
# The plugin logs to subsystem "com.writea.viz.musicplugin". Because it runs inside
# Music (a sandboxed process), os_log is the reliable channel — not stdout/files.
#
#   ./logs.sh          # live stream
#   ./logs.sh show     # dump the last 10 minutes and exit
#
set -euo pipefail
PRED='subsystem == "com.writea.viz.musicplugin"'
if [[ "${1:-}" == "show" ]]; then
  exec log show --last 10m --predicate "$PRED" --style compact --info
else
  echo ">> streaming com.writea.viz.musicplugin  (Ctrl-C to stop)"
  exec log stream --predicate "$PRED" --style compact --level info
fi

#!/bin/sh
HERE="$(cd "$(dirname "$0")" && pwd)"
LAUNCHER_DIR="$HERE" PAYLOAD=inotia4_nx exec "$HERE/../runtime/launcher/build.sh" "$@"

#!/usr/bin/env bash
# Send a build to a 3DS running the Homebrew Launcher's netloader and keep listening: the game's log comes back over
# the network (port/3ds/console.c redirects it when sent this way) and is printed here and saved to build/3ds-log.txt.
#   tools/3dslink.sh IP [build/ff9.3dsx]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IP=${1:?usage: tools/3dslink.sh IP [file.3dsx]}
APP=${2:-build/ff9.3dsx}
cd "$ROOT" && FF9DS_DOCKER_ARGS="--network host" tools/docker.sh 3dslink -s -a "$IP" "$APP" 2>&1 | tee build/3ds-log.txt

#!/bin/sh
# Verify a klogg Ubuntu .deb installs and smoke-runs on a given Ubuntu base image.
#
# Usage: scripts/verify_ubuntu_deb.sh <path-to.deb> <ubuntu-base-tag>
#
# Runs the exact check_command the ubuntu-*-deb legs of ci-build.yml use:
#   apt-get update; apt-get install -y /usr/local/<deb>; QT_QPA_PLATFORM=offscreen klogg -v
#
# Exit status is the container's exit status, so it can be used as a gate:
#   scripts/verify_ubuntu_deb.sh klogg-*-noble-vs-gen.deb ubuntu:26.04   # fails (t64 renames)
#   scripts/verify_ubuntu_deb.sh klogg-*-resolute-vs-gen.deb ubuntu:26.04 # passes
set -eu

DEB=${1:?usage: verify_ubuntu_deb.sh <path-to.deb> <ubuntu-base-tag>}
BASE=${2:?usage: verify_ubuntu_deb.sh <path-to.deb> <ubuntu-base-tag>}

DEB_ABS=$(cd "$(dirname "$DEB")" && pwd)/$(basename "$DEB")
DEB_NAME=$(basename "$DEB_ABS")

test -f "$DEB_ABS" || { echo "error: deb not found: $DEB_ABS" >&2; exit 2; }

echo "==> installing $DEB_NAME on $BASE (apt + offscreen klogg -v)"
# $DEB_NAME is passed as a positional parameter ($1) instead of being
# interpolated into the bash -c script: a filename containing shell syntax
# must stay literal data, never executable code. The mount is read-only —
# the container only reads the deb (apt/klogg write to the container FS).
docker run --rm -v "$(dirname "$DEB_ABS")":/usr/local:ro \
  "$BASE" /bin/bash -c '
    for attempt in 1 2 3; do
      apt-get update -o Acquire::Retries=3 -o APT::Update::Error-Mode=any && break
      if [ "$attempt" = 3 ]; then exit 1; fi
      sleep $((attempt * 10))
    done
    DEBIAN_FRONTEND=noninteractive apt-get install -y "/usr/local/$1"
    QT_QPA_PLATFORM=offscreen klogg -v
  ' verify_ubuntu_deb "$DEB_NAME"

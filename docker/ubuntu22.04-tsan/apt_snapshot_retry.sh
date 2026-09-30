#!/bin/sh
# Retry one complete transaction against the caller's pinned snapshot sources.
# With no package arguments, only refresh and validate the package indexes.
set -eu

snapshot_apt_get() {
    apt-get \
        -o Acquire::Retries=5 \
        -o Acquire::http::Timeout=30 \
        -o Acquire::https::Timeout=30 \
        -o APT::Update::Error-Mode=any \
        "$@"
}

max_attempts=5
attempt=1
while :; do
    printf 'Snapshot APT attempt %s/%s\n' "$attempt" "$max_attempts" >&2
    # Discard partial/stale indexes, not downloaded packages or dpkg state.
    rm -rf -- /var/lib/apt/lists/*
    status=0
    snapshot_apt_get update -y || status=$?
    if [ "$status" -eq 0 ] && [ "$#" -gt 0 ]; then
        snapshot_apt_get install --no-install-recommends -y "$@" || status=$?
    fi
    if [ "$status" -eq 0 ]; then
        exit 0
    fi

    printf 'Snapshot APT attempt %s/%s failed with status %s\n' \
        "$attempt" "$max_attempts" "$status" >&2
    if [ "$attempt" -eq "$max_attempts" ]; then
        exit "$status"
    fi
    sleep "$((attempt * 15))"
    attempt=$((attempt + 1))
done

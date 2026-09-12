#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

image=${1:?usage: smoke_container.sh IMAGE}
name="shadnet-smoke-${RANDOM}-$$"
volume="${name}-data"
cleanup() {
    docker logs "$name" 2>&1 || true
    docker rm -f "$name" >/dev/null 2>&1 || true
    docker volume rm "$volume" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker volume create "$volume" >/dev/null
start() {
    # No host networking, published ports, or production volumes.
    docker run -d --name "$name" --network none \
        --health-interval 1s --health-start-period 1s \
        -v "$volume:/data" \
        -e SHADNET_MEMBER_API_KEY=smoke-member \
        -e SHADNET_ADMIN_API_KEY=smoke-admin \
        -e SHADNET_ICE_TURN_HOST="$1" "$image" >/dev/null
    for ((attempt = 0; attempt < 60; attempt++)); do
        if [[ $(docker inspect -f '{{.State.Health.Status}}' "$name") == healthy ]]; then
            return
        fi
        sleep 1
    done
    echo 'Container did not become healthy' >&2
    return 1
}

start relay.example.test
[[ $(docker exec "$name" id -u) == 10001 ]]
docker exec "$name" sh -ec '
    test -s /data/db/shadnet.db
    test -s /data/worlds.cfg
    test -s /data/scoreboards.cfg
    grep -qx "BloodborneSeamlessCoop=false" /data/shadnet.cfg
    grep -qx "IceTurnHost=relay.example.test" /data/shadnet.cfg
    echo persistent > /data/smoke-marker
'
# A container replacement must use the saved configuration, even if the new
# environment specifies a different value.
docker stop -t 2 "$name" >/dev/null
docker rm "$name" >/dev/null
start ignored.example.test
docker exec "$name" sh -ec '
    grep -qx persistent /data/smoke-marker
    grep -qx "IceTurnHost=relay.example.test" /data/shadnet.cfg
'
if docker logs "$name" 2>&1 | grep -q 'Applied database migration'; then
    echo 'Existing database was migrated again' >&2
    exit 1
fi
echo 'Container startup and persistent-volume checks passed'

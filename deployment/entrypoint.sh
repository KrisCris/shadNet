#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
# Prepare /data and hand over to shadnet.
#
# shadNet resolves every relative path against its own binary's directory
# (QDir::setCurrent(applicationDirPath()) in main.cpp), so /opt/shadnet gets
# symlinks pointing at the persistent volume.
set -eu

APP_DIR=/opt/shadnet
DATA_DIR=${SHADNET_DATA_DIR:-/data}

log() { printf '%s  %s\n' "$(date -u +%H:%M:%S)" "$*"; }

mkdir -p "$DATA_DIR/db" "$DATA_DIR/score_data"

# Seed configuration that ships with the build, but never overwrite a file the
# operator has already edited.
for f in worlds.cfg scoreboards.cfg; do
    if [ ! -e "$DATA_DIR/$f" ] && [ -e "$APP_DIR/defaults/$f" ]; then
        cp "$APP_DIR/defaults/$f" "$DATA_DIR/$f"
        log "seeded $f"
    fi
done

# Generate shadnet.cfg on first run only. After that the file is yours: edit it
# in the volume and restart. Environment variables are deliberately NOT applied
# to an existing file, so a container restart can never silently revert a
# hand-made change.
CFG="$DATA_DIR/shadnet.cfg"
if [ ! -e "$CFG" ]; then
    log "no shadnet.cfg found, generating one from the environment"
    cat > "$CFG" <<EOF
[General]
Host=${SHADNET_HOST:-0.0.0.0}
UnsecuredPort=${SHADNET_TCP_PORT:-31313}
WebApiPort=${SHADNET_WEBAPI_PORT:-31315}
StatsEnabled=${SHADNET_STATS_ENABLED:-false}
Matching2Enabled=${SHADNET_MATCHING2_ENABLED:-true}
BloodborneSeamlessCoop=${SHADNET_SEAMLESS_COOP:-false}
StatsPort=${SHADNET_STATS_PORT:-31320}
StatsPath=${SHADNET_STATS_PATH:-stats}
StatsCacheLife=${SHADNET_STATS_CACHE_LIFE:-30}
EmailValidated=${SHADNET_EMAIL_VALIDATED:-false}
AdminsList=${SHADNET_ADMINS_LIST:-}
RegistrationSecretKey=${SHADNET_REGISTRATION_KEY:-}
MemberApiEnabled=true
MemberApiHost=${SHADNET_MEMBER_API_HOST:-127.0.0.1}
MemberApiPort=${SHADNET_MEMBER_API_PORT:-31360}
MemberApiKey=${SHADNET_MEMBER_API_KEY:-}
AdminApiEnabled=true
AdminApiHost=${SHADNET_ADMIN_API_HOST:-127.0.0.1}
AdminApiPort=${SHADNET_ADMIN_API_PORT:-31350}
AdminApiKey=${SHADNET_ADMIN_API_KEY:-}
ApiTrustedProxies=${SHADNET_API_TRUSTED_PROXIES:-local}
IceStunHost=${SHADNET_ICE_STUN_HOST:-}
IceStunPort=${SHADNET_ICE_STUN_PORT:-31478}
IceTurnHost=${SHADNET_ICE_TURN_HOST:-}
IceTurnPort=${SHADNET_ICE_TURN_PORT:-31478}
IceTurnSecret=${SHADNET_ICE_TURN_SECRET:-}
IceTurnTtlSeconds=${SHADNET_ICE_TURN_TTL_SECONDS:-3600}
EOF
fi

# Point the app directory at the volume. Recreated every start so an image
# upgrade cannot leave a stale link behind.
for name in shadnet.cfg worlds.cfg scoreboards.cfg domains_banlist.txt db score_data; do
    rm -rf "$APP_DIR/$name"
    ln -s "$DATA_DIR/$name" "$APP_DIR/$name"
done

if [ -f "$APP_DIR/build-info.txt" ]; then
    log "build info:"
    sed 's/^/    /' "$APP_DIR/build-info.txt"
fi

log "data directory: $DATA_DIR"
log "listening per $CFG:"
grep -E '^(Host|UnsecuredPort|WebApiPort|MemberApiPort|AdminApiPort)=' "$CFG" | sed 's/^/    /'

# The web frontend authenticates with these; if they are blank the server
# generates its own on first start and nothing else will know them.
for k in MemberApiKey AdminApiKey; do
    if ! grep -qE "^$k=.+" "$CFG"; then
        log "WARNING: $k is empty in $CFG. The server will generate one and"
        log "         write it back; copy that value to the web frontend, or"
        log "         set it in .env before the first start."
    fi
done

cd "$APP_DIR"
exec ./shadnet "$@"

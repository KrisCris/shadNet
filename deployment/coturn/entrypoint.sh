#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu
# The image runs as nobody, which can write to /tmp and nowhere else.
umask 077
if [ -z "${TURN_SECRET:-}" ]; then
  echo 'SHADNET_ICE_TURN_SECRET is empty. Set it in .env (openssl rand'
  echo '-hex 32) -- coturn and shadNet must agree on it or every'
  echo 'allocation is rejected.' >&2
  exit 1
fi
cp /etc/coturn/turnserver.conf /tmp/turnserver.conf
# Host networking means every interface on the machine, and a box
# that runs other containers has a lot of them. Left unset, coturn
# binds all of them and can hand a player a relay address on a
# Docker bridge -- an address that resolves, answers locally, and is
# unreachable from anywhere the player is. Naming the interfaces to
# use is a LAN address, which is stable, unlike the public one.
for ip in ${TURN_LISTEN_IP:-}; do
  echo "listening-ip=$ip" >> /tmp/turnserver.conf
  echo "relay-ip=$ip" >> /tmp/turnserver.conf
done
echo "static-auth-secret=$TURN_SECRET" >> /tmp/turnserver.conf
# The advertised address may be given as a literal IP or as a name.
# A name is the useful case on a dynamic address: it is resolved here,
# at start-up, and re-checked below.
#
# Resolved as A by default, deliberately. external-ip exists only to
# work around NAT, and IPv6 here has none -- the host has a global
# address and coturn's own socket address is already correct for it.
# Set SHADNET_TURN_EXTERNAL_FAMILY=ipv6 only if you know the name's
# AAAA points at this machine and coturn listens on that address.
resolve_ext() {
  case "${TURN_EXTERNAL_FAMILY:-ipv4}" in
    ipv6) getent ahostsv6 "$1" 2>/dev/null | awk '$2=="STREAM"{print $1; exit}' ;;
    *)    getent ahostsv4 "$1" 2>/dev/null | awk '$2=="STREAM"{print $1; exit}' ;;
  esac
}
# An IPv4 dotted quad has only digits and dots; anything with a colon
# is an IPv6 literal. Everything else is treated as a name to resolve.
is_ip_literal() {
  case "$1" in
    *:*) return 0 ;;
    *[!0-9.]*) return 1 ;;
    *) return 0 ;;
  esac
}
EXT=""
WATCH_NAME=""
if [ -n "${TURN_EXTERNAL_IP:-}" ]; then
  if is_ip_literal "$TURN_EXTERNAL_IP"; then
    EXT="$TURN_EXTERNAL_IP"
    echo "advertising external-ip=$EXT (literal)"
  else
    WATCH_NAME="$TURN_EXTERNAL_IP"
    # DNS may not be up in the first moment after a host reboot, so
    # give it a few tries before giving up on this start.
    n=0
    while [ "$n" -lt 5 ]; do
      EXT=$(resolve_ext "$WATCH_NAME")
      if [ -n "$EXT" ]; then
        break
      fi
      n=$((n + 1))
      sleep 2
    done
    if [ -n "$EXT" ]; then
      echo "advertising external-ip=$EXT (resolved $WATCH_NAME, ${TURN_EXTERNAL_FAMILY:-ipv4})"
    else
      echo "WARNING: could not resolve $WATCH_NAME. Starting without an" >&2
      echo "         advertised address; the re-check below will restart" >&2
      echo "         coturn once the name resolves." >&2
    fi
  fi
else
  echo 'WARNING: SHADNET_TURN_EXTERNAL_IP is empty. coturn will advertise'
  echo '         the address it sees on its own interface, which behind a'
  echo '         home router is a LAN address no internet peer can use.'
fi
if [ -n "$EXT" ]; then
  echo "external-ip=$EXT" >> /tmp/turnserver.conf
fi
# coturn reads external-ip once, at start-up, so a changed address
# means a restart. Rather than reload machinery, the watcher ends the
# container and lets `restart: unless-stopped` start it again with the
# new value. Relayed sessions do not survive the address change anyway.
RECHECK=${TURN_EXTERNAL_RECHECK_SECONDS:-300}
if [ -n "$WATCH_NAME" ] && [ "$RECHECK" -gt 0 ]; then
  (
    while sleep "$RECHECK"; do
      new=$(resolve_ext "$WATCH_NAME")
      if [ -z "$new" ] || [ "$new" = "$EXT" ]; then
        continue
      fi
      echo "external address for $WATCH_NAME changed: $EXT -> $new"
      echo "restarting so coturn advertises it"
      # turnserver is pid 1 after the exec below; ending it ends the
      # container, and the restart policy brings it back.
      kill -TERM 1
      exit 0
    done
  ) &
fi
exec turnserver -c /tmp/turnserver.conf

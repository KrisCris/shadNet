# Deploying a TURN relay

shadNet does not carry game traffic between players. Players connect to each
other directly, and this server's only job in that is to tell each side where
to look. When no direct path exists, a TURN relay can carry the traffic
instead -- at the cost of your bandwidth, for the whole session.

This document is about standing one up. You may well not need it: see
[Do you actually need this?](#do-you-actually-need-this) first.

## Four different things people call "the TURN port"

Most failed relay deployments come from conflating these. They are separate,
and three of them have to be reachable.

| | What it is | Value here | Must be reachable from the internet? |
| --- | --- | --- | --- |
| **Listening port** | Where a player's client contacts the relay to ask for an allocation | `31478` udp and tcp | **Yes** |
| **Relay port range** | The ports the relay allocates and the *other* player sends to | `31500-31540` udp | **Yes, the whole range** |
| **Bind address** | What coturn listens on inside the container | every interface | no, it is local |
| **Advertised address** | What coturn *tells* players its relayed candidates are at | `SHADNET_TURN_EXTERNAL_IP` | it is not a port at all |

The second row is the one that gets missed. Forwarding `31478` alone produces a
relay that accepts allocations and hands back a candidate nobody can send to.
The allocation succeeds, the client reports a relay candidate, and the
connection still fails -- which looks like a bug somewhere else entirely.

The fourth row is the one that cannot be guessed. coturn behind a home router
sees only its LAN address. Unless you tell it otherwise, that LAN address is
what it advertises, and no peer on the internet can use it.

On a dynamic address, give `SHADNET_TURN_EXTERNAL_IP` a hostname rather than an
address. The compose service resolves it at start-up and re-checks it while
running; when it changes, coturn restarts with the new value. A DDNS name is
exactly the right thing here.

The record used is the **A** record, unless `SHADNET_TURN_EXTERNAL_FAMILY` says
`ipv6`. That default is deliberate: `external-ip` exists only to work around
NAT, and a host with a global IPv6 address already advertises the correct
address without help. A DDNS name's AAAA also often belongs to the router
rather than to the machine coturn runs on, and an `external-ip` in a family
coturn does not listen on names a socket that does not exist.

## A reachable shadNet is not a reachable relay

This is worth saying on its own, because the inference is tempting and wrong.

Players reach shadNet on `31313/tcp` and `31315/tcp`. Those
forwards say nothing about `31478` or about `31500-31540`. A server that every
player connects to fine can host a relay that no player can use.

## Why not 3478

`3478` is the IANA port for STUN and TURN, and this deployment does not use
it. Residential ISPs that filter inbound traffic tend to do it on low ports,
so every port here is five digits. The relay range avoids a second trap: it
stays below the ephemeral range (`32768-60999` on Linux), because a relay port
drawn from that range can lose a race to an unrelated outgoing connection, and
the allocation that fails is one a player was waiting on.

Nothing about the protocol needs the standard port -- clients are told where
to go. `src/config.cpp` still defaults to `3478` for anyone who does not set
it; the compose deployment always sets it.

Verify the relay separately, from outside your network, before you trust it.

## Port forwards to add

These are in addition to whatever already forwards the game ports. Review them
before changing anything on the router -- nothing here does it for you.

| Protocol | External | Internal | To |
| --- | --- | --- | --- |
| UDP | 31478 | 31478 | the shadNet host |
| TCP | 31478 | 31478 | the shadNet host |
| UDP | 31500-31540 | 31500-31540 | the shadNet host |

The TCP forward is for clients on networks that block outbound UDP entirely.
If you would rather not open it, drop it -- those players simply will not get
a relay.

If you narrow the range, change `min-port` and `max-port` in
`coturn/turnserver.conf` to match. Each concurrent relayed session uses two
ports, so `31500-31540` is twenty sessions.

## Turning it on

1. Generate a secret and put it in `.env`:

   ```
   openssl rand -hex 32
   ```

   `SHADNET_ICE_TURN_SECRET` is shared between shadNet and coturn and nothing
   else. shadNet derives a short-lived per-player credential from it
   (`<expiry>:<npid>` plus `base64(HMAC-SHA1(secret, username))`), and coturn
   verifies that credential without ever having been told the player exists.
   The secret is never returned by an API and never logged.

2. Fill in the rest of `.env`:

   ```
   SHADNET_ICE_TURN_HOST=turn.example.org    # what players are told to contact
   SHADNET_ICE_TURN_SECRET=<the value above>
   SHADNET_TURN_EXTERNAL_IP=turn.example.org   # or a literal public IPv4
   ```

   `SHADNET_TURN_EXTERNAL_IP` takes either. A hostname is resolved at start-up
   and re-checked every `SHADNET_TURN_EXTERNAL_RECHECK_SECONDS` (0 disables),
   which is what makes a dynamic address survivable.

3. Add the port forwards above.

4. Start it:

   ```
   docker compose --profile turn up -d
   ```

   The relay is behind a compose profile, so a plain `docker compose up` never
   starts it. That is deliberate: a relay the internet cannot reach is worse
   than no relay, because the server then advertises a candidate that fails
   silently after everything else has already been given up on.

5. If the server is already running, note that `shadnet.cfg` is generated on
   **first run only**. An existing deployment will not pick these up from the
   environment -- edit the file in the volume directly:

   ```
   docker compose exec shadnet sh -c 'cat >> /data/shadnet.cfg' <<'CFG'
   IceTurnHost=turn.example.org
   IceTurnPort=31478
   IceTurnSecret=<the value above>
   IceTurnTtlSeconds=3600
   CFG
   docker compose restart shadnet
   ```

## Checking it works

`docker compose logs coturn` tells you whether it started, and nothing more
useful than that. What matters is whether a player outside your network can
allocate, and the only way to know is to try from outside.

On a client, the emulator logs the nominated candidate pair for each peer
session. A relayed connection shows `typ relay` on at least one side. If you
never see that line, no relay was used -- which is the good case if players
are connecting anyway.

Note that a configured relay is not evidence of a working one. The candidate
pair in that log line is.

## Do you actually need this?

Probably not, and it is worth checking before spending bandwidth on it.

A relay only matters when *both* peers are behind NAT that defeats hole
punching. Two cases make it irrelevant:

- **Both players have working IPv6.** There is no NAT to traverse and the
  direct path wins outright.
- **Both players are on the same network.** The host candidates connect
  immediately.

Turn STUN on first, play a session, and read the nominated pair out of the
client log. If it says `typ host` or `typ srflx`, a relay would have sat idle.
Add one when you have an actual pair of players who cannot connect without it.

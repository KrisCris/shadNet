# Signaling (peer sessions and ICE)

How two players' emulators find a path to each other. The server pairs them and relays the ICE exchange; it never carries game traffic. It extracts candidate IP addresses for diagnostics without changing the relayed payload.

---

## Overview

A peer session pairs two authenticated accounts for one connectivity attempt. Through it, each side sends the other its ICE description and candidates, and libjuice on the client does the actual hole punching. shadNet's part is only the rendezvous, plus the handful of facts both sides must agree on before they can talk: who offers, which attempt this is, and what address each player is known by inside the emulated P2P namespace.

This replaced a UDP STUN listener that recorded each client's external endpoint and handed it out on request. That approach can only work when the endpoint the server observes is an endpoint the peer can reach, which is false for symmetric NAT and for most carrier-grade NAT — the cases that actually needed the help. It is gone, along with the `udpExt` registry and the `RequestSignalingInfos` command (105, retired).

The server still drives no connection-state machine. ESTABLISHED, DEAD and activation events are produced by the emulator's matching2-signaling layer, not here.

---

## Configuration

### Address diagnostics

After login, `Peer addresses: account: [IPs]` records the TCP source address.
As ICE descriptions and candidates arrive, it adds IPv4 and IPv6 addresses
and prints an updated, deduplicated list only when an address source changes.
The list lasts for that authenticated connection.

The suffix identifies each source:

- `observed-tcp`: the address the server's socket actually sees.
- `reported-host`: local interface candidates reported by the client, including IPv6 and VPN interfaces.
- `reported-srflx` / `reported-prflx`: reflexive candidates reported by the client.
- `reported-relay`: TURN relay candidates, which belong to the relay rather than the player's device.
- `reported-related`: related addresses carried in candidate `raddr` fields.

Both address families can appear in the same ICE exchange. Gathering starts
when a peer connection is requested, so login alone does not produce the
full candidate list. Client reports are not verified reachability, and a
single TCP socket cannot reveal addresses hidden by NAT. The selected path
and connection state still come from the client's ICE log. ICE passwords,
user fragments, and TURN credentials are never included in address logs.

Socket-only health checks do not open a per-client database connection.
Unauthenticated connection/disconnection messages use debug level; login,
authentication failures, and database errors retain their normal levels.

### ICE servers

| Setting | Default | Description |
|---|---|---|
| `IceStunHost` | (empty) | STUN server handed to clients. Empty disables STUN. |
| `IceStunPort` | `3478` | |
| `IceTurnHost` | (empty) | TURN relay handed to clients. Empty disables TURN. |
| `IceTurnPort` | `3478` | |
| `IceTurnSecret` | (empty) | Shared secret for coturn's `use-auth-secret`. Both this and `IceTurnHost` must be set, or no TURN server is offered. |
| `IceTurnTtlSeconds` | `3600` | Lifetime of an issued TURN credential. |

Set in `shadnet.cfg`. There is no UDP listener any more; the server binds TCP only. See `turn-deployment.md` for standing up the relay itself.

---

## Architecture

```text
     player A                     ShadNetServer                   player B
  ┌────────────┐               ┌───────────────────┐           ┌────────────┐
  │ libjuice   │   TCP 31313   │ SessionCoordinator │ TCP 31313 │ libjuice   │
  │ ICE agent  │◄─────────────►│ pairs A and B,     │◄─────────►│ ICE agent  │
  └─────┬──────┘  cmd 120-123  │ assigns roles and  │ notif 20-22└─────┬─────┘
        │                      │ virtual addresses  │                  │
        │                      └───────────────────┘                   │
        │                                                              │
        └────────────── game datagrams, direct or via TURN ────────────┘
                             (never through shadNet)
```

---

## Commands

### PeerSessionBegin (120)

Opens the session, or joins the one the peer already opened. Both players ring this bell independently as soon as each decides it wants the other, so the common case is a race: the second caller joins the first caller's session rather than opening a second one. The pair is keyed on `(offerer, answerer, titleId, attempt)`, ordered so both call directions produce the same key.

**Request:** `PeerSessionBeginRequest { target_npid, title_id, attempt }`

**Reply:** `PeerSessionBeginReply { session_id, generation, is_offerer, local_virtual_addr, peer_virtual_addr, peer_npid }`

**Side effect:** sends `PeerSessionOpened` (notification 20) to the target. The caller learns of the session from the reply and the peer learns of the same session from a notification; both converge on the same handler.

`is_offerer` is the server's decision, and it is what keeps the ICE roles apart. libjuice offers no way to set a role explicitly — whichever API call comes first decides it — so the offerer describes itself immediately and becomes controlling, and the answerer waits for that description and becomes controlled. Two controlling agents can still reach a connection, so a working connection is not evidence the roles were assigned correctly; the client asserts on libjuice's own log instead.

`title_id` is part of the pairing key. Two players sending different title ids open two sessions instead of joining one.

---

### PeerSignal (121)

Relays one ICE message to the other participant.

**Request:** `PeerSignalRequest { session_id, generation, kind, payload }`, where `kind` is description, candidate, or gathering-done.

**Reply:** `PeerSignalReply` (empty).

**Forwarded as:** `NotifyPeerSignal { session_id, generation, kind, payload, from_npid }` (notification 21).

The payload is opaque. The server checks that the sender is a current participant of that session at that generation, looks up the other one, and forwards the bytes unread. Authorisation comes from the authenticated connection, never from the request, so a client cannot signal on another account's behalf.

Only the payload's length is ever logged. An ICE description carries the session's short-term credentials.

`ErrorType::NotFound` from this command means the peer is offline, not that the session is unknown.

---

### PeerSessionEnd (122)

Ends the session and notifies the other participant with `NotifyPeerSessionClosed` (notification 22).

**Request:** `PeerSessionEndRequest { session_id, generation, reason }`

**Reply:** `PeerSessionEndReply` (empty).

Ending a session that is already gone is success, not an error: both peers end independently, and the second one must not see a failure. A client that disconnects has every session naming it dropped, so a peer waiting on it stops being told the session lives.

---

### GetIceServers (123)

Returns the STUN and TURN servers this client should use.

**Reply:** `GetIceServersReply { servers: [IceServer { host, port, is_turn, username, credential, expires_at }] }`

TURN credentials are minted per request in coturn's `use-auth-secret` form: the username is `<expiry-unix-seconds>:<npid>` and the credential is `base64(HMAC-SHA1(secret, username))`. The relay validates credentials it was never told about, and `IceTurnSecret` never leaves the server.

An empty `IceStunHost`, or a TURN host without a secret, simply omits that entry. Peers then fall back to host candidates, which is enough on a LAN and generally not enough across the internet.

---

## Generations

A session id survives a retry; the generation does not. `Renew` bumps the generation while keeping the id, and every forwarded signal is gated on the current one, so a straggling candidate from a failed attempt is rejected rather than mixed into the new attempt. The client bumps `attempt` for the same reason one level up: a retry gets a genuinely new session rather than rejoining the one that just failed.

---

## Virtual addresses

Each participant is assigned an address from `198.18.0.0/15` (the RFC 2544 benchmarking range) for the life of the session.

This is identity, not routing. The range is not routable on the public internet and is not one home networks hand out, which is the point. The emulated game asks for an IPv4 address for its peer and gets one; because nothing ever addresses a real packet to it, a bug in the layer above surfaces as a dropped datagram rather than as traffic sent to a stranger who happens to own that range.

Values are host byte order on the wire and on the server. The client converts once, on receipt.

---

## Shared state

### SessionCoordinator

| Field | Description |
|---|---|
| `m_sessions` | sessionId → Session |
| `m_byPair` | (offerer, answerer, titleId, attempt) → sessionId |
| `m_nextSessionId` | Monotonic session id generator |
| `m_virtualCursor` | Cursor into the virtual address range |

Guarded by its own `QReadWriteLock`, independent of the matching locks.

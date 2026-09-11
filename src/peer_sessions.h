// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <tuple>

#include <QHash>
#include <QList>
#include <QMap>
#include <QReadWriteLock>
#include <QString>

// A peer session pairs two authenticated accounts for one connectivity attempt
// so they can exchange ICE descriptions and candidates through the server.
//
// The server is a rendezvous point, not a participant: it never parses the
// signaling payloads. What it does own is the things both sides must agree on
// before they can talk -- who offers, which attempt this is, and what address
// each participant will be known by inside the emulated P2P namespace.
namespace Peer {

// Virtual addresses come from 198.18.0.0/15 (RFC 2544 benchmarking range),
// which is not routable on the public internet and is not a range home
// networks hand out. Values are host byte order here and on the wire; the
// client converts once, on receipt.
inline constexpr quint32 kVirtualRangeFirst = 0xC6120000u; // 198.18.0.0
inline constexpr quint32 kVirtualRangeLast = 0xC613FFFFu;  // 198.19.255.255

struct Session {
    quint64 sessionId = 0;
    quint32 generation = 0;
    QString offererNpid;
    QString answererNpid;
    QString titleId;
    quint32 attempt = 0;
    quint32 offererVirtualAddr = 0;
    quint32 answererVirtualAddr = 0;
    qint64 createdAtMs = 0;

    bool IsParticipant(const QString& npid) const {
        return npid == offererNpid || npid == answererNpid;
    }
    QString PeerOf(const QString& npid) const {
        if (npid == offererNpid) {
            return answererNpid;
        }
        if (npid == answererNpid) {
            return offererNpid;
        }
        return {};
    }
    quint32 VirtualAddrOf(const QString& npid) const {
        if (npid == offererNpid) {
            return offererVirtualAddr;
        }
        if (npid == answererNpid) {
            return answererVirtualAddr;
        }
        return 0;
    }
};

class SessionCoordinator {
public:
    // Returns the session for this pair/title/attempt, creating it on first
    // call. Both participants racing to begin the same attempt is the normal
    // case -- each side rings its bell independently -- so the second caller
    // joins the first caller's session rather than opening a second one.
    Session BeginOrJoin(const QString& selfNpid, const QString& peerNpid, const QString& titleId,
                        quint32 attempt, qint64 nowMs);

    std::optional<Session> Find(quint64 sessionId) const;

    // True only when the session exists, npid is one of its two participants,
    // and generation is the session's current one. Every forwarded signal is
    // gated on this.
    bool IsCurrentParticipant(quint64 sessionId, quint32 generation, const QString& npid) const;

    // Bumps the generation, keeping the session id. Used when an attempt is
    // retried without the pair changing; packets from the old generation are
    // then rejected rather than mixed into the new attempt.
    quint32 Renew(quint64 sessionId);

    void End(quint64 sessionId);

    // Removes every session naming this account. Called when a client
    // disconnects, so a peer waiting on it stops being told the session lives.
    QList<Session> DropParticipant(const QString& npid);

    int SessionCount() const;

private:
    // (offerer, answerer, titleId, attempt) -- the identity two racing callers
    // must agree on. Ordered, so both call directions produce the same key.
    using PairKey = std::tuple<QString, QString, QString, quint32>;

    quint32 NextVirtualAddrLocked();

    mutable QReadWriteLock m_lock;
    QHash<quint64, Session> m_sessions;
    QMap<PairKey, quint64> m_byPair;
    quint64 m_nextSessionId = 1;
    quint32 m_virtualCursor = 0;
};

} // namespace Peer

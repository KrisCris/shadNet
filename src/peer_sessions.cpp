// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "peer_sessions.h"

#include <utility>

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

namespace Peer {

namespace {

// Both participants call BeginOrJoin with themselves first, so the pair has to
// be ordered by something that does not depend on who called. The smaller
// online ID offers.
std::pair<QString, QString> Ordered(const QString& a, const QString& b) {
    return a.compare(b) <= 0 ? std::pair{a, b} : std::pair{b, a};
}

} // namespace

TurnCredential MakeTurnCredential(const QString& npid, const QByteArray& secret,
                                  qint64 nowUnixSeconds, qint64 ttlSeconds) {
    TurnCredential out;
    out.expiresAt = static_cast<quint64>(nowUnixSeconds + ttlSeconds);
    out.username = QStringLiteral("%1:%2").arg(out.expiresAt).arg(npid);
    out.credential = QString::fromLatin1(
        QMessageAuthenticationCode::hash(out.username.toUtf8(), secret, QCryptographicHash::Sha1)
            .toBase64());
    return out;
}

Session SessionCoordinator::BeginOrJoin(const QString& selfNpid, const QString& peerNpid,
                                        const QString& titleId, quint32 attempt, qint64 nowMs) {
    const auto [offerer, answerer] = Ordered(selfNpid, peerNpid);
    const PairKey key{offerer, answerer, titleId, attempt};

    QWriteLocker lk(&m_lock);

    const auto existing = m_byPair.constFind(key);
    if (existing != m_byPair.constEnd()) {
        const auto session = m_sessions.constFind(existing.value());
        if (session != m_sessions.constEnd()) {
            return session.value();
        }
        // The session was ended but its index entry outlived it. Fall through
        // and build a fresh one rather than handing back a dangling id.
        m_byPair.erase(existing);
    }

    Session session;
    session.sessionId = m_nextSessionId++;
    session.generation = 1;
    session.offererNpid = offerer;
    session.answererNpid = answerer;
    session.titleId = titleId;
    session.attempt = attempt;
    session.offererVirtualAddr = VirtualAddrForLocked(offerer);
    session.answererVirtualAddr = VirtualAddrForLocked(answerer);
    session.createdAtMs = nowMs;

    m_sessions.insert(session.sessionId, session);
    m_byPair.insert(key, session.sessionId);
    return session;
}

std::optional<Session> SessionCoordinator::Find(quint64 sessionId) const {
    QReadLocker lk(&m_lock);
    const auto it = m_sessions.constFind(sessionId);
    if (it == m_sessions.constEnd()) {
        return std::nullopt;
    }
    return it.value();
}

bool SessionCoordinator::IsCurrentParticipant(quint64 sessionId, quint32 generation,
                                              const QString& npid) const {
    QReadLocker lk(&m_lock);
    const auto it = m_sessions.constFind(sessionId);
    if (it == m_sessions.constEnd()) {
        return false;
    }
    return it->generation == generation && it->IsParticipant(npid);
}

quint32 SessionCoordinator::Renew(quint64 sessionId) {
    QWriteLocker lk(&m_lock);
    const auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end()) {
        return 0;
    }
    return ++it->generation;
}

void SessionCoordinator::End(quint64 sessionId) {
    QWriteLocker lk(&m_lock);
    const auto it = m_sessions.constFind(sessionId);
    if (it == m_sessions.constEnd()) {
        return;
    }
    m_byPair.remove(PairKey{it->offererNpid, it->answererNpid, it->titleId, it->attempt});
    m_sessions.erase(it);
}

QList<Session> SessionCoordinator::DropParticipant(const QString& npid) {
    QList<Session> dropped;

    QWriteLocker lk(&m_lock);
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (!it->IsParticipant(npid)) {
            ++it;
            continue;
        }
        m_byPair.remove(PairKey{it->offererNpid, it->answererNpid, it->titleId, it->attempt});
        dropped.append(it.value());
        it = m_sessions.erase(it);
    }
    // The lease ends with the connection. Holding it would consume the range
    // for players who have gone, and a reconnecting account has no claim on
    // the address its peers have already stopped using.
    m_addrByNpid.remove(npid);
    return dropped;
}

int SessionCoordinator::SessionCount() const {
    QReadLocker lk(&m_lock);
    return static_cast<int>(m_sessions.size());
}

quint32 SessionCoordinator::VirtualAddrForLocked(const QString& npid) {
    const auto existing = m_addrByNpid.constFind(npid);
    if (existing != m_addrByNpid.constEnd()) {
        return existing.value();
    }
    const quint32 addr = NextVirtualAddrLocked();
    m_addrByNpid.insert(npid, addr);
    return addr;
}

quint32 SessionCoordinator::NextVirtualAddrLocked() {
    // Skip the .0 and .255 host parts of every /24 in the range so nothing
    // downstream has to special-case a network or broadcast address.
    constexpr quint32 kSpan = kVirtualRangeLast - kVirtualRangeFirst + 1;
    for (quint32 tried = 0; tried < kSpan; ++tried) {
        const quint32 addr = kVirtualRangeFirst + (m_virtualCursor % kSpan);
        ++m_virtualCursor;
        const quint32 host = addr & 0xFFu;
        if (host != 0x00u && host != 0xFFu) {
            return addr;
        }
    }
    return 0;
}

} // namespace Peer

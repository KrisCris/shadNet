// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDateTime>
#include <QDebug>

#include "client_session.h"
#include "config.h"
#include "peer_sessions.h"
#include "proto_utils.h"

namespace {

const char* SignalKindName(shadnet::PeerSignalKind kind) {
    switch (kind) {
    case shadnet::PEER_SIGNAL_DESCRIPTION:
        return "description";
    case shadnet::PEER_SIGNAL_CANDIDATE:
        return "candidate";
    case shadnet::PEER_SIGNAL_GATHERING_DONE:
        return "gathering-done";
    default:
        return "unknown";
    }
}

} // namespace

ErrorType ClientSession::CmdPeerSessionBegin(StreamExtractor& data, QByteArray& reply) {
    shadnet::PeerSessionBeginRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    const QString targetNpid = QString::fromStdString(req.target_npid());
    if (targetNpid.isEmpty() || targetNpid == m_info.npid)
        return ErrorType::InvalidInput;

    const QString titleId = QString::fromStdString(req.title_id());
    const Peer::Session session =
        m_shared->peers.BeginOrJoin(m_info.npid, targetNpid, titleId, req.attempt(),
                                    QDateTime::currentDateTimeUtc().toMSecsSinceEpoch());

    const bool isOfferer = session.offererNpid == m_info.npid;

    shadnet::PeerSessionBeginReply rep;
    rep.set_session_id(session.sessionId);
    rep.set_generation(session.generation);
    rep.set_is_offerer(isOfferer);
    rep.set_local_virtual_addr(session.VirtualAddrOf(m_info.npid));
    rep.set_peer_virtual_addr(session.VirtualAddrOf(targetNpid));
    rep.set_peer_npid(targetNpid.toStdString());
    appendProto(reply, rep);

    qInfo() << "Peer session" << session.sessionId << "gen" << session.generation << ":"
            << m_info.npid << (isOfferer ? "offers" : "answers") << "to" << targetNpid
            << "title=" << titleId << "attempt=" << session.attempt;

    NotifyPeerSessionOpened(session, targetNpid);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdPeerSignal(StreamExtractor& data, QByteArray& reply) {
    shadnet::PeerSignalRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    // Authorisation comes from the authenticated connection, never from the
    // request: a client cannot signal on another account's behalf, and a
    // stale generation cannot bleed into a retried attempt.
    if (!m_shared->peers.IsCurrentParticipant(req.session_id(), req.generation(), m_info.npid)) {
        qDebug() << "Peer signal refused: session" << req.session_id() << "gen" << req.generation()
                 << "from" << m_info.npid << "-- not a current participant";
        return ErrorType::Unauthorized;
    }

    const std::optional<Peer::Session> session = m_shared->peers.Find(req.session_id());
    if (!session.has_value())
        return ErrorType::NotFound;

    const QString peerNpid = session->PeerOf(m_info.npid);
    int64_t peerUserId = -1;
    {
        QReadLocker lk(&m_shared->clientsLock);
        const auto it = m_shared->npidToUserId.constFind(peerNpid);
        if (it != m_shared->npidToUserId.constEnd())
            peerUserId = it.value();
    }
    if (peerUserId < 0) {
        qDebug() << "Peer signal dropped: session" << req.session_id() << "peer" << peerNpid
                 << "is offline";
        return ErrorType::NotFound;
    }

    shadnet::NotifyPeerSignal note;
    note.set_session_id(req.session_id());
    note.set_generation(req.generation());
    note.set_kind(req.kind());
    note.set_payload(req.payload());
    note.set_from_npid(m_info.npid.toStdString());

    QByteArray payload;
    appendProto(payload, note);
    SendNotification(NotificationType::PeerSignal, payload, peerUserId);

    // Payload length only. An ICE description carries the session's short-term
    // credentials, so it never goes to the log.
    qDebug() << "Peer signal: session" << req.session_id() << "gen" << req.generation()
             << SignalKindName(req.kind()) << m_info.npid << "->" << peerNpid
             << "bytes=" << static_cast<int>(req.payload().size());

    shadnet::PeerSignalReply rep;
    appendProto(reply, rep);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdPeerSessionEnd(StreamExtractor& data, QByteArray& reply) {
    shadnet::PeerSessionEndRequest req;
    if (!decodeProto(req, data) || data.error())
        return ErrorType::Malformed;

    const std::optional<Peer::Session> session = m_shared->peers.Find(req.session_id());
    // Ending a session that is already gone is success, not an error: both
    // peers end independently and the second one must not see a failure.
    if (session.has_value()) {
        if (!session->IsParticipant(m_info.npid))
            return ErrorType::Unauthorized;

        ClosePeerSessions({*session}, req.reason());
        m_shared->peers.End(req.session_id());
        qInfo() << "Peer session" << req.session_id() << "ended by" << m_info.npid
                << "reason=" << req.reason();
    }

    shadnet::PeerSessionEndReply rep;
    appendProto(reply, rep);
    return ErrorType::NoError;
}

ErrorType ClientSession::CmdGetIceServers(QByteArray& reply) {
    ConfigManager* config = m_shared->config;
    shadnet::GetIceServersReply rep;

    const QString stunHost = config->GetIceStunHost();
    if (!stunHost.isEmpty()) {
        shadnet::IceServer* server = rep.add_servers();
        server->set_host(stunHost.toStdString());
        server->set_port(config->GetIceStunPort());
        server->set_is_turn(false);
    }

    const QString turnHost = config->GetIceTurnHost();
    const QByteArray turnSecret = config->GetIceTurnSecret();
    if (!turnHost.isEmpty() && !turnSecret.isEmpty()) {
        const Peer::TurnCredential credential =
            Peer::MakeTurnCredential(m_info.npid, turnSecret, QDateTime::currentSecsSinceEpoch(),
                                     config->GetIceTurnTtlSeconds());
        shadnet::IceServer* server = rep.add_servers();
        server->set_host(turnHost.toStdString());
        server->set_port(config->GetIceTurnPort());
        server->set_is_turn(true);
        server->set_username(credential.username.toStdString());
        server->set_credential(credential.credential.toStdString());
        server->set_expires_at(credential.expiresAt);
    }

    appendProto(reply, rep);

    // Kinds and count only -- never the credential.
    qInfo() << "IceServers:" << m_info.npid << "stun=" << !stunHost.isEmpty()
            << "turn=" << (!turnHost.isEmpty() && !turnSecret.isEmpty());
    return ErrorType::NoError;
}

void ClientSession::NotifyPeerSessionOpened(const Peer::Session& session,
                                            const QString& recipientNpid) {
    int64_t recipientUserId = -1;
    {
        QReadLocker lk(&m_shared->clientsLock);
        const auto it = m_shared->npidToUserId.constFind(recipientNpid);
        if (it != m_shared->npidToUserId.constEnd())
            recipientUserId = it.value();
    }
    if (recipientUserId < 0)
        return;

    shadnet::NotifyPeerSessionOpened note;
    note.set_session_id(session.sessionId);
    note.set_generation(session.generation);
    note.set_is_offerer(session.offererNpid == recipientNpid);
    note.set_peer_npid(session.PeerOf(recipientNpid).toStdString());
    note.set_local_virtual_addr(session.VirtualAddrOf(recipientNpid));
    note.set_peer_virtual_addr(session.VirtualAddrOf(session.PeerOf(recipientNpid)));
    note.set_title_id(session.titleId.toStdString());

    QByteArray payload;
    appendProto(payload, note);
    SendNotification(NotificationType::PeerSessionOpened, payload, recipientUserId);
}

void ClientSession::ClosePeerSessions(const QList<Peer::Session>& sessions, quint32 reason) {
    for (const Peer::Session& session : sessions) {
        for (const QString& npid : {session.offererNpid, session.answererNpid}) {
            if (npid == m_info.npid)
                continue;

            int64_t userId = -1;
            {
                QReadLocker lk(&m_shared->clientsLock);
                const auto it = m_shared->npidToUserId.constFind(npid);
                if (it != m_shared->npidToUserId.constEnd())
                    userId = it.value();
            }
            if (userId < 0)
                continue;

            shadnet::NotifyPeerSessionClosed note;
            note.set_session_id(session.sessionId);
            note.set_generation(session.generation);
            note.set_reason(reason);

            QByteArray payload;
            appendProto(payload, note);
            SendNotification(NotificationType::PeerSessionClosed, payload, userId);
        }
    }
}

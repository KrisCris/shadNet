// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <optional>
#include <QDateTime>
#include <QHash>
#include <QHttpServer>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include "config.h"
#include "database.h"

struct SharedState;

class AdminApiServer : public QObject {
    Q_OBJECT
public:
    explicit AdminApiServer(QObject* parent = nullptr);
    ~AdminApiServer();
    bool Start(ConfigManager* config, const QString& dbPath, SharedState* shared);
    int SyncConfigAdmins();

private:
    struct AdminSession {
        int64_t userId = 0;
        QString npid;
        QDateTime expiresAt;
    };

    // Per-IP failed-login counter, so the login endpoint can't be brute-forced.
    struct LoginThrottle {
        int failures = 0;
        QDateTime blockedUntil;
    };

    void RegisterRoutes();
    bool CheckApiKey(const QHttpServerRequest& req) const;
    QHttpServerResponse ApiKeyError(const QHttpServerRequest& req) const;
    std::optional<AdminSession> Authenticate(const QHttpServerRequest& req);
    QHttpServerResponse AuthError(const QHttpServerRequest& req) const;

    QString MintToken(int64_t userId, const QString& npid);
    void PurgeExpiredSessions();

    bool IsThrottled(const QString& peer, int& retryAfterSecs);
    void NoteLoginFailure(const QString& peer);
    void ClearLoginFailures(const QString& peer);

    // True when the user currently holds an authenticated game session.
    bool IsOnline(int64_t userId) const;
    // Drops the user's live game session, if any. Returns true when one was closed.
    bool KickUser(int64_t userId);

    // Deletes the account's scores, score blobs, TUS slots, and relationships, and
    // evicts the scores from the live leaderboard cache.
    bool PurgeUserData(int64_t userId, PurgeSummary& summary, int& cachedScoresDropped,
                       int& blobsDeleted);

    // As above, but also removes the account row itself.
    bool DeleteAccount(int64_t userId, PurgeSummary& summary, int& cachedScoresDropped,
                       int& blobsDeleted);

    // Filesystem + cache cleanup that follows either of the two above. 
    void CleanUpAfterDataRemoval(int64_t userId, const PurgeSummary& summary,
                                 int& cachedScoresDropped, int& blobsDeleted);

    QJsonObject UserRowToJson(const AdminUserRow& row) const;

    ConfigManager* m_config = nullptr;
    SharedState* m_shared = nullptr;
    std::unique_ptr<Database> m_db;
    std::unique_ptr<QHttpServer> m_http;
    std::unique_ptr<QTcpServer> m_tcp;

    mutable QMutex m_sessionsMutex;
    QHash<QString, AdminSession> m_sessions;
    QHash<QString, LoginThrottle> m_throttle;

    int m_sessionMinutes = 480; // idle lifetime of an admin token
};

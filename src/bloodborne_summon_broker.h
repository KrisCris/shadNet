// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>

namespace Bloodborne {

class SummonBroker {
public:
    enum class State { Advertised, Preparing, Claimed, Delivered, Consumed };
    enum class ClaimStatus { Claimed, AlreadyClaimed, NotFound, Conflict };

    struct Options {
        qint64 ttlMs = 130'000;
        qint64 seamlessTtlMs = 15 * 60 * 1000;
        bool seamlessCoop = false;
        bool seamlessAnywhereSummons = false;
    };

    struct AdvertiseResult {
        State state = State::Advertised;
        QByteArray pendingClaim;
        QByteArray pendingHostPlacement;
        // How many times this same claim has now been handed out. Anything
        // above 1 means an earlier delivery did not take -- which looks
        // exactly like a fresh success in the log unless it is counted.
        int claimDeliveries = 0;
    };

    struct ClaimResult {
        ClaimStatus status = ClaimStatus::NotFound;
        QString targetSessionId;
        qint64 targetUserId = -1;
    };

    struct ConsumeResult {
        int consumed = 0;
        int retained = 0;
        QByteArray pendingHostPlacement;
    };

    SummonBroker();
    explicit SummonBroker(qint64 ttlMs);
    explicit SummonBroker(Options options);

    AdvertiseResult Advertise(const QJsonObject& body, const QByteArray& rawBody, qint64 nowMs);
    QList<QByteArray> Search(const QJsonObject& request, qint64 nowMs,
                             const QByteArray& hostPlacement = {});
    ClaimResult Claim(const QJsonObject& request, const QByteArray& rawRequest, qint64 nowMs,
                      const QByteArray& hostPlacement = {});
    ConsumeResult Consume(const QJsonObject& request, qint64 nowMs);

    std::optional<State> StateFor(const QString& sessionId, qint64 userId, qint64 nowMs);

    // The key identifying one advertisement, as used internally and by
    // StateFor(). Exposed because a create request held open for a claim has
    // to be matched against a Claim() that arrives on a different connection,
    // and both sides must agree on what identifies the advertisement.
    static QString KeyFor(const QString& sessionId, qint64 userId);
    int Size(qint64 nowMs);
    bool IsSeamlessCoopEnabled() const;
    bool IsSeamlessAnywhereSummonsEnabled() const;

private:
    struct Record {
        State state = State::Advertised;
        QJsonObject advertisement;
        QByteArray rawAdvertisement;
        QJsonObject claim;
        QByteArray rawClaim;
        QByteArray hostPlacement;
        qint64 preparationRequester = -1;
        qint64 updatedAtMs = 0;
        int claimDeliveries = 0;
    };

    void PurgeExpiredLocked(qint64 nowMs);

    qint64 m_ttlMs;
    qint64 m_seamlessTtlMs;
    bool m_seamlessCoop;
    bool m_seamlessAnywhereSummons;
    QMutex m_mutex;
    QHash<QString, Record> m_records;
};

bool HasRequiredAdvertisementFields(const QJsonObject& body);
QByteArray BuildClaimDeliveryResponse(const QByteArray& rawClaim);

} // namespace Bloodborne

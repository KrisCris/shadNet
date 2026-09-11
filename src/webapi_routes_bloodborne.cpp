// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_bloodborne.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>

#include <QDateTime>
#include <QDebug>
#include <QFuture>
#include <QHash>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QPromise>
#include <QString>
#include <QTimer>

#include "bloodborne_summon_broker.h"

namespace WebApiRoutes {
namespace {

constexpr auto HostPlacementHeader = "X-ShadPS4-Bloodborne-Host-Placement";

QHttpServerResponse SummonResponse(const QString& messageId, const QByteArray& hostPlacement = {}) {
    QJsonObject body;
    body.insert(QStringLiteral("ResKind"), 0);
    body.insert(QStringLiteral("MessageId"), messageId);
    QHttpServerResponse response{"application/json",
                                 QJsonDocument(body).toJson(QJsonDocument::Compact),
                                 QHttpServerResponse::StatusCode::Ok};
    if (!hostPlacement.isEmpty()) {
        QHttpHeaders headers = response.headers();
        if (headers.append(HostPlacementHeader, hostPlacement)) {
            response.setHeaders(std::move(headers));
        }
    }
    return response;
}

QHttpServerResponse SummonListResponse(const QList<QByteArray>& signs) {
    QByteArray body = "{\"SummonDataList\":[";
    for (qsizetype index = 0; index < signs.size(); ++index) {
        if (index != 0) {
            body.append(',');
        }
        body.append(signs[index]);
    }
    body.append("],\"ResKind\":0,\"MessageId\":\"SummonDataGetListResponse\"}");
    return QHttpServerResponse{"application/json", body, QHttpServerResponse::StatusCode::Ok};
}

QHttpServerResponse RawJsonResponse(const QByteArray& body, const QByteArray& hostPlacement = {}) {
    QHttpServerResponse response{"application/json", body, QHttpServerResponse::StatusCode::Ok};
    if (!hostPlacement.isEmpty()) {
        QHttpHeaders headers = response.headers();
        if (headers.append(HostPlacementHeader, hostPlacement)) {
            response.setHeaders(std::move(headers));
        }
    }
    return response;
}

QHttpServerResponse InvalidRequest(const QString& reason) {
    QJsonObject body;
    body.insert(QStringLiteral("ResKind"), 1);
    body.insert(QStringLiteral("MessageId"), QString());
    body.insert(QStringLiteral("Error"), reason);
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::BadRequest};
}

std::optional<QJsonObject> ParseRequest(const QHttpServerRequest& request) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(request.body(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

qint64 Integer(const QJsonObject& object, const QString& key, qint64 fallback = 0) {
    const QJsonValue value = object.value(key);
    return value.isDouble() ? value.toVariant().toLongLong() : fallback;
}

bool EnvEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && QString::fromUtf8(value).trimmed() != "0";
}

// How long to hold a summon advertisement's create request open waiting for
// somebody to summon that player, in milliseconds. 0 disables holding and
// restores the plain request/response behaviour.
//
// This exists because the game learns it has been summoned only from the reply
// to its own SummonDataCreateRequest, which it re-posts once a minute, while
// the summoning host tears its room down after about 55 seconds. Answering
// immediately therefore guarantees the news arrives after the room is gone.
// Holding the reply turns that once-a-minute re-post into a long poll.
qint64 ClaimHoldMs() {
    static const qint64 value = [] {
        const QByteArray raw = qgetenv("SHADNET_BLOODBORNE_CLAIM_HOLD_MS");
        if (raw.isEmpty()) {
            return qint64{20000};
        }
        bool ok = false;
        const qint64 parsed = raw.toLongLong(&ok);
        if (!ok || parsed < 0) {
            qWarning() << "SHADNET_BLOODBORNE_CLAIM_HOLD_MS is not a non-negative integer:" << raw
                       << "-- falling back to 20000";
            return qint64{20000};
        }
        return parsed;
    }();
    return value;
}

// A create request parked until a summon arrives for it or the hold expires.
struct ClaimWaiter {
    QJsonObject body;
    QByteArray rawBody;
    std::shared_ptr<QPromise<QHttpServerResponse>> promise;
    // Distinguishes successive holds for the same advertisement. A hold's
    // expiry timer keeps firing even after the hold it belongs to has been
    // answered early, and by then the key may name a newer hold -- which the
    // stale timer would otherwise cut short.
    quint64 generation = 0;
};

using ClaimWaiters = QHash<QString, ClaimWaiter>;

QFuture<QHttpServerResponse> ReadyFuture(QHttpServerResponse&& response) {
    QPromise<QHttpServerResponse> promise;
    QFuture<QHttpServerResponse> future = promise.future();
    promise.start();
    promise.addResult(std::move(response));
    promise.finish();
    return future;
}

// Take the waiter for `key` off the table, if any. The caller owns answering it.
std::optional<ClaimWaiter> TakeWaiter(ClaimWaiters& waiters, const QString& key) {
    const auto it = waiters.find(key);
    if (it == waiters.end()) {
        return std::nullopt;
    }
    ClaimWaiter waiter = std::move(it.value());
    waiters.erase(it);
    return waiter;
}

// Answer a held create with the ordinary "your sign is up" reply. Used when the
// hold expires and when a fresh create supersedes an older one.
void ReleaseWaiter(ClaimWaiters& waiters, const QString& key, const char* why,
                   std::optional<quint64> generation = std::nullopt) {
    if (generation.has_value()) {
        const auto it = waiters.constFind(key);
        if (it == waiters.constEnd() || it->generation != *generation) {
            return; // already answered; the key now names a different hold
        }
    }
    auto waiter = TakeWaiter(waiters, key);
    if (!waiter) {
        return;
    }
    qInfo() << "Bloodborne summon: released held create" << key << "--" << why;
    waiter->promise->addResult(SummonResponse(QStringLiteral("SummonDataCreateResponse")));
    waiter->promise->finish();
}

// Answer a held create with the claim that just arrived for it. Returns false
// when nothing was waiting, which is the normal case for a client that is not
// currently parked on a create.
bool DeliverClaimToWaiter(Bloodborne::SummonBroker& broker, ClaimWaiters& waiters,
                          const QString& key) {
    auto waiter = TakeWaiter(waiters, key);
    if (!waiter) {
        return false;
    }

    // Re-run the advertisement through the broker so the Claimed -> Delivered
    // transition, and every rule guarding it, stays in one place.
    const auto result = broker.Advertise(waiter->body, waiter->rawBody,
                                         QDateTime::currentMSecsSinceEpoch());
    if (result.pendingClaim.isEmpty()) {
        qWarning() << "Bloodborne summon: held create" << key
                   << "woken by a claim the broker will not deliver yet -- releasing";
        waiter->promise->addResult(SummonResponse(QStringLiteral("SummonDataCreateResponse"),
                                                  result.pendingHostPlacement));
        waiter->promise->finish();
        return true;
    }

    const QByteArray response = Bloodborne::BuildClaimDeliveryResponse(result.pendingClaim);
    if (response.isEmpty()) {
        qWarning() << "Bloodborne summon: held create" << key << "has an invalid pending claim";
        waiter->promise->addResult(InvalidRequest(QStringLiteral("Invalid pending summon claim")));
        waiter->promise->finish();
        return true;
    }

    qInfo() << "Bloodborne summon: delivered claim on held create" << key
            << "host-placement-bytes" << result.pendingHostPlacement.size() << "delivery"
            << result.claimDeliveries;
    waiter->promise->addResult(RawJsonResponse(response, result.pendingHostPlacement));
    waiter->promise->finish();
    return true;
}

void TraceSummonRequest(const char* route, const QHttpServerRequest& request) {
    if (!EnvEnabled("SHADNET_BLOODBORNE_RE_TRACE")) {
        return;
    }
    qInfo().noquote() << "Bloodborne summon RE" << route << request.body();
}

} // namespace

void RegisterBloodborneRoutes(QHttpServer& http, bool seamlessCoop) {
    seamlessCoop = seamlessCoop || EnvEnabled("SHADNET_BLOODBORNE_SEAMLESS_COOP");
    Bloodborne::SummonBroker::Options options;
    options.seamlessCoop = seamlessCoop;
    auto broker = std::make_shared<Bloodborne::SummonBroker>(options);
    auto waiters = std::make_shared<ClaimWaiters>();

    qInfo() << "Bloodborne summon routes registered; seamless co-op"
            << (broker->IsSeamlessCoopEnabled() ? "enabled" : "disabled") << "anywhere summons"
            << (broker->IsSeamlessAnywhereSummonsEnabled() ? "enabled" : "disabled");

    http.route("/summon_messenger/create", QHttpServerRequest::Method::Post,
               [broker, waiters, &http](const QHttpServerRequest& request)
                   -> QFuture<QHttpServerResponse> {
                   TraceSummonRequest("create", request);
                   const auto body = ParseRequest(request);
                   if (!body || !Bloodborne::HasRequiredAdvertisementFields(*body)) {
                       return ReadyFuture(
                           InvalidRequest(QStringLiteral("Invalid summon advertisement")));
                   }
                   const QByteArray rawBody = request.body();
                   const auto result =
                       broker->Advertise(*body, rawBody, QDateTime::currentMSecsSinceEpoch());
                   if (!result.pendingClaim.isEmpty()) {
                       const QByteArray response =
                           Bloodborne::BuildClaimDeliveryResponse(result.pendingClaim);
                       if (response.isEmpty()) {
                           return ReadyFuture(
                               InvalidRequest(QStringLiteral("Invalid pending summon claim")));
                       }
                       qInfo() << "Bloodborne summon: delivered claim to user"
                               << Integer(*body, QStringLiteral("UserId")) << "session"
                               << body->value(QStringLiteral("SessionId")).toString()
                               << "host-placement-bytes" << result.pendingHostPlacement.size()
                               << "delivery" << result.claimDeliveries;
                       if (result.claimDeliveries > 1) {
                           // The player has been told about this summon before
                           // and has not acted on it. Handing it over again is
                           // correct, but it is not progress, and it reads as
                           // progress unless the count is on the line.
                           qWarning()
                               << "Bloodborne summon: this is delivery" << result.claimDeliveries
                               << "of the same claim to user"
                               << Integer(*body, QStringLiteral("UserId"))
                               << "-- earlier deliveries were not acted on";
                       }
                       return ReadyFuture(
                           RawJsonResponse(response, result.pendingHostPlacement));
                   }
                   if (!result.pendingHostPlacement.isEmpty()) {
                       qInfo() << "Bloodborne summon: preparing cross-map user"
                               << Integer(*body, QStringLiteral("UserId")) << "session"
                               << body->value(QStringLiteral("SessionId")).toString()
                               << "destination placement bytes"
                               << result.pendingHostPlacement.size();
                       return ReadyFuture(SummonResponse(
                           QStringLiteral("SummonDataCreateResponse"),
                           result.pendingHostPlacement));
                   }

                   const QString sessionId =
                       body->value(QStringLiteral("SessionId")).toString();
                   const qint64 userId = Integer(*body, QStringLiteral("UserId"), -1);
                   const QString key = Bloodborne::SummonBroker::KeyFor(sessionId, userId);
                   const qint64 holdMs = ClaimHoldMs();

                   if (holdMs <= 0) {
                       qInfo() << "Bloodborne summon: advertised user" << userId << "session"
                               << sessionId;
                       return ReadyFuture(
                           SummonResponse(QStringLiteral("SummonDataCreateResponse")));
                   }

                   // A re-post for the same advertisement means the previous
                   // hold is stale; answer it before parking the new one, so a
                   // client can never have two of its requests outstanding.
                   ReleaseWaiter(*waiters, key, "superseded by a newer create");

                   static quint64 nextGeneration = 0;
                   const quint64 generation = ++nextGeneration;

                   ClaimWaiter waiter;
                   waiter.body = *body;
                   waiter.rawBody = rawBody;
                   waiter.generation = generation;
                   waiter.promise = std::make_shared<QPromise<QHttpServerResponse>>();
                   waiter.promise->start();
                   QFuture<QHttpServerResponse> future = waiter.promise->future();
                   waiters->insert(key, std::move(waiter));

                   qInfo() << "Bloodborne summon: advertised user" << userId << "session"
                           << sessionId << "-- holding create for up to" << holdMs << "ms";

                   QTimer::singleShot(std::chrono::milliseconds(holdMs), &http,
                                      [waiters, key, generation] {
                       ReleaseWaiter(*waiters, key, "hold expired", generation);
                   });
                   return future;
               });

    http.route("/summon_messenger/get", QHttpServerRequest::Method::Post,
               [broker](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("get", request);
                   const auto body = ParseRequest(request);
                   if (!body) {
                       return InvalidRequest(QStringLiteral("Invalid summon search"));
                   }
                   const QList<QByteArray> signs =
                       broker->Search(*body, QDateTime::currentMSecsSinceEpoch(),
                                      request.value(HostPlacementHeader));
                   qInfo() << "Bloodborne summon: search for user"
                           << Integer(*body, QStringLiteral("UserId")) << "returned"
                           << signs.size();
                   return SummonListResponse(signs);
               });

    http.route("/summon_messenger/delete", QHttpServerRequest::Method::Post,
               [broker](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("delete", request);
                   const auto body = ParseRequest(request);
                   if (!body) {
                       return InvalidRequest(QStringLiteral("Invalid summon removal"));
                   }
                   const auto result = broker->Consume(*body, QDateTime::currentMSecsSinceEpoch());
                   qInfo() << "Bloodborne summon: consumed" << result.consumed << "retained"
                           << result.retained << "advertisement(s) host-placement-bytes"
                           << result.pendingHostPlacement.size();
                   QHttpServerResponse response =
                       SummonResponse(QStringLiteral("SummonDataRemoveResponse"));
                   if (result.pendingHostPlacement.isEmpty()) {
                       return response;
                   }
                   QHttpHeaders headers = response.headers();
                   if (headers.append(HostPlacementHeader, result.pendingHostPlacement)) {
                       response.setHeaders(std::move(headers));
                   }
                   return response;
               });

    http.route("/summon_messenger/request", QHttpServerRequest::Method::Post,
               [broker, waiters](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("request", request);
                   const auto body = ParseRequest(request);
                   if (!body) {
                       return InvalidRequest(QStringLiteral("Invalid summon request"));
                   }
                   const auto result =
                       broker->Claim(*body, request.body(), QDateTime::currentMSecsSinceEpoch(),
                                     request.value(HostPlacementHeader));
                   switch (result.status) {
                   case Bloodborne::SummonBroker::ClaimStatus::Claimed: {
                       qInfo() << "Bloodborne summon: claimed session" << result.targetSessionId
                               << "user" << result.targetUserId;
                       // Hand the news to the summoned player straight away if
                       // they are parked on a create. Otherwise it waits for
                       // their next one, which is the old behaviour.
                       const QString key = Bloodborne::SummonBroker::KeyFor(
                           result.targetSessionId, result.targetUserId);
                       if (!DeliverClaimToWaiter(*broker, *waiters, key)) {
                           qInfo() << "Bloodborne summon: no held create for" << key
                                   << "-- claim waits for the next one";
                       }
                       break;
                   }
                   case Bloodborne::SummonBroker::ClaimStatus::AlreadyClaimed:
                       qInfo() << "Bloodborne summon: repeated claim for session"
                               << result.targetSessionId;
                       break;
                   case Bloodborne::SummonBroker::ClaimStatus::NotFound:
                       qWarning() << "Bloodborne summon: claim target not found";
                       break;
                   case Bloodborne::SummonBroker::ClaimStatus::Conflict:
                       qWarning() << "Bloodborne summon: conflicting claim for session"
                                  << result.targetSessionId;
                       break;
                   }
                   return SummonResponse(QStringLiteral("SummonDataSummonResponse"));
               });
}

} // namespace WebApiRoutes

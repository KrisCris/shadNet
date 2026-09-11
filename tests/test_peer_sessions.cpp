// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>

#include "peer_sessions.h"

namespace {

bool Check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
  }
  return condition;
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!Check((expression), #expression, __LINE__))                           \
      return 1;                                                                \
  } while (false)

constexpr qint64 kNow = 1'757'548'800'000;

} // namespace

int main() {
  // Both peers ring independently and race to begin. That must produce one
  // session, not two, or each side negotiates against a different id.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session a = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    const Peer::Session b = coordinator.BeginOrJoin(
        QStringLiteral("MintCoffeeCat"), QStringLiteral("connlost"),
        QStringLiteral("CUSA03173"), 1, kNow);
    CHECK(a.sessionId == b.sessionId);
    CHECK(a.generation == b.generation);
    CHECK(coordinator.SessionCount() == 1);
  }

  // The offerer must be the same from either call direction, so exactly one
  // side offers regardless of who asked first.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session a = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    const Peer::Session b = coordinator.BeginOrJoin(
        QStringLiteral("MintCoffeeCat"), QStringLiteral("connlost"),
        QStringLiteral("CUSA03173"), 1, kNow);
    CHECK(a.offererNpid == QStringLiteral("MintCoffeeCat"));
    CHECK(a.answererNpid == QStringLiteral("connlost"));
    CHECK(a.offererNpid == b.offererNpid);
    CHECK(a.answererNpid == b.answererNpid);
  }

  // Re-ringing the bell is a new attempt and must not reuse the old session.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session first = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    const Peer::Session second = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 2, kNow);
    CHECK(first.sessionId != second.sessionId);
    CHECK(coordinator.SessionCount() == 2);
  }

  // A different title is a different session even for the same pair.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session a = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    const Peer::Session b = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA00900"), 1, kNow);
    CHECK(a.sessionId != b.sessionId);
  }

  // Virtual addresses must be distinct, inside 198.18.0.0/15, and consistent
  // whichever participant asks.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session session = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    CHECK(session.offererVirtualAddr != session.answererVirtualAddr);
    for (const quint32 addr :
         {session.offererVirtualAddr, session.answererVirtualAddr}) {
      CHECK(addr >= Peer::kVirtualRangeFirst);
      CHECK(addr <= Peer::kVirtualRangeLast);
      // Neither the network nor the broadcast address of any /24 inside the
      // range, so nothing downstream has to special-case them.
      CHECK((addr & 0xFFu) != 0x00u);
      CHECK((addr & 0xFFu) != 0xFFu);
    }
    CHECK(session.VirtualAddrOf(QStringLiteral("connlost")) ==
          session.answererVirtualAddr);
    CHECK(session.VirtualAddrOf(QStringLiteral("MintCoffeeCat")) ==
          session.offererVirtualAddr);
    CHECK(session.VirtualAddrOf(QStringLiteral("nobody")) == 0);
  }

  // Successive sessions must not hand out the same address twice in a row.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session a = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    const Peer::Session b = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 2, kNow);
    CHECK(a.offererVirtualAddr != b.offererVirtualAddr);
    CHECK(a.offererVirtualAddr != b.answererVirtualAddr);
    CHECK(a.answererVirtualAddr != b.offererVirtualAddr);
    CHECK(a.answererVirtualAddr != b.answererVirtualAddr);
  }

  // After renewal the old generation is stale. A late packet carrying it must
  // be refused rather than applied to the new attempt.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session session = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    CHECK(coordinator.IsCurrentParticipant(
        session.sessionId, session.generation, QStringLiteral("connlost")));
    const quint32 renewed = coordinator.Renew(session.sessionId);
    CHECK(renewed != session.generation);
    CHECK(!coordinator.IsCurrentParticipant(
        session.sessionId, session.generation, QStringLiteral("connlost")));
    CHECK(coordinator.IsCurrentParticipant(session.sessionId, renewed,
                                           QStringLiteral("connlost")));
    // Renewal keeps the id, so the client does not have to re-begin.
    const std::optional<Peer::Session> found =
        coordinator.Find(session.sessionId);
    CHECK(found.has_value());
    CHECK(found->generation == renewed);
  }

  // A third account must never be able to signal into someone else's pair.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session session = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    CHECK(!coordinator.IsCurrentParticipant(
        session.sessionId, session.generation, QStringLiteral("intruder")));
    CHECK(!coordinator.IsCurrentParticipant(session.sessionId + 1000,
                                            session.generation,
                                            QStringLiteral("connlost")));
  }

  // Ending a session is idempotent; a second End must not misbehave.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session session = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    coordinator.End(session.sessionId);
    CHECK(coordinator.SessionCount() == 0);
    coordinator.End(session.sessionId);
    CHECK(coordinator.SessionCount() == 0);
    CHECK(!coordinator.Find(session.sessionId).has_value());
  }

  // After a session is ended, beginning the same attempt again must work
  // rather than resurrecting the removed pair index entry.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session first = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    coordinator.End(first.sessionId);
    const Peer::Session again = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    CHECK(again.sessionId != first.sessionId);
    CHECK(coordinator.SessionCount() == 1);
    CHECK(coordinator.Find(again.sessionId).has_value());
  }

  // Logout drops every session the account was in, and reports them so the
  // remaining peer can be told rather than left waiting.
  {
    Peer::SessionCoordinator coordinator;
    coordinator.BeginOrJoin(QStringLiteral("connlost"),
                            QStringLiteral("MintCoffeeCat"),
                            QStringLiteral("CUSA03173"), 1, kNow);
    coordinator.BeginOrJoin(QStringLiteral("connlost"),
                            QStringLiteral("SomeoneElse"),
                            QStringLiteral("CUSA03173"), 1, kNow);
    coordinator.BeginOrJoin(QStringLiteral("MintCoffeeCat"),
                            QStringLiteral("SomeoneElse"),
                            QStringLiteral("CUSA03173"), 1, kNow);
    CHECK(coordinator.SessionCount() == 3);
    const QList<Peer::Session> dropped =
        coordinator.DropParticipant(QStringLiteral("connlost"));
    CHECK(dropped.size() == 2);
    CHECK(coordinator.SessionCount() == 1);
    for (const Peer::Session &session : dropped) {
      CHECK(session.IsParticipant(QStringLiteral("connlost")));
      CHECK(!coordinator.Find(session.sessionId).has_value());
    }
    // The pair index must have been cleaned too: beginning that same attempt
    // again has to create a fresh session, not return a dangling id.
    const Peer::Session again = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    CHECK(coordinator.Find(again.sessionId).has_value());
    CHECK(coordinator.SessionCount() == 2);
  }

  std::cout << "peer session tests passed\n";
  return 0;
}

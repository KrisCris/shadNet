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

  // Two different pairs must not be handed the same address.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session a = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    const Peer::Session b = coordinator.BeginOrJoin(
        QStringLiteral("Hunter"), QStringLiteral("Doll"),
        QStringLiteral("CUSA03173"), 1, kNow);
    CHECK(a.offererVirtualAddr != b.offererVirtualAddr);
    CHECK(a.offererVirtualAddr != b.answererVirtualAddr);
    CHECK(a.answererVirtualAddr != b.offererVirtualAddr);
    CHECK(a.answererVirtualAddr != b.answererVirtualAddr);
  }

  // Three players in one room, each paired with the other two.
  //
  // A player's virtual address is its identity: it is what the peer's socket
  // layer sees as the source of every datagram, and what the player itself
  // advertises into room data. A third player joining must not change what
  // the first two already know, so an account's address has to be the same in
  // every session it takes part in.
  {
    Peer::SessionCoordinator coordinator;
    const QString first = QStringLiteral("connlost");
    const QString second = QStringLiteral("MintCoffeeCat");
    const QString third = QStringLiteral("PaleBlood");
    const QString title = QStringLiteral("CUSA03173");

    const Peer::Session ab = coordinator.BeginOrJoin(first, second, title, 1, kNow);
    const Peer::Session ac = coordinator.BeginOrJoin(first, third, title, 1, kNow);
    const Peer::Session bc = coordinator.BeginOrJoin(second, third, title, 1, kNow);

    CHECK(coordinator.SessionCount() == 3);

    CHECK(ab.VirtualAddrOf(first) == ac.VirtualAddrOf(first));
    CHECK(ab.VirtualAddrOf(second) == bc.VirtualAddrOf(second));
    CHECK(ac.VirtualAddrOf(third) == bc.VirtualAddrOf(third));

    // And the three players must still be distinguishable from each other.
    CHECK(ab.VirtualAddrOf(first) != ab.VirtualAddrOf(second));
    CHECK(ab.VirtualAddrOf(first) != ac.VirtualAddrOf(third));
    CHECK(ab.VirtualAddrOf(second) != ac.VirtualAddrOf(third));
  }

  // A retry must not move a player either. The peers that are already talking
  // to it did not retry, and the address it advertised into room data does not
  // change because one of its connections was re-rung.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session first = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    const Peer::Session second = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 2, kNow);

    // A different session, but the same two players in it.
    CHECK(first.sessionId != second.sessionId);
    CHECK(first.offererVirtualAddr == second.offererVirtualAddr);
    CHECK(first.answererVirtualAddr == second.answererVirtualAddr);
  }

  // An account that disconnects gives its address up, so the range is not
  // consumed by players who have gone.
  {
    Peer::SessionCoordinator coordinator;
    const Peer::Session before = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    coordinator.DropParticipant(QStringLiteral("connlost"));
    const Peer::Session after = coordinator.BeginOrJoin(
        QStringLiteral("connlost"), QStringLiteral("MintCoffeeCat"),
        QStringLiteral("CUSA03173"), 1, kNow);
    // Reconnecting is a new lease, not the old one.
    CHECK(after.VirtualAddrOf(QStringLiteral("connlost")) !=
          before.VirtualAddrOf(QStringLiteral("connlost")));
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

  // TURN credentials follow coturn's use-auth-secret scheme exactly, so this
  // asserts against the documented construction rather than restating our own
  // implementation.
  {
    const Peer::TurnCredential c = Peer::MakeTurnCredential(
        QStringLiteral("connlost"), QByteArrayLiteral("testsecret"), 1757548800,
        3600);
    CHECK(c.expiresAt == 1757552400ull);
    CHECK(c.username == QStringLiteral("1757552400:connlost"));
    // Fixed vector, computed independently of this code:
    //   base64(HMAC-SHA1(key="testsecret", msg="1757552400:connlost"))
    // Recomputing it here with the same Qt call would only restate the
    // implementation and would pass even if the scheme were wrong.
    CHECK(c.credential == QStringLiteral("QUGSNM71FCId8R1W5qOaN9uMm0M="));
  }

  // Two accounts must never share a credential, or the relay cannot attribute
  // an allocation to one of them.
  {
    const Peer::TurnCredential a = Peer::MakeTurnCredential(
        QStringLiteral("connlost"), QByteArrayLiteral("s"), 0, 60);
    const Peer::TurnCredential b = Peer::MakeTurnCredential(
        QStringLiteral("MintCoffeeCat"), QByteArrayLiteral("s"), 0, 60);
    CHECK(a.username != b.username);
    CHECK(a.credential != b.credential);
  }

  // Rotating the secret must invalidate credentials for the same username.
  {
    const Peer::TurnCredential a = Peer::MakeTurnCredential(
        QStringLiteral("connlost"), QByteArrayLiteral("secret-one"), 100, 60);
    const Peer::TurnCredential b = Peer::MakeTurnCredential(
        QStringLiteral("connlost"), QByteArrayLiteral("secret-two"), 100, 60);
    CHECK(a.username == b.username);
    CHECK(a.credential != b.credential);
  }

  std::cout << "peer session tests passed\n";
  return 0;
}

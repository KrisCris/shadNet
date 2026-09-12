// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "peer_address_log.h"

#include <iostream>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << "Failed at line " << __LINE__ << ": " << #expression << '\n';             \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

int main() {
    Peer::AddressLog addresses;
    CHECK(addresses.AddObserved(QHostAddress("::ffff:203.0.113.1")));
    CHECK(!addresses.AddObserved(QHostAddress("203.0.113.1")));
    CHECK(addresses.AddCandidates(
        "a=ice-pwd:DO-NOT-LOG\r\n"
        "a=candidate:1 1 UDP 123 192.168.1.2 12345 typ host\r\n"
        "a=candidate:2 1 UDP 122 2001:db8::2 12345 typ host\r\n"
        "a=candidate:3 1 UDP 121 203.0.113.2 23456 typ srflx raddr 192.168.1.2 rport 12345\r\n"
        "a=candidate:4 1 UDP 120 203.0.113.3 31525 typ relay raddr 0.0.0.0 rport 0\r\n"));
    CHECK(!addresses.AddCandidates("candidate:2 1 UDP 122 2001:db8::2 12345 typ host"));
    CHECK(!addresses.AddCandidates("a=candidate:broken\na=ice-ufrag:DO-NOT-LOG"));
    CHECK(!addresses.AddCandidates("candidate:1 1 UDP 123 invalid-address 12345 typ host"));
    const QString result = addresses.Format("player");
    CHECK(result.startsWith("player: ["));
    CHECK(result.contains("observed-tcp=[203.0.113.1]"));
    CHECK(result.contains("reported-host=[192.168.1.2, 2001:db8::2]"));
    CHECK(result.contains("reported-srflx=[203.0.113.2]"));
    CHECK(result.contains("reported-relay=[203.0.113.3]"));
    CHECK(!result.contains("DO-NOT-LOG"));
    CHECK(!result.contains("0.0.0.0"));
    std::cout << result.toStdString() << '\n';
}

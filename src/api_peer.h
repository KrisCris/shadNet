// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHostAddress>
#include <QHttpServerRequest>
#include <QString>
#include <QStringList>

namespace ShadNet {

// Both REST APIs throttle failed logins per peer, and both take the peer from
// the socket address. That is right when clients connect directly, and wrong
// the moment a web frontend sits in front: every member then shares one bucket,
// so eight wrong passwords from anybody locks out everybody.
//
// ApiTrustedProxies names the hops allowed to speak for someone else. It is
// empty by default -- X-Forwarded-For is attacker-controlled, so honouring it
// unconditionally would let anyone dodge the throttle by inventing a new value
// per attempt. Entries are addresses ("127.0.0.1"), CIDR subnets
// ("172.16.0.0/12"), or the keyword "local" for loopback plus the private
// ranges, which covers both the docker bridge and host networking.
//
// The rightmost X-Forwarded-For entry is used: that is the address seen by the
// last hop before us, the only one in the list our trusted proxy actually
// vouched for. Anything further left was copied from the client verbatim.
inline bool IsTrustedProxy(const QHostAddress& addr, const QStringList& trusted) {
    if (addr.isNull() || trusted.isEmpty())
        return false;
    for (const QString& entry : trusted) {
        const QString e = entry.trimmed();
        if (e.isEmpty())
            continue;
        if (e.compare(QLatin1String("local"), Qt::CaseInsensitive) == 0) {
            if (addr.isLoopback() ||
                addr.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8) ||
                addr.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12) ||
                addr.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16) ||
                addr.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7)) {
                return true;
            }
            continue;
        }
        if (e.contains(QLatin1Char('/'))) {
            const auto subnet = QHostAddress::parseSubnet(e);
            if (!subnet.first.isNull() && addr.isInSubnet(subnet))
                return true;
            continue;
        }
        const QHostAddress exact(e);
        if (!exact.isNull() && addr.isEqual(exact, QHostAddress::ConvertV4MappedToIPv4))
            return true;
    }
    return false;
}

inline QString ResolvePeer(const QHttpServerRequest& req, const QStringList& trusted) {
    const QHostAddress addr = req.remoteAddress();
    if (IsTrustedProxy(addr, trusted)) {
        const QByteArray raw = req.value("X-Forwarded-For");
        if (!raw.isEmpty()) {
            const QList<QByteArray> hops = raw.split(',');
            const QString last = QString::fromUtf8(hops.last()).trimmed();
            // Only accept something that actually parses as an address, so a
            // junk header cannot create unlimited distinct throttle buckets.
            if (!QHostAddress(last).isNull())
                return last;
        }
    }
    return addr.isNull() ? QStringLiteral("unknown") : addr.toString();
}

} // namespace ShadNet

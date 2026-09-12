// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "peer_address_log.h"

#include <QRegularExpression>
#include <QStringList>

namespace Peer {

bool AddressLog::Add(const QString& source, const QHostAddress& address) {
    if (address.isNull() || address == QHostAddress::AnyIPv4 || address == QHostAddress::AnyIPv6)
        return false;
    bool isV4 = false;
    const quint32 v4 = address.toIPv4Address(&isV4);
    const QString text = isV4 ? QHostAddress(v4).toString() : address.toString();
    auto& values = m_addresses[source];
    if (values.contains(text))
        return false;
    values.insert(text);
    return true;
}

bool AddressLog::AddObserved(const QHostAddress& address) {
    return Add(QStringLiteral("observed-tcp"), address);
}

bool AddressLog::AddCandidates(const QString& description) {
    bool changed = false;
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    for (const QString& line : description.split(QLatin1Char('\n'))) {
        const QString candidate = line.trimmed();
        if (!candidate.startsWith(QStringLiteral("a=candidate:")) &&
            !candidate.startsWith(QStringLiteral("candidate:")))
            continue;
        const QStringList fields = candidate.split(whitespace, Qt::SkipEmptyParts);
        if (fields.size() < 8 || fields[6] != QStringLiteral("typ"))
            continue;
        const QString& kind = fields[7];
        if (kind != QStringLiteral("host") && kind != QStringLiteral("srflx") &&
            kind != QStringLiteral("prflx") && kind != QStringLiteral("relay"))
            continue;
        changed |= Add(QStringLiteral("reported-") + kind, QHostAddress(fields[4]));
        for (qsizetype i = 8; i + 1 < fields.size(); i += 2) {
            if (fields[i] == QStringLiteral("raddr"))
                changed |= Add(QStringLiteral("reported-related"), QHostAddress(fields[i + 1]));
        }
    }
    return changed;
}

QString AddressLog::Format(const QString& npid) const {
    QSet<QString> all;
    QStringList sources;
    for (auto it = m_addresses.cbegin(); it != m_addresses.cend(); ++it) {
        all.unite(it.value());
        QStringList values = it.value().values();
        values.sort();
        sources.append(it.key() + QStringLiteral("=[") + values.join(QStringLiteral(", ")) +
                       QLatin1Char(']'));
    }
    QStringList values = all.values();
    values.sort();
    return npid + QStringLiteral(": [") + values.join(QStringLiteral(", ")) + QStringLiteral("] ") +
           sources.join(QLatin1Char(' '));
}

} // namespace Peer

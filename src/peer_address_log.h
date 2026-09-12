// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHostAddress>
#include <QMap>
#include <QSet>
#include <QString>

namespace Peer {

// Diagnostic addresses only. Client-reported candidates are not proof of reachability.
class AddressLog {
public:
    bool AddObserved(const QHostAddress& address);
    bool AddCandidates(const QString& description);
    QString Format(const QString& npid) const;

private:
    bool Add(const QString& source, const QHostAddress& address);
    QMap<QString, QSet<QString>> m_addresses;
};

} // namespace Peer

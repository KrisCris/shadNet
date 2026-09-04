// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

#include "database.h"

struct TrophyConfig {
    QString comId;
    QString titleName;
    QString language;
    QList<Database::TrophyMetaRow> trophies;
    // Trophy groups: the base game, plus a group per DLC pack. Empty for a title
    // that declares none, which is normal for a game with no add-ons.
    QList<Database::TrophyGroupRow> groups;

    // Empty when parsing succeeded.
    QString error;

    bool ok() const {
        return error.isEmpty();
    }
};

TrophyConfig ParseTrophyConfig(const QByteArray& xml, const QString& language = QString());

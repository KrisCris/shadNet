// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "trophy_config.h"

#include <QXmlStreamReader>

QString NormaliseGrade(const QString& raw) {
    const QString g = raw.trimmed().toUpper();
    if (g == QLatin1String("B") || g == QLatin1String("S") || g == QLatin1String("G") ||
        g == QLatin1String("P")) {
        return g;
    }
    return QString();
}

bool ParseHidden(const QString& raw) {
    const QString h = raw.trimmed().toLower();
    return h == QLatin1String("yes") || h == QLatin1String("true") || h == QLatin1String("1");
}

constexpr int MaxNameLength = 256;
constexpr int MaxDetailLength = 1024;

TrophyConfig ParseTrophyConfig(const QByteArray& xml, const QString& language) {
    TrophyConfig cfg;
    cfg.language = language;

    if (xml.isEmpty()) {
        cfg.error = QStringLiteral("The file is empty.");
        return cfg;
    }

    QXmlStreamReader r(xml);
    bool sawTrophyConf = false;

    while (!r.atEnd() && !r.hasError()) {
        if (r.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        const QStringView name = r.name();

        if (name == QLatin1String("trophyconf")) {
            sawTrophyConf = true;
            continue;
        }
        if (!sawTrophyConf) {
            continue; // ignore anything before the root we care about
        }

        if (name == QLatin1String("npcommid")) {
            cfg.comId = r.readElementText().trimmed();
            continue;
        }
        if (name == QLatin1String("title-name")) {
            cfg.titleName = r.readElementText().trimmed().left(MaxNameLength);
            continue;
        }
        if (name == QLatin1String("group")) {
            // <group id="000"><name>Base Game</name><detail>…</detail></group>
            bool gidOk = false;
            const int gid = r.attributes().value(QLatin1String("id")).toInt(&gidOk);
            if (!gidOk || gid < 0) {
                continue;
            }
            Database::TrophyGroupRow group;
            group.groupId = gid;
            while (!r.atEnd() && !r.hasError()) {
                const auto token = r.readNext();
                if (token == QXmlStreamReader::EndElement && r.name() == QLatin1String("group")) {
                    break;
                }
                if (token != QXmlStreamReader::StartElement) {
                    continue;
                }
                if (r.name() == QLatin1String("name")) {
                    group.name = r.readElementText().trimmed().left(MaxNameLength);
                } else if (r.name() == QLatin1String("detail")) {
                    group.detail = r.readElementText().trimmed().left(MaxDetailLength);
                }
            }
            // A group with no name would render as an empty heading, which is
            // worse than folding those trophies into the default section.
            if (!group.name.isEmpty()) {
                cfg.groups.append(group);
            }
            continue;
        }
        if (name != QLatin1String("trophy")) {
            continue;
        }

        const QXmlStreamAttributes attrs = r.attributes();
        bool idOk = false;
        const int trophyId = attrs.value(QLatin1String("id")).toInt(&idOk);
        if (!idOk || trophyId < 0) {
            // Skip rather than fail: one bad entry should not lose the whole file.
            continue;
        }

        Database::TrophyMetaRow row;
        row.trophyId = trophyId;
        row.grade = NormaliseGrade(attrs.value(QLatin1String("ttype")).toString());
        row.hidden = ParseHidden(attrs.value(QLatin1String("hidden")).toString());
        row.groupId = attrs.value(QLatin1String("gid")).toInt();
        row.language = language;

        // <name> and <detail> are children of <trophy>; walk to its end element.
        while (!r.atEnd() && !r.hasError()) {
            const auto token = r.readNext();
            if (token == QXmlStreamReader::EndElement && r.name() == QLatin1String("trophy")) {
                break;
            }
            if (token != QXmlStreamReader::StartElement) {
                continue;
            }
            if (r.name() == QLatin1String("name")) {
                row.name = r.readElementText().trimmed().left(MaxNameLength);
            } else if (r.name() == QLatin1String("detail")) {
                row.detail = r.readElementText().trimmed().left(MaxDetailLength);
            }
        }

        if (row.name.isEmpty()) {
            continue;
        }
        cfg.trophies.append(row);
    }

    if (r.hasError()) {
        cfg.error = QStringLiteral("Line %1: %2").arg(r.lineNumber()).arg(r.errorString());
        return cfg;
    }
    if (!sawTrophyConf) {
        cfg.error = QStringLiteral("No <trophyconf> element — this does not look like a "
                                   "TROP.XML trophy configuration.");
        return cfg;
    }
    if (cfg.trophies.isEmpty()) {
        cfg.error = QStringLiteral("No named trophies found in the file.");
        return cfg;
    }
    return cfg;
}

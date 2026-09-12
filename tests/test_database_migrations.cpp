// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <QCoreApplication>
#include <QSqlError>
#include <QTemporaryDir>
#include "database.h"

namespace {
QStringList messages;
void CaptureMessage(QtMsgType, const QMessageLogContext&, const QString& message) {
    messages.append(message);
}
int AppliedCount() {
    int count = 0;
    for (const QString& message : messages)
        count += message.startsWith("Applied database migration");
    return count;
}
bool Exec(QSqlDatabase db, const QString& sql) {
    QSqlQuery query(db);
    if (query.exec(sql))
        return true;
    std::cerr << query.lastError().text().toStdString() << '\n';
    return false;
}
int Scalar(QSqlDatabase db, const QString& sql) {
    QSqlQuery query(db);
    if (!query.exec(sql) || !query.next())
        return -1;
    return query.value(0).toInt();
}
} // namespace

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << "Failed at line " << __LINE__ << ": " << #expression << '\n';             \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qInstallMessageHandler(CaptureMessage);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString path = directory.filePath("test.db");
    {
        Database db("initial");
        CHECK(db.Open(path));
        CHECK(!db.Conn().tables().contains("account"));
        CHECK(db.Migrate());
        CHECK(AppliedCount() == 7);
        CHECK(Scalar(db.Conn(), "SELECT COUNT(*) FROM migration") == 7);
        CHECK(Exec(db.Conn(), "INSERT INTO score_table(communication_id, board_id) VALUES('', 1)"));
    }
    messages.clear();
    {
        Database db("reopened");
        CHECK(db.Open(path));
        // Opening a connection neither migrates nor runs startup maintenance.
        CHECK(AppliedCount() == 0);
        CHECK(Scalar(db.Conn(), "SELECT COUNT(*) FROM score_table") == 1);
        CHECK(db.Migrate());
        CHECK(AppliedCount() == 0);
        db.RunMaintenance();
        CHECK(Scalar(db.Conn(), "SELECT COUNT(*) FROM score_table") == 0);

        // Upgrade an existing database missing its last migration.
        CHECK(Exec(db.Conn(), "DROP TABLE trophy_group"));
        CHECK(Exec(db.Conn(), "DELETE FROM migration WHERE migration_id=7"));
        CHECK(db.Migrate());
        CHECK(AppliedCount() == 1);
        CHECK(db.Conn().tables().contains("trophy_group"));

        // The second ALTER fails. The first ALTER and version marker must
        // both roll back, so the failed migration can be retried safely.
        messages.clear();
        CHECK(Exec(db.Conn(), "ALTER TABLE account DROP COLUMN client_version"));
        CHECK(Exec(db.Conn(), "DELETE FROM migration WHERE migration_id=6"));
        CHECK(!db.Migrate());
        CHECK(AppliedCount() == 0);
        CHECK(Scalar(db.Conn(), "SELECT COUNT(*) FROM pragma_table_info('account') "
                                "WHERE name='client_version'") == 0);
        CHECK(Scalar(db.Conn(), "SELECT COUNT(*) FROM migration WHERE migration_id=6") == 0);
        CHECK(Exec(db.Conn(), "ALTER TABLE account DROP COLUMN client_version_at"));
        CHECK(db.Migrate());
        CHECK(AppliedCount() == 1);
    }
    std::cout << "PASS: explicit migration, quiet reopen, upgrade, rollback and retry\n";
}

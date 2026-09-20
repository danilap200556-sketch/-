/* Диагностика: подключение и запрос через НАСТОЯЩИЙ Qt QSqlDatabase/QSqlQuery
 * (не голый libpq, как в tlsdiag.exe) - точное воспроизведение того, что
 * делает Database::open() в приложении. Нужно, чтобы проверить гипотезу:
 * Qt для PostgreSQL может использовать protocol-level prepared statements
 * даже для простых запросов, а это плохо совместимо с транзакционным
 * pooler-режимом (PgBouncer/Neon pooler) - тогда как "голый" PQexec из
 * tlsdiag.exe (простой протокол запроса) работает нормально.
 *
 * Использование: qtpgtest.exe host port user database password
 */
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);

    if (argc < 6) {
        out << "Использование: qtpgtest.exe host port user database password\n";
        return 2;
    }
    const QString host = argv[1];
    const int port = QString(argv[2]).toInt();
    const QString user = argv[3];
    const QString dbname = argv[4];
    const QString password = argv[5];

    out << "Доступные SQL-драйверы: " << QSqlDatabase::drivers().join(", ") << "\n";

    QSqlDatabase db = QSqlDatabase::addDatabase("QPSQL");
    db.setHostName(host);
    db.setPort(port);
    db.setDatabaseName(dbname);
    db.setUserName(user);
    db.setPassword(password);
    db.setConnectOptions("sslmode=require;sslnegotiation=postgres;connect_timeout=20");

    out << "Подключаюсь через QSqlDatabase::open()...\n";
    if (!db.open()) {
        out << "OPEN FAILED: " << db.lastError().text() << "\n";
        return 1;
    }
    out << ">>> db.open() OK <<<\n\n";

    out << "Выполняю тестовый запрос через QSqlQuery::exec(QString)...\n";
    QSqlQuery q(db);
    const bool ok1 = q.exec("CREATE TABLE IF NOT EXISTS qtpgtest_probe (id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY)");
    out << "Результат 1 (CREATE TABLE, exec(QString)): " << (ok1 ? "OK" : "FAIL") << "\n";
    if (!ok1)
        out << "  lastError: " << q.lastError().text() << "\n";

    if (ok1) {
        out << "\nВыполняю второй запрос через prepare()+exec() (с bind-параметром)...\n";
        QSqlQuery q2(db);
        q2.prepare("INSERT INTO qtpgtest_probe DEFAULT VALUES");
        const bool ok2 = q2.exec();
        out << "Результат 2 (INSERT через prepare): " << (ok2 ? "OK" : "FAIL") << "\n";
        if (!ok2)
            out << "  lastError: " << q2.lastError().text() << "\n";

        out << "\nВыполняю третий запрос (DROP TABLE, exec(QString))...\n";
        QSqlQuery q3(db);
        const bool ok3 = q3.exec("DROP TABLE IF EXISTS qtpgtest_probe");
        out << "Результат 3 (DROP TABLE): " << (ok3 ? "OK" : "FAIL") << "\n";
        if (!ok3)
            out << "  lastError: " << q3.lastError().text() << "\n";
    }

    out << "\nСоединение всё ещё открыто: " << (db.isOpen() ? "да" : "НЕТ") << "\n";
    db.close();
    return 0;
}

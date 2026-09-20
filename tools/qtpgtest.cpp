/* Диагностика: подключение и запрос через НАСТОЯЩИЙ Qt QSqlDatabase/QSqlQuery
 * (не голый libpq, как в tlsdiag.exe) - точное воспроизведение того, что
 * делает Database::open() в приложении.
 *
 * Использование: qtpgtest.exe host port user database password
 */
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QTextStream>

namespace {

void printError(QTextStream &out, const QString &label, const QSqlError &err)
{
    out << "  [" << label << "] type=" << int(err.type())
        << " nativeErrorCode=" << err.nativeErrorCode() << "\n";
    out << "  [" << label << "] databaseText=\"" << err.databaseText() << "\"\n";
    out << "  [" << label << "] driverText=\"" << err.driverText() << "\"\n";
}

} // namespace

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
        out << "OPEN FAILED\n";
        printError(out, "open", db.lastError());
        return 1;
    }
    out << ">>> db.open() OK, isOpen=" << (db.isOpen() ? "true" : "false") << " <<<\n\n";

    // Тест 0: максимально простой запрос без каких-либо DDL/типов.
    {
        out << "--- Тест 0: SELECT 1 через exec(QString) ---\n";
        QSqlQuery q(db);
        const bool ok = q.exec("SELECT 1");
        out << "Результат: " << (ok ? "OK" : "FAIL") << ", isOpen после=" << (db.isOpen() ? "true" : "false") << "\n";
        if (ok) {
            if (q.next())
                out << "  значение: " << q.value(0).toString() << "\n";
        } else {
            printError(out, "SELECT 1 / query", q.lastError());
            printError(out, "SELECT 1 / db", db.lastError());
        }
        out << "\n";
    }

    if (!db.isOpen()) {
        out << ">>> Соединение уже закрыто после SELECT 1 - дальше пробовать бессмысленно. <<<\n";
        return 1;
    }

    // Тест 1: CREATE TABLE, как в реальном Database::open().
    {
        out << "--- Тест 1: CREATE TABLE через exec(QString) ---\n";
        QSqlQuery q(db);
        const bool ok = q.exec("CREATE TABLE IF NOT EXISTS qtpgtest_probe (id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY)");
        out << "Результат: " << (ok ? "OK" : "FAIL") << ", isOpen после=" << (db.isOpen() ? "true" : "false") << "\n";
        if (!ok) {
            printError(out, "CREATE / query", q.lastError());
            printError(out, "CREATE / db", db.lastError());
        } else {
            QSqlQuery drop(db);
            drop.exec("DROP TABLE IF EXISTS qtpgtest_probe");
        }
        out << "\n";
    }

    out << "Соединение всё ещё открыто: " << (db.isOpen() ? "да" : "НЕТ") << "\n";
    db.close();
    return 0;
}

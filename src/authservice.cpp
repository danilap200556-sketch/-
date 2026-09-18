#include "authservice.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>

namespace {

QString randomSaltHex(int bytes = 16)
{
    QByteArray buf(bytes, Qt::Uninitialized);
    for (int i = 0; i < bytes; ++i)
        buf[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    return QString::fromLatin1(buf.toHex());
}

QString hashPassword(const QString &password, const QString &saltHex)
{
    const QByteArray salted = QByteArray::fromHex(saltHex.toLatin1()) + password.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(salted, QCryptographicHash::Sha256).toHex());
}

} // namespace

bool AuthService::hasAnyUser()
{
    QSqlQuery q("SELECT COUNT(*) FROM users");
    if (!q.next())
        return false;
    return q.value(0).toInt() > 0;
}

bool AuthService::createUser(const QString &username, const QString &password, QString *error)
{
    const QString trimmed = username.trimmed();
    if (trimmed.isEmpty()) {
        if (error)
            *error = QStringLiteral("Введите логин");
        return false;
    }
    if (password.size() < 4) {
        if (error)
            *error = QStringLiteral("Пароль должен быть не короче 4 символов");
        return false;
    }

    const QString salt = randomSaltHex();
    const QString hash = hashPassword(password, salt);

    QSqlQuery q;
    q.prepare("INSERT INTO users (username, password_hash, salt) VALUES (?, ?, ?)");
    q.addBindValue(trimmed);
    q.addBindValue(hash);
    q.addBindValue(salt);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text().contains("UNIQUE", Qt::CaseInsensitive)
                         ? QStringLiteral("Пользователь с таким логином уже существует")
                         : q.lastError().text();
        return false;
    }
    return true;
}

bool AuthService::verifyLogin(const QString &username, const QString &password, QString *error)
{
    QSqlQuery q;
    q.prepare("SELECT password_hash, salt FROM users WHERE username = ?");
    q.addBindValue(username.trimmed());
    if (!q.exec() || !q.next()) {
        if (error)
            *error = QStringLiteral("Неверный логин или пароль");
        return false;
    }

    const QString storedHash = q.value(0).toString();
    const QString salt = q.value(1).toString();
    if (hashPassword(password, salt) != storedHash) {
        if (error)
            *error = QStringLiteral("Неверный логин или пароль");
        return false;
    }
    return true;
}

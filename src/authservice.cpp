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

bool AuthService::validatePassword(const QString &password, QString *error)
{
    if (password.size() < 4) {
        if (error)
            *error = QStringLiteral("Пароль должен быть не короче 4 символов");
        return false;
    }
    return true;
}

bool AuthService::createUser(const QString &username, const QString &password, bool isAdmin,
                             QString *error)
{
    return createUser(username, password, isAdmin ? Role::Admin : Role::Editor, error);
}

bool AuthService::createUser(const QString &username, const QString &password, Role role, QString *error)
{
    const QString trimmed = username.trimmed();
    if (trimmed.isEmpty()) {
        if (error)
            *error = QStringLiteral("Введите логин");
        return false;
    }
    if (!validatePassword(password, error))
        return false;

    const QString salt = randomSaltHex();
    const QString hash = hashPassword(password, salt);

    QSqlQuery q;
    q.prepare("INSERT INTO users (username, password_hash, salt, is_admin, read_only) VALUES (?, ?, ?, ?, ?)");
    q.addBindValue(trimmed);
    q.addBindValue(hash);
    q.addBindValue(salt);
    q.addBindValue(role == Role::Admin);
    q.addBindValue(role == Role::Viewer);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().nativeErrorCode() == QLatin1String("23505")
                         ? QStringLiteral("Пользователь с таким логином уже существует")
                         : q.lastError().text();
        return false;
    }
    return true;
}

QList<AuthService::User> AuthService::listUsers()
{
    QList<User> users;
    QSqlQuery q("SELECT id, username, is_admin, read_only AND NOT is_admin, created_at FROM users ORDER BY username");
    while (q.next())
        users.append({q.value(0).toInt(), q.value(1).toString(), q.value(2).toBool(), q.value(3).toBool(),
                      q.value(4).toDateTime()});
    return users;
}

bool AuthService::isAdmin(const QString &username)
{
    QSqlQuery q;
    q.prepare("SELECT is_admin FROM users WHERE username = ?");
    q.addBindValue(username.trimmed());
    return q.exec() && q.next() && q.value(0).toBool();
}

bool AuthService::setPassword(int userId, const QString &password, QString *error)
{
    if (!validatePassword(password, error))
        return false;
    const QString salt = randomSaltHex();
    QSqlQuery q;
    q.prepare("UPDATE users SET password_hash = ?, salt = ? WHERE id = ?");
    q.addBindValue(hashPassword(password, salt));
    q.addBindValue(salt);
    q.addBindValue(userId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

namespace {

// Условие "кроме этого пользователя останется хотя бы один администратор"
// проверяется в том же UPDATE/DELETE, а не отдельным SELECT до него - иначе
// два админа, одновременно разжаловавшие друг друга, оставили бы базу без админов.
const char *kOtherAdminExists =
    "EXISTS (SELECT 1 FROM users o WHERE o.is_admin AND o.id <> users.id)";

bool execGuarded(QSqlQuery &q, QString *error)
{
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    if (q.numRowsAffected() == 0) {
        if (error)
            *error = QStringLiteral("Нельзя оставить приложение без администратора");
        return false;
    }
    return true;
}

} // namespace

bool AuthService::setAdmin(int userId, bool admin, QString *error)
{
    return setRole(userId, admin ? Role::Admin : Role::Editor, error);
}

bool AuthService::setRole(int userId, Role role, QString *error)
{
    QSqlQuery q;
    if (role == Role::Admin) {
        q.prepare("UPDATE users SET is_admin = TRUE, read_only = FALSE WHERE id = ?");
        q.addBindValue(userId);
        if (!q.exec()) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
        return true;
    }
    // Лишить прав администратора можно, только если останется другой администратор.
    q.prepare(QStringLiteral("UPDATE users SET is_admin = FALSE, read_only = ? WHERE id = ? AND "
                             "(NOT is_admin OR %1)").arg(QLatin1String(kOtherAdminExists)));
    q.addBindValue(role == Role::Viewer);
    q.addBindValue(userId);
    return execGuarded(q, error);
}

QString AuthService::roleName(Role role)
{
    switch (role) {
    case Role::Admin: return QStringLiteral("Администратор");
    case Role::Editor: return QStringLiteral("Сотрудник");
    case Role::Viewer: return QStringLiteral("Только просмотр");
    }
    return QString();
}

AuthService::Role AuthService::roleOf(const User &user)
{
    return user.isAdmin ? Role::Admin : (user.readOnly ? Role::Viewer : Role::Editor);
}

bool AuthService::canEdit(const QString &username)
{
    QSqlQuery q;
    q.prepare("SELECT NOT (read_only AND NOT is_admin) FROM users WHERE username = ?");
    q.addBindValue(username.trimmed());
    return q.exec() && q.next() && q.value(0).toBool();
}

bool AuthService::deleteUser(int userId, QString *error)
{
    QSqlQuery q;
    q.prepare(QStringLiteral("DELETE FROM users WHERE id = ? AND (NOT is_admin OR %1)")
                  .arg(QLatin1String(kOtherAdminExists)));
    q.addBindValue(userId);
    return execGuarded(q, error);
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

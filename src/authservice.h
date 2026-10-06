#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

// Учётные записи приложения (логин + пароль, хранятся в общей базе на
// сервере; пароль - только в виде соли+хеша, никогда в открытом виде).
// Роли: администратор (всё, включая пользователей и кабинеты Маркета),
// сотрудник (редактирует данные) и "только просмотр" (ничего не меняет).
// Последнего администратора удалить или разжаловать нельзя.
class AuthService
{
public:
    enum class Role { Admin, Editor, Viewer };

    struct User {
        int id = 0;
        QString username;
        bool isAdmin = false;
        bool readOnly = false; // только просмотр (у администратора всегда false)
        QDateTime createdAt;
    };

    static bool hasAnyUser();

    static bool createUser(const QString &username, const QString &password, bool isAdmin,
                           QString *error = nullptr);
    static bool createUser(const QString &username, const QString &password, Role role,
                           QString *error = nullptr);

    static bool verifyLogin(const QString &username, const QString &password, QString *error = nullptr);

    static QList<User> listUsers();
    static bool isAdmin(const QString &username);
    // Может ли пользователь менять данные (false - роль "только просмотр").
    static bool canEdit(const QString &username);
    static QString roleName(Role role);
    static Role roleOf(const User &user);
    static bool setRole(int userId, Role role, QString *error = nullptr);
    static bool setPassword(int userId, const QString &password, QString *error = nullptr);
    static bool setAdmin(int userId, bool admin, QString *error = nullptr);
    static bool deleteUser(int userId, QString *error = nullptr);

    static bool validatePassword(const QString &password, QString *error = nullptr);
};

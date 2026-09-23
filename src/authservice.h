#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

// Учётные записи приложения (логин + пароль, хранятся в общей базе на
// сервере; пароль - только в виде соли+хеша, никогда в открытом виде).
// Администратор может заводить и удалять пользователей и настраивать
// кабинеты Маркета; последнего администратора удалить или разжаловать нельзя.
class AuthService
{
public:
    struct User {
        int id = 0;
        QString username;
        bool isAdmin = false;
        QDateTime createdAt;
    };

    static bool hasAnyUser();

    static bool createUser(const QString &username, const QString &password, bool isAdmin,
                           QString *error = nullptr);

    static bool verifyLogin(const QString &username, const QString &password, QString *error = nullptr);

    static QList<User> listUsers();
    static bool isAdmin(const QString &username);
    static bool setPassword(int userId, const QString &password, QString *error = nullptr);
    static bool setAdmin(int userId, bool admin, QString *error = nullptr);
    static bool deleteUser(int userId, QString *error = nullptr);

    static bool validatePassword(const QString &password, QString *error = nullptr);
};

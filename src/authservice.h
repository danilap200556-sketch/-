#pragma once

#include <QString>

// Простая локальная аутентификация (логин + пароль, хранятся в локальной
// SQLite базе, пароль - только в виде соли+хеша, никогда в открытом виде).
//
// Задел на будущее: сейчас это заглушка "на одну машину", но интерфейс
// (hasAnyUser/createUser/verifyLogin) не завязан на то, что проверка идёт
// локально - когда появится центральный сервер/несколько кабинетов,
// verifyLogin можно будет заменить на сетевой вызов, не трогая LoginDialog.
class AuthService
{
public:
    static bool hasAnyUser();

    static bool createUser(const QString &username, const QString &password, QString *error = nullptr);

    static bool verifyLogin(const QString &username, const QString &password, QString *error = nullptr);
};

#pragma once

#include <QString>

// Параметры подключения к серверу PostgreSQL, на котором лежит общая база
// склада. Хранятся локально (на каждой машине) через QSettings, чтобы не
// вводить их заново при каждом запуске.
struct ServerConfig
{
    QString host;
    int port = 5432;
    QString database;
    QString user;
    QString password;
    bool useSsl = true;

    bool isComplete() const
    {
        return !host.isEmpty() && !database.isEmpty() && !user.isEmpty();
    }

    static ServerConfig load();
    void save() const;
};

#include "serverconfig.h"

#include <QSettings>

namespace {
constexpr auto kGroup = "server";
}

ServerConfig ServerConfig::load()
{
    QSettings settings;
    settings.beginGroup(kGroup);
    ServerConfig cfg;
    cfg.host = settings.value("host").toString();
    cfg.port = settings.value("port", 5432).toInt();
    cfg.database = settings.value("database").toString();
    cfg.user = settings.value("user").toString();
    cfg.password = settings.value("password").toString();
    cfg.useSsl = settings.value("useSsl", true).toBool();
    settings.endGroup();
    return cfg;
}

void ServerConfig::save() const
{
    QSettings settings;
    settings.beginGroup(kGroup);
    settings.setValue("host", host);
    settings.setValue("port", port);
    settings.setValue("database", database);
    settings.setValue("user", user);
    settings.setValue("password", password);
    settings.setValue("useSsl", useSsl);
    settings.endGroup();
}

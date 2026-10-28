#include "appsettings.h"

#include <QSqlQuery>
#include <QVariant>

namespace AppSettings {

QString get(const QString &key, const QString &fallback)
{
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT value FROM app_settings WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return fallback;
}

bool set(const QString &key, const QString &value)
{
    QSqlQuery q;
    q.prepare(QStringLiteral("INSERT INTO app_settings (key, value) VALUES (?, ?) "
                             "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    return q.exec();
}

} // namespace AppSettings

#include "appversion.h"

#include <QDate>
#include <QLocale>
#include <QtGlobal>

#ifndef APP_BUILD
#define APP_BUILD 0
#endif
// Коммит всегда непустой (иначе MSVC не принимает пустой аргумент макроса): «local» - собрано не в CI.
#ifndef APP_COMMIT
#define APP_COMMIT local
#endif
#define APP_STR2(x) #x
#define APP_STR(x) APP_STR2(x)

namespace AppVersion {

int build()
{
    if (qEnvironmentVariableIsSet("INVENTORY_FAKE_BUILD"))
        return qEnvironmentVariableIntValue("INVENTORY_FAKE_BUILD");
    return APP_BUILD;
}

QString commit()
{
    const QString c = QString::fromLatin1(APP_STR(APP_COMMIT)).trimmed();
    return c == QLatin1String("local") ? QString() : c;
}

QString builtOn()
{
    // __DATE__ - «Oct  3 2026» (дата компиляции этого файла)
    const QDate d = QLocale::c().toDate(QString::fromLatin1(__DATE__).simplified(), QStringLiteral("MMM d yyyy"));
    return d.isValid() ? d.toString(QStringLiteral("dd.MM.yyyy")) : QString();
}

QString text()
{
    const int b = build();
    if (b <= 0)
        return QStringLiteral("версия для разработки");
    const QString c = commit();
    return c.isEmpty() ? QStringLiteral("сборка %1").arg(b) : QStringLiteral("сборка %1 (%2)").arg(b).arg(c);
}

} // namespace AppVersion

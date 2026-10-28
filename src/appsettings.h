#pragma once

#include <QString>

// Общие настройки программы в базе (таблица app_settings): ключ Google для поиска фото, параметры Честного знака.
// Лежат в базе, а не на компьютере, чтобы на всех компьютерах были одни и те же значения.
namespace AppSettings {

QString get(const QString &key, const QString &fallback = QString());
bool set(const QString &key, const QString &value);

} // namespace AppSettings

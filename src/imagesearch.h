#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <functional>

// Поиск картинок в интернете по названию товара: сначала Bing (картинки), если он не ответил или ограничил запросы -
// DuckDuckGo. Ключей и регистрации не нужно, но это обычные страницы поисковиков: формат у них может меняться, поэтому
// разбор ответов вынесен отдельно (parse*), а адреса можно подменить переменными окружения INVENTORY_BING_URL и
// INVENTORY_DDG_URL (для проверок).
namespace ImageSearch {

struct Hit {
    QString url;      // сама картинка
    QString thumbUrl; // миниатюра на сервере поисковика (запасной вариант, если оригинал не скачался)
    QString pageUrl;  // страница, где картинку нашли
};

// Запрос по названию товара: без размера обуви/одежды в конце ("Nike Air Force 1 42" -> "Nike Air Force 1"), без лишних
// пробелов. Название пустое - берётся артикул.
QString cleanQuery(const QString &name, const QString &sku);

QList<Hit> parseBing(const QByteArray &html);
QString parseDdgToken(const QByteArray &html);
QList<Hit> parseDdg(const QByteArray &json);

// Ищет до limit картинок. false - ни один поисковик не ответил (в error причина; "остановлено", если остановили);
// true с пустым списком - ответили, но ничего не нашли. cancelled опрашивается во время ожидания.
bool search(const QString &query, int limit, QList<Hit> *hits, QString *error, const std::function<bool()> &cancelled = {});

} // namespace ImageSearch

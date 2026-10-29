#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// Поиск фото товара в интернете ПО АРТИКУЛУ (а не по названию: поиск по названию приносил случайные картинки).
// Источники по порядку:
//   1) «Магазины» - открытые интернет-магазины кроссовок на Shopify: поиск по артикулу (/search/suggest.json), затем карточка
//      товара (/products/<имя>.js). Снимки берутся, только если артикул нашёлся в артикулах вариантов, названии, тегах, адресе
//      товара или имени файла картинки - то есть это тот самый товар, а не похожий.
//   2) «New Balance» - официальный каталог картинок New Balance (адрес строится из артикула; есть не у всех моделей).
//   3) «Google» - официальный поиск картинок (Custom Search API), только если задан ключ.
// У каждого результата есть признак «проверено». Все запросы - обычные открытые адреса, без входа и без обхода защиты: если
// магазин отвечает отказом (403/429), до перезапуска программы к нему больше не обращаемся.
// Адреса источников подменяются переменными окружения (для проверок), каждый запрос записывается в Reply::trace - по нему
// видно, что именно ответил источник.
//   INVENTORY_SHOPS (адреса магазинов через запятую), INVENTORY_NB_URL, INVENTORY_GOOGLE_CSE_URL, INVENTORY_WEB_PAUSE_MS.
namespace ImageSearch {

enum Source { Shops = 1, NewBalance = 2, Google = 4, AllSources = 7 };

QString sourceName(Source source); // «Магазины», «New Balance», «Google»

struct Hit {
    QString url;        // сама картинка
    QString thumbUrl;   // миниатюра (запасной вариант, если оригинал не скачался)
    QString pageUrl;    // страница, где картинку нашли
    QString title;      // подпись: название товара
    QString source;     // sourceName, для магазинов - «Магазин: адрес»
    bool verified = false;
};

// Ключ и идентификатор поисковой системы Google Custom Search (programmablesearchengine.google.com): без них Google не
// используется (обычную страницу поиска он без скриптов не отдаёт).
struct Settings {
    QString googleKey;
    QString googleCx;
    bool hasGoogleApi() const { return !googleKey.trimmed().isEmpty() && !googleCx.trimmed().isEmpty(); }
};

using Cancel = std::function<bool()>;

struct Reply {
    QList<Hit> hits;
    QString error;      // источник не ответил или отказал; пусто - ответил (возможно, без результатов)
    QStringList trace;  // что спрашивали и что ответили (для диагностики; ключ API скрыт)
    bool stopped = false;
    bool skipped = false; // источник к этому артикулу не применим, ничего не спрашивали (не считается ни ответом, ни отказом)
};

// Заглавные буквы и цифры, остальное выброшено: «dd1391-100» == «DD1391 100».
QString normalize(const QString &text);
// Артикул есть в тексте. Артикулы короче 5 знаков не проверяются (подходят к чему угодно).
bool mentions(const QString &text, const QString &article);
// Что искать: сначала без размера на конце («DD1391-100-42» -> «DD1391-100»), затем артикул как есть.
QStringList articleVariants(const QString &article);

Reply searchShops(const QString &article, const Cancel &cancelled = {});
Reply searchNewBalance(const QString &article, const Cancel &cancelled = {});
Reply searchGoogle(const QString &article, const Settings &settings, const Cancel &cancelled = {});
Reply search(Source source, const QString &article, const Settings &settings, const Cancel &cancelled = {});
// Магазины, которые ответили отказом, больше не спрашиваются; эта функция снимает запрет (для проверок и кнопки «Проверить»).
void forgetRefusals();

// --- разбор ответов (вынесен для проверок)
// Подсказки поиска магазина (/search/suggest.json): адреса товаров (handle).
QStringList parseShopSuggest(const QByteArray &json);
// Карточка товара (/products/<handle>.js): снимки, если артикул есть в артикулах вариантов, названии, тегах, имени товара или файла.
QList<Hit> parseShopProduct(const QByteArray &json, const QString &shopLabel, const QString &shopBase, const QString &article);
QList<Hit> parseGoogleCse(const QByteArray &json, const QString &article, QString *error);

} // namespace ImageSearch

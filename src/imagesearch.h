#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// Поиск фото товара в интернете ПО АРТИКУЛУ (а не по названию: поиск по названию приносил случайные картинки).
// Источники по порядку: Pinterest, Google (картинки). У каждого результата есть признак «проверено»: артикул нашёлся в
// названии, описании, адресе страницы или адресе картинки. Непроверенное сохраняется только по желанию пользователя, обычно
// его показывают в окне просмотра.
//
// У Pinterest нет открытого API для поиска, поэтому берётся то же, что получает сайт: JSON-запрос поиска и, если он не
// ответил, страница поиска. Страницы Google без ключа меняются без предупреждения. Поэтому разбор ответов вынесен в parse*,
// адреса подменяются переменными окружения (для проверок), а каждый запрос записывается в Reply::trace - по нему видно,
// что именно ответил источник.
//   INVENTORY_PINTEREST_URL, INVENTORY_GOOGLE_URL, INVENTORY_GOOGLE_CSE_URL.
namespace ImageSearch {

enum Source { Pinterest = 1, Google = 2, AllSources = 3 };

QString sourceName(Source source); // «Pinterest», «Google»

struct Hit {
    QString url;        // сама картинка
    QString thumbUrl;   // миниатюра (запасной вариант, если оригинал не скачался)
    QString pageUrl;    // страница, где картинку нашли
    QString title;      // подпись: название товара
    QString source;     // sourceName
    bool verified = false;
};

// Ключ и идентификатор поисковой системы Google Custom Search (programmablesearchengine.google.com): с ними Google
// отдаёт картинки официальным API (100 запросов в день бесплатно), без них разбирается обычная страница поиска.
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
};

// Заглавные буквы и цифры, остальное выброшено: «dd1391-100» == «DD1391 100».
QString normalize(const QString &text);
// Артикул есть в тексте. Артикулы короче 5 знаков не проверяются (подходят к чему угодно).
bool mentions(const QString &text, const QString &article);
// Что искать: сначала без размера на конце («DD1391-100-42» -> «DD1391-100»), затем артикул как есть.
QStringList articleVariants(const QString &article);

Reply searchPinterest(const QString &article, const Cancel &cancelled = {});
Reply searchGoogle(const QString &article, const Settings &settings, const Cancel &cancelled = {});
Reply search(Source source, const QString &article, const Settings &settings, const Cancel &cancelled = {});

// --- разбор ответов (вынесен для проверок)
// Pinterest: ответ JSON-запроса поиска (resource_response.data.results) или любой JSON со страницы - пины с images.orig.
QList<Hit> parsePinterestJson(const QByteArray &json, const QString &article, QString *error = nullptr);
// Pinterest: страница поиска - JSON внутри страницы, а если его нет - любые адреса i.pinimg.com (приводятся к оригиналам).
QList<Hit> parsePinterestHtml(const QByteArray &html, const QString &article);
QList<Hit> parseGoogleCse(const QByteArray &json, const QString &article, QString *error);
QList<Hit> parseGooglePage(const QByteArray &html, const QString &article);

} // namespace ImageSearch

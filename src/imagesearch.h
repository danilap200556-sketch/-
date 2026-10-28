#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// Поиск фото товара в интернете ПО АРТИКУЛУ (а не по названию: поиск по названию приносил случайные картинки).
// Источники по порядку: Poizon, Google (картинки), Wildberries. У каждого результата есть признак «проверено»: артикул
// нашёлся в названии, подписи, адресе страницы или в данных товара. Непроверенное сохраняется только по желанию
// пользователя, обычно его показывают в окне просмотра.
//
// Ни у Poizon, ни у Wildberries нет официального открытого API для картинок, а страницы Google без ключа меняются без
// предупреждения, поэтому разбор ответов вынесен в parse*, адреса подменяются переменными окружения (для проверок), а
// каждый запрос записывается в Reply::trace - по нему видно, что именно ответил источник.
//   INVENTORY_POIZON_URL, INVENTORY_GOOGLE_URL, INVENTORY_GOOGLE_CSE_URL, INVENTORY_WB_SEARCH_URL,
//   INVENTORY_WB_BASKET_URL (шаблон с {nn} - номером «корзины» Wildberries).
namespace ImageSearch {

enum Source { Poizon = 1, Google = 2, Wildberries = 4, AllSources = 7 };

QString sourceName(Source source); // «Poizon», «Google», «Wildberries»

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

Reply searchPoizon(const QString &article, const Settings &settings, const Cancel &cancelled = {});
Reply searchGoogle(const QString &article, const Settings &settings, const Cancel &cancelled = {});
Reply searchWildberries(const QString &article, const Cancel &cancelled = {});
Reply search(Source source, const QString &article, const Settings &settings, const Cancel &cancelled = {});

// --- разбор ответов (вынесен для проверок)
QList<Hit> parsePoizon(const QByteArray &body, const QString &article);
QList<Hit> parseGoogleCse(const QByteArray &json, const QString &article, QString *error);
QList<Hit> parseGooglePage(const QByteArray &html, const QString &article);

struct WbProduct {
    qint64 id = 0;
    QString name;
    QString brand;
};
QList<WbProduct> parseWbSearch(const QByteArray &json);
int wbBasketNumber(qint64 id);               // номер «корзины» с файлами карточки (01...)
QString wbBasketBase(qint64 id);             // https://basket-NN.wbbasket.ru
QString wbImageUrl(qint64 id, int number);   // большое фото №number

} // namespace ImageSearch

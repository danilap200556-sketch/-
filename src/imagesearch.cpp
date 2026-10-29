#include "imagesearch.h"

#include <QMutex>
#include <QMutexLocker>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace ImageSearch {

namespace {

constexpr int kTimeoutMs = 20000;
constexpr int kMaxHits = 12;             // сколько результатов оставляем от одного источника
constexpr int kMaxShopsAsked = 6;        // у скольких магазинов спрашиваем артикул, пока не нашлось
constexpr int kShopProducts = 3;         // сколько товаров из подсказок магазина открываем
constexpr int kShopPauseMs = 250;        // пауза между запросами к магазину
constexpr int kMinImageBytes = 200;      // меньше - это точно не картинка
const char kUserAgent[] = "Mozilla/5.0 (compatible; InventoryManager photo lookup)";
const char kStopped[] = "остановлено";

QString envOr(const char *name, const QString &fallback)
{
    const QString v = qEnvironmentVariable(name);
    return v.isEmpty() ? fallback : v;
}

struct Page {
    QByteArray body;
    int status = 0;
    QString error; // пусто - получили ответ 2xx/3xx
};

Page get(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers, const Cancel &cancelled)
{
    Page page;
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setTransferTimeout(kTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", kUserAgent);
    req.setRawHeader("Accept-Language", "ru-RU,ru;q=0.9,en;q=0.5");
    for (const auto &h : headers)
        req.setRawHeader(h.first, h.second);
    QNetworkReply *reply = nam.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer watch;
    watch.setInterval(150);
    bool stopped = false, polling = false;
    QObject::connect(&watch, &QTimer::timeout, &loop, [&]() {
        if (polling || !cancelled)
            return;
        polling = true;
        const bool asked = cancelled();
        polling = false;
        if (asked) {
            stopped = true;
            reply->abort();
        }
    });
    watch.start();
    loop.exec();
    page.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    page.body = reply->readAll();
    if (stopped)
        page.error = QString::fromUtf8(kStopped);
    else if (reply->error() == QNetworkReply::TimeoutError)
        page.error = QStringLiteral("не ответил за %1 с").arg(kTimeoutMs / 1000);
    else if (reply->error() != QNetworkReply::NoError || page.status >= 400)
        page.error = page.status ? QStringLiteral("HTTP %1").arg(page.status) : reply->errorString();
    delete reply;
    return page;
}

// Адрес для журнала без секретов (ключ API).
QString safeUrl(const QUrl &url)
{
    QUrl u = url;
    QUrlQuery q(u);
    if (q.hasQueryItem(QStringLiteral("key"))) {
        q.removeQueryItem(QStringLiteral("key"));
        q.addQueryItem(QStringLiteral("key"), QStringLiteral("***"));
    }
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

// Один запрос с записью в журнал. Возвращает страницу; reply->error/stopped заполняются по ней.
Page fetch(Reply *reply, const QString &label, const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers, const Cancel &cancelled)
{
    Page p = get(url, headers, cancelled);
    reply->trace << QStringLiteral("%1: GET %2 -> %3, %4 байт").arg(label, safeUrl(url), p.error.isEmpty() ? QStringLiteral("HTTP %1").arg(p.status) : p.error).arg(p.body.size());
    if (p.error == QString::fromUtf8(kStopped))
        reply->stopped = true;
    return p;
}

QString absoluteUrl(QString link)
{
    link = link.trimmed();
    if (link.startsWith(QLatin1String("//")))
        link.prepend(QStringLiteral("https:"));
    return link;
}

void addHit(QList<Hit> *hits, QSet<QString> *seen, Hit h)
{
    h.url = absoluteUrl(h.url);
    h.thumbUrl = absoluteUrl(h.thumbUrl);
    if (h.url.isEmpty() || seen->contains(h.url))
        return;
    seen->insert(h.url);
    hits->append(h);
}

void pause(int ms, const Cancel &cancelled)
{
    if (ms <= 0)
        return;
    QEventLoop loop;
    QTimer done, watch;
    done.setSingleShot(true);
    QObject::connect(&done, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&watch, &QTimer::timeout, &loop, [&]() {
        if (cancelled && cancelled())
            loop.quit();
    });
    done.start(ms);
    watch.start(100);
    loop.exec();
}

int shopPauseMs()
{
    bool ok = false;
    const int v = qEnvironmentVariable("INVENTORY_WEB_PAUSE_MS").toInt(&ok);
    return ok ? v : kShopPauseMs;
}

// Магазины, которые отказали (403/429): до перезапуска программы не спрашиваем.
QMutex g_refusedLock;
QSet<QString> g_refused;

bool isRefused(const QString &base)
{
    QMutexLocker lock(&g_refusedLock);
    return g_refused.contains(base);
}

void markRefused(const QString &base)
{
    QMutexLocker lock(&g_refusedLock);
    g_refused.insert(base);
}

// Открытые магазины на Shopify, по убыванию того, как часто у них находился нужный артикул.
const char *const kShopHosts[] = {
    "extrabutterny.com",
    "www.theclosetinc.com",
    "www.sneakersnstuff.com",
    "www.dtlr.com",
    "kith.com",
    "www.overkillshop.com",
    "www.solefly.com",
    "shoegallerymiami.com",
};

QStringList shopBases()
{
    QStringList out;
    const QString env = qEnvironmentVariable("INVENTORY_SHOPS");
    if (!env.isEmpty()) {
        for (QString s : env.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            s = s.trimmed();
            while (s.endsWith(QLatin1Char('/')))
                s.chop(1);
            if (!s.isEmpty())
                out << s;
        }
        return out;
    }
    for (const char *host : kShopHosts)
        out << QStringLiteral("https://") + QLatin1String(host);
    return out;
}

// Как спросить магазин: артикул как есть и, для кодов Nike (DD1391100), с дефисом (DD1391-100) - так он записан в магазинах.
QStringList shopQueries(const QString &article)
{
    QStringList out{article.simplified()};
    const QString n = normalize(article);
    static const QRegularExpression nike(QStringLiteral("^[A-Z]{2}\\d{4}\\d{3}$"));
    if (nike.match(n).hasMatch()) {
        const QString dashed = n.left(6) + QLatin1Char('-') + n.mid(6);
        if (!out.contains(dashed, Qt::CaseInsensitive))
            out << dashed;
    }
    return out;
}

// Артикул как отдельное «слово» в тексте: «DD1391-100», «dd1391 100», но не «DD13911000» и не «XDD1391100».
bool mentionsWord(const QString &text, const QString &article)
{
    const QString n = normalize(article);
    if (n.size() < 5)
        return false;
    QString pattern = QStringLiteral("(?<![A-Za-z0-9])");
    for (int i = 0; i < n.size(); ++i) {
        if (i > 0)
            pattern += QStringLiteral("[^A-Za-z0-9]{0,3}");
        pattern += QRegularExpression::escape(n.mid(i, 1));
    }
    pattern += QStringLiteral("(?![A-Za-z0-9])");
    return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption).match(text).hasMatch();
}

QString fileNameOf(const QString &link)
{
    const QUrl u(absoluteUrl(link));
    return u.isValid() ? u.fileName() : link;
}

} // namespace

QString sourceName(Source source)
{
    switch (source) {
    case Shops: return QStringLiteral("Магазины");
    case NewBalance: return QStringLiteral("New Balance");
    case Google: return QStringLiteral("Google");
    default: return QString();
    }
}

QString normalize(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text)
        if (c.isLetterOrNumber())
            out.append(c.toUpper());
    return out;
}

bool mentions(const QString &text, const QString &article)
{
    const QString n = normalize(article);
    return n.size() >= 5 && normalize(text).contains(n);
}

QStringList articleVariants(const QString &article)
{
    QStringList out;
    const QString a = article.simplified();
    if (a.isEmpty())
        return out;
    // Размер обуви/одежды в конце артикула («DD1391-100-42», «DD1391-100 42.5») мешает искать модель: сначала ищем
    // без размера (так один поиск обслуживает все размеры), потом - как записано.
    static const QRegularExpression size(QStringLiteral("^(.*[A-Za-zА-Яа-я0-9])[-_/ ]+(?:3[4-9]|4[0-9]|50)(?:[.,]5)?$"));
    const auto m = size.match(a);
    if (m.hasMatch()) {
        const QString base = m.captured(1);
        static const QRegularExpression hasDigit(QStringLiteral("\\d"));
        if (normalize(base).size() >= 5 && base.contains(hasDigit))
            out << base;
    }
    out << a;
    return out;
}

// ---------------------------------------------------------------- Магазины (Shopify)

QStringList parseShopSuggest(const QByteArray &json)
{
    QStringList handles;
    const QJsonArray products = QJsonDocument::fromJson(json).object().value(QStringLiteral("resources")).toObject().value(QStringLiteral("results")).toObject()
                                    .value(QStringLiteral("products")).toArray();
    for (const QJsonValue &v : products) {
        const QString handle = v.toObject().value(QStringLiteral("handle")).toString();
        if (!handle.isEmpty() && !handles.contains(handle))
            handles << handle;
    }
    return handles;
}

QList<Hit> parseShopProduct(const QByteArray &json, const QString &shopLabel, const QString &shopBase, const QString &article)
{
    QList<Hit> hits;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.isEmpty())
        return hits;
    const QString handle = o.value(QStringLiteral("handle")).toString();
    const QString title = o.value(QStringLiteral("title")).toString();
    QStringList images;
    for (const QJsonValue &v : o.value(QStringLiteral("images")).toArray())
        if (v.isString() && !v.toString().isEmpty())
            images << absoluteUrl(v.toString());
    if (images.isEmpty())
        return hits;

    // Где нашёлся артикул: в артикулах вариантов (самое надёжное), в адресе товара, тегах, названии или имени файла картинки.
    bool found = false;
    for (const QJsonValue &v : o.value(QStringLiteral("variants")).toArray())
        found = found || mentionsWord(v.toObject().value(QStringLiteral("sku")).toString(), article);
    found = found || mentionsWord(handle, article) || mentionsWord(title, article);
    const QJsonValue tags = o.value(QStringLiteral("tags"));
    if (tags.isArray()) {
        for (const QJsonValue &t : tags.toArray())
            found = found || mentionsWord(t.toString(), article);
    } else {
        found = found || mentionsWord(tags.toString(), article);
    }
    for (int i = 0; i < images.size() && i < 4; ++i)
        found = found || mentionsWord(fileNameOf(images[i]), article);
    if (!found)
        return hits;

    QSet<QString> seen;
    const QString page = shopBase + QStringLiteral("/products/") + handle;
    for (const QString &img : images) {
        Hit h;
        h.url = img;
        h.thumbUrl = img;
        h.pageUrl = page;
        h.title = title;
        h.source = QStringLiteral("Магазин: ") + shopLabel;
        h.verified = true;
        addHit(&hits, &seen, h);
        if (hits.size() >= kMaxHits)
            break;
    }
    return hits;
}

Reply searchShops(const QString &article, const Cancel &cancelled)
{
    Reply r;
    const QStringList queries = shopQueries(article);
    int asked = 0, answered = 0;
    QStringList refusals;
    for (const QString &base : shopBases()) {
        if (cancelled && cancelled()) {
            r.stopped = true;
            r.error = QString::fromUtf8(kStopped);
            return r;
        }
        if (isRefused(base))
            continue;
        if (asked >= kMaxShopsAsked)
            break;
        ++asked;
        const QUrl baseUrl(base);
        const QString label = baseUrl.host() + (baseUrl.port() > 0 ? QStringLiteral(":%1").arg(baseUrl.port()) : QString()) + baseUrl.path();
        QStringList handles;
        bool failed = false;
        for (const QString &q : queries) {
            QUrl url(base + QStringLiteral("/search/suggest.json"));
            QUrlQuery qq;
            qq.addQueryItem(QStringLiteral("q"), q);
            qq.addQueryItem(QStringLiteral("resources[type]"), QStringLiteral("product"));
            qq.addQueryItem(QStringLiteral("resources[limit]"), QStringLiteral("4"));
            url.setQuery(qq);
            pause(shopPauseMs(), cancelled);
            const Page p = fetch(&r, label, url, {{"Accept", "application/json"}}, cancelled);
            if (r.stopped) {
                r.error = QString::fromUtf8(kStopped);
                return r;
            }
            if (!p.error.isEmpty()) {
                if (p.status == 403 || p.status == 429 || p.status == 430)
                    markRefused(base);
                refusals << QStringLiteral("%1: %2").arg(label, p.error);
                failed = true;
                break;
            }
            for (const QString &h : parseShopSuggest(p.body))
                if (!handles.contains(h))
                    handles << h;
            if (!handles.isEmpty())
                break;
        }
        if (failed)
            continue;
        ++answered;
        for (int i = 0; i < handles.size() && i < kShopProducts; ++i) {
            pause(shopPauseMs(), cancelled);
            const Page p = fetch(&r, label, QUrl(base + QStringLiteral("/products/") + handles[i] + QStringLiteral(".js")), {{"Accept", "application/json"}}, cancelled);
            if (r.stopped) {
                r.error = QString::fromUtf8(kStopped);
                return r;
            }
            if (!p.error.isEmpty()) {
                if (p.status == 403 || p.status == 429 || p.status == 430) {
                    markRefused(base);
                    refusals << QStringLiteral("%1: %2").arg(label, p.error);
                    break;
                }
                continue;
            }
            const QList<Hit> hits = parseShopProduct(p.body, label, base, article);
            if (!hits.isEmpty()) {
                r.hits = hits;
                r.trace << QStringLiteral("Магазины: артикул %1 найден в %2 (товар %3), снимков %4").arg(article, label, handles[i]).arg(hits.size());
                return r;
            }
        }
    }
    if (answered == 0 && !refusals.isEmpty())
        r.error = refusals.join(QStringLiteral("; "));
    else
        r.trace << QStringLiteral("Магазины: артикул %1 не найден (спросили магазинов: %2)").arg(article).arg(answered);
    return r;
}

// ---------------------------------------------------------------- New Balance

Reply searchNewBalance(const QString &article, const Cancel &cancelled)
{
    Reply r;
    // Артикулы New Balance: буквы-цифры-буквы («U574WPG», «MR530SG», «BB480LEC»). Остальное (Nike, adidas) не спрашиваем.
    const QString code = normalize(article).toLower();
    static const QRegularExpression style(QStringLiteral("^[a-z]{1,3}\\d{3,4}[a-z0-9]{0,5}$"));
    static const QRegularExpression nikeLike(QStringLiteral("^[a-z]{2}\\d{7}$"));
    if (!style.match(code).hasMatch() || nikeLike.match(code).hasMatch()) {
        r.skipped = true;
        r.trace << QStringLiteral("New Balance: «%1» не похож на артикул New Balance").arg(article);
        return r;
    }
    const QString base = envOr("INVENTORY_NB_URL", QStringLiteral("https://nb.scene7.com/is/image/NB"));
    for (const QString &view : {QStringLiteral("02"), QStringLiteral("03"), QStringLiteral("04"), QStringLiteral("05")}) {
        const QString link = QStringLiteral("%1/%2_nb_%3_i?$pdpflexf2$&wid=800&hei=800").arg(base, code, view);
        pause(shopPauseMs(), cancelled);
        const Page p = fetch(&r, QStringLiteral("New Balance"), QUrl(link, QUrl::TolerantMode), {{"Accept", "image/*"}}, cancelled);
        if (r.stopped) {
            r.error = QString::fromUtf8(kStopped);
            return r;
        }
        // Нет такого снимка: каталог отвечает на несуществующий отказом (403). Нет первого - у модели нет снимков вовсе.
        if (!p.error.isEmpty() || p.body.size() < kMinImageBytes) {
            if (r.hits.isEmpty()) {
                if (p.status != 403 && p.status != 404 && !p.error.isEmpty())
                    r.error = p.error;
                break;
            }
            continue;
        }
        Hit h;
        h.url = link;
        h.thumbUrl = link;
        h.pageUrl = QStringLiteral("https://www.newbalance.com/");
        h.title = QStringLiteral("New Balance %1").arg(code.toUpper());
        h.source = sourceName(NewBalance);
        h.verified = true;
        r.hits << h;
    }
    r.trace << QStringLiteral("New Balance: снимков %1").arg(r.hits.size());
    return r;
}

// ---------------------------------------------------------------- Google

QList<Hit> parseGoogleCse(const QByteArray &json, const QString &article, QString *error)
{
    QList<Hit> hits;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (root.contains(QStringLiteral("error"))) {
        const QJsonObject e = root.value(QStringLiteral("error")).toObject();
        if (error)
            *error = QStringLiteral("%1 %2").arg(e.value(QStringLiteral("code")).toInt()).arg(e.value(QStringLiteral("message")).toString());
        return hits;
    }
    QSet<QString> seen;
    for (const QJsonValue &v : root.value(QStringLiteral("items")).toArray()) {
        const QJsonObject o = v.toObject();
        const QJsonObject image = o.value(QStringLiteral("image")).toObject();
        Hit h;
        h.url = o.value(QStringLiteral("link")).toString();
        h.thumbUrl = image.value(QStringLiteral("thumbnailLink")).toString();
        h.pageUrl = image.value(QStringLiteral("contextLink")).toString();
        h.title = o.value(QStringLiteral("title")).toString();
        h.source = sourceName(Google);
        h.verified = mentions(h.title + QLatin1Char(' ') + o.value(QStringLiteral("snippet")).toString() + QLatin1Char(' ') + h.pageUrl + QLatin1Char(' ') + h.url, article);
        addHit(&hits, &seen, h);
    }
    return hits;
}

Reply searchGoogle(const QString &article, const Settings &settings, const Cancel &cancelled)
{
    Reply r;
    if (!settings.hasGoogleApi()) {
        r.error = QStringLiteral("не задан ключ Google API");
        return r;
    }
    QUrl url(envOr("INVENTORY_GOOGLE_CSE_URL", QStringLiteral("https://www.googleapis.com/customsearch/v1")));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("key"), settings.googleKey.trimmed());
    q.addQueryItem(QStringLiteral("cx"), settings.googleCx.trimmed());
    q.addQueryItem(QStringLiteral("q"), QLatin1Char('"') + article + QLatin1Char('"'));
    q.addQueryItem(QStringLiteral("searchType"), QStringLiteral("image"));
    q.addQueryItem(QStringLiteral("num"), QStringLiteral("10"));
    q.addQueryItem(QStringLiteral("safe"), QStringLiteral("off"));
    q.addQueryItem(QStringLiteral("hl"), QStringLiteral("ru"));
    url.setQuery(q);
    const Page p = fetch(&r, QStringLiteral("Google API"), url, {{"Accept", "application/json"}}, cancelled);
    if (r.stopped) {
        r.error = QString::fromUtf8(kStopped);
        return r;
    }
    QString apiError;
    r.hits = parseGoogleCse(p.body, article, &apiError);
    if (!apiError.isEmpty() || !p.error.isEmpty()) {
        r.error = apiError.isEmpty() ? p.error : apiError;
        r.trace << QStringLiteral("Google API: отказ - %1").arg(r.error);
        return r;
    }
    r.trace << QStringLiteral("Google API: найдено картинок %1 (проверенных %2)").arg(r.hits.size()).arg(std::count_if(r.hits.cbegin(), r.hits.cend(), [](const Hit &h) { return h.verified; }));
    return r;
}

Reply search(Source source, const QString &article, const Settings &settings, const Cancel &cancelled)
{
    switch (source) {
    case Shops: return searchShops(article, cancelled);
    case NewBalance: return searchNewBalance(article, cancelled);
    case Google: return searchGoogle(article, settings, cancelled);
    default: return {};
    }
}

void forgetRefusals()
{
    QMutexLocker lock(&g_refusedLock);
    g_refused.clear();
}

} // namespace ImageSearch

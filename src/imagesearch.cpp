#include "imagesearch.h"

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
const char kUserAgent[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
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

QString unescapeHtml(QString s)
{
    s.replace(QLatin1String("&quot;"), QLatin1String("\""));
    s.replace(QLatin1String("&#39;"), QLatin1String("'"));
    s.replace(QLatin1String("&lt;"), QLatin1String("<"));
    s.replace(QLatin1String("&gt;"), QLatin1String(">"));
    s.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return s;
}

// \u0026 и \/ из строк JavaScript -> обычные символы.
QString unescapeJs(QString s)
{
    static const QRegularExpression u(QStringLiteral("\\\\u([0-9a-fA-F]{4})"));
    QString out;
    int last = 0;
    for (auto it = u.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        out += QChar(m.captured(1).toUShort(nullptr, 16));
        last = m.capturedEnd();
    }
    out += s.mid(last);
    out.replace(QLatin1String("\\/"), QLatin1String("/"));
    return out;
}

QString absoluteUrl(QString link)
{
    link = link.trimmed();
    if (link.startsWith(QLatin1String("//")))
        link.prepend(QStringLiteral("https:"));
    return link;
}

bool looksLikeImage(const QString &link)
{
    const QUrl u(link, QUrl::StrictMode);
    if (!u.isValid() || u.host().isEmpty() || (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https")))
        return false;
    static const QRegularExpression ext(QStringLiteral("\\.(?:jpe?g|png|webp|gif|bmp)(?:$|[?#!/_])"), QRegularExpression::CaseInsensitiveOption);
    return ext.match(u.path() + QLatin1Char('?') + u.query()).hasMatch() || ext.match(link).hasMatch();
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

bool isGstatic(const QString &link)
{
    return link.contains(QLatin1String("gstatic.com")) || link.contains(QLatin1String("googleusercontent.com"));
}

// --- Poizon: ищем в разобранном JSON страницы объекты «товар», у которых есть и артикул, и картинки.
const QStringList kPoizonArticleKeys = {QStringLiteral("articlenumber"), QStringLiteral("article_number"), QStringLiteral("spuarticlenumber"),
                                        QStringLiteral("article"), QStringLiteral("itemno"), QStringLiteral("styleid"), QStringLiteral("skucode")};
const QStringList kPoizonImageKeys = {QStringLiteral("logourl"), QStringLiteral("imageurl"), QStringLiteral("image"), QStringLiteral("img"),
                                      QStringLiteral("picurl"), QStringLiteral("mainpic"), QStringLiteral("coverurl"), QStringLiteral("cover"),
                                      QStringLiteral("pic"), QStringLiteral("thumb"), QStringLiteral("url")};
const QStringList kPoizonListKeys = {QStringLiteral("images"), QStringLiteral("imagelist"), QStringLiteral("gallery"), QStringLiteral("pics"),
                                     QStringLiteral("piclist"), QStringLiteral("detailimages"), QStringLiteral("spuimages")};

void collectImages(const QJsonValue &v, QStringList *out, int depth = 0)
{
    if (depth > 4)
        return;
    if (v.isString()) {
        const QString s = absoluteUrl(v.toString());
        if (looksLikeImage(s))
            *out << s;
    } else if (v.isArray()) {
        for (const QJsonValue &x : v.toArray())
            collectImages(x, out, depth + 1);
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            const QString k = it.key().toLower();
            if (kPoizonImageKeys.contains(k) || kPoizonListKeys.contains(k))
                collectImages(it.value(), out, depth + 1);
        }
    }
}

void walkPoizon(const QJsonValue &v, const QString &article, QList<Hit> *hits, QSet<QString> *seen, int depth = 0)
{
    if (depth > 24)
        return;
    if (v.isArray()) {
        for (const QJsonValue &x : v.toArray())
            walkPoizon(x, article, hits, seen, depth + 1);
        return;
    }
    if (!v.isObject())
        return;
    const QJsonObject o = v.toObject();
    QString articleValue, title;
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString k = it.key().toLower();
        if (it.value().isString() && kPoizonArticleKeys.contains(k))
            articleValue = it.value().toString();
        else if (it.value().isString() && (k == QLatin1String("title") || k == QLatin1String("name") || k == QLatin1String("productname")))
            title = it.value().toString();
    }
    if (!articleValue.isEmpty()) {
        QStringList images;
        collectImages(o, &images);
        const bool match = normalize(articleValue) == normalize(article) || mentions(articleValue, article) || mentions(article, articleValue);
        for (const QString &img : std::as_const(images)) {
            Hit h;
            h.url = img;
            h.title = title.isEmpty() ? articleValue : title;
            h.source = sourceName(Poizon);
            h.verified = match && normalize(articleValue).size() >= 5;
            addHit(hits, seen, h);
        }
    }
    for (auto it = o.begin(); it != o.end(); ++it)
        if (it.value().isObject() || it.value().isArray())
            walkPoizon(it.value(), article, hits, seen, depth + 1);
}

QString jsonBlob(const QString &html, const QRegularExpression &re)
{
    const auto m = re.match(html);
    return m.hasMatch() ? m.captured(1) : QString();
}

// --- Wildberries
const int kBasketRanges[] = {143, 287, 431, 719, 1007, 1061, 1115, 1169, 1313, 1601, 1655, 1919, 2045, 2189, 2405, 2621, 2837, 3053, 3269, 3485, 3701, 3917, 4133, 4349, 4565};

} // namespace

QString sourceName(Source source)
{
    switch (source) {
    case Poizon: return QStringLiteral("Poizon");
    case Google: return QStringLiteral("Google");
    case Wildberries: return QStringLiteral("Wildberries");
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

// ---------------------------------------------------------------- Poizon

QList<Hit> parsePoizon(const QByteArray &body, const QString &article)
{
    const QString text = QString::fromUtf8(body);
    QList<Hit> hits;
    QSet<QString> seen;

    // 1) JSON внутри страницы (Next.js __NEXT_DATA__, window.__INITIAL_STATE__, любой <script type="application/json">)
    QStringList blobs;
    static const QRegularExpression nextData(QStringLiteral("<script[^>]*id=[\"']__NEXT_DATA__[\"'][^>]*>(.*?)</script>"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression jsonScript(QStringLiteral("<script[^>]*type=[\"']application/(?:ld\\+)?json[\"'][^>]*>(.*?)</script>"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression state(QStringLiteral("window\\.__[A-Z_]+__\\s*=\\s*(\\{.*?\\})\\s*;?\\s*</script>"), QRegularExpression::DotMatchesEverythingOption);
    if (const QString b = jsonBlob(text, nextData); !b.isEmpty())
        blobs << b;
    for (auto it = jsonScript.globalMatch(text); it.hasNext();)
        blobs << it.next().captured(1);
    if (const QString b = jsonBlob(text, state); !b.isEmpty())
        blobs << b;
    if (blobs.isEmpty() && text.trimmed().startsWith(QLatin1Char('{'))) // ответ - сам JSON (API)
        blobs << text;
    for (const QString &b : std::as_const(blobs)) {
        const QJsonDocument doc = QJsonDocument::fromJson(b.toUtf8());
        if (doc.isObject())
            walkPoizon(doc.object(), article, &hits, &seen);
        else if (doc.isArray())
            walkPoizon(doc.array(), article, &hits, &seen);
    }

    // 2) og:image страницы товара - главная фотография. Проверено, если артикул есть в тексте страницы.
    static const QRegularExpression og(QStringLiteral("<meta[^>]+property=[\"']og:image[\"'][^>]+content=[\"']([^\"']+)[\"']"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ogRev(QStringLiteral("<meta[^>]+content=[\"']([^\"']+)[\"'][^>]+property=[\"']og:image[\"']"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ogTitle(QStringLiteral("<meta[^>]+property=[\"']og:title[\"'][^>]+content=[\"']([^\"']*)[\"']"), QRegularExpression::CaseInsensitiveOption);
    QString pageTitle = ogTitle.match(text).captured(1);
    for (const QRegularExpression *re : {&og, &ogRev})
        for (auto it = re->globalMatch(text); it.hasNext();) {
            Hit h;
            h.url = unescapeHtml(it.next().captured(1));
            h.title = unescapeHtml(pageTitle);
            h.source = sourceName(Poizon);
            h.verified = mentions(pageTitle, article) || mentions(h.url, article);
            if (looksLikeImage(absoluteUrl(h.url)))
                addHit(&hits, &seen, h);
        }

    // 3) Картинки с адресов Poizon (Dewu) где угодно в тексте: артикул неизвестен, поэтому не проверено.
    static const QRegularExpression cdn(QStringLiteral("(?:https?:)?//[\\w.-]*(?:poizon|dewu|hupucdn)[\\w.-]*/[^\\s\"'<>\\\\)]+?\\.(?:jpe?g|png|webp)(?:\\?[^\\s\"'<>\\\\)]*)?"), QRegularExpression::CaseInsensitiveOption);
    for (auto it = cdn.globalMatch(unescapeJs(text)); it.hasNext() && hits.size() < kMaxHits * 2;) {
        Hit h;
        h.url = it.next().captured(0);
        h.source = sourceName(Poizon);
        h.verified = mentions(h.url, article);
        addHit(&hits, &seen, h);
    }
    // Сначала проверенные
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.verified && !b.verified; });
    while (hits.size() > kMaxHits)
        hits.removeLast();
    return hits;
}

Reply searchPoizon(const QString &article, const Settings &settings, const Cancel &cancelled)
{
    Reply r;
    QStringList problems;
    // Страница поиска самого Poizon.
    {
        QUrl url(envOr("INVENTORY_POIZON_URL", QStringLiteral("https://www.poizon.com")) + QStringLiteral("/search"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("keyword"), article);
        url.setQuery(q);
        const Page p = fetch(&r, QStringLiteral("Poizon"), url, {{"Accept", "text/html,application/json;q=0.9,*/*;q=0.5"}}, cancelled);
        if (r.stopped) {
            r.error = QString::fromUtf8(kStopped);
            return r;
        }
        if (!p.error.isEmpty()) {
            problems << QStringLiteral("страница поиска: %1").arg(p.error);
        } else {
            r.hits = parsePoizon(p.body, article);
            r.trace << QStringLiteral("Poizon: найдено картинок %1 (проверенных %2)").arg(r.hits.size()).arg(std::count_if(r.hits.cbegin(), r.hits.cend(), [](const Hit &h) { return h.verified; }));
            if (std::any_of(r.hits.cbegin(), r.hits.cend(), [](const Hit &h) { return h.verified; }))
                return r;
        }
    }
    // Страницы Poizon часто собираются скриптом и без него пусты - тогда просим картинки с сайта poizon.com у Google.
    Reply viaGoogle = searchGoogle(article + QStringLiteral(" site:poizon.com"), settings, cancelled);
    r.trace << viaGoogle.trace;
    if (viaGoogle.stopped) {
        r.stopped = true;
        r.error = QString::fromUtf8(kStopped);
        return r;
    }
    for (Hit &h : viaGoogle.hits) {
        h.source = sourceName(Poizon);
        r.hits << h;
    }
    if (!viaGoogle.error.isEmpty())
        problems << QStringLiteral("через Google: %1").arg(viaGoogle.error);
    while (r.hits.size() > kMaxHits)
        r.hits.removeLast();
    if (r.hits.isEmpty() && !problems.isEmpty())
        r.error = problems.join(QStringLiteral("; "));
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

QList<Hit> parseGooglePage(const QByteArray &html, const QString &article)
{
    // В странице картинки лежат в данных скрипта массивами ["адрес", высота, ширина]: сначала миниатюра на gstatic,
    // следом оригинал. Подписей и страниц-источников без полного разбора данных нет, поэтому «проверено» только
    // если артикул встречается в самом адресе картинки.
    const QString text = unescapeJs(QString::fromUtf8(html));
    static const QRegularExpression arr(QStringLiteral("\\[\"(https?://[^\"\\\\\\s]+)\",(\\d+),(\\d+)\\]"));
    QList<Hit> hits;
    QSet<QString> seen;
    QString pendingThumb;
    for (auto it = arr.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        const QString link = m.captured(1);
        if (isGstatic(link)) {
            pendingThumb = link;
            continue;
        }
        if (link.contains(QLatin1String("google.")) || !looksLikeImage(link)) {
            pendingThumb.clear();
            continue;
        }
        Hit h;
        h.url = link;
        h.thumbUrl = pendingThumb;
        h.source = sourceName(Google);
        h.verified = mentions(link, article);
        pendingThumb.clear();
        addHit(&hits, &seen, h);
    }
    // Простая версия страницы: только миниатюры <img src="https://encrypted-tbn0.gstatic.com/images?q=tbn:...">
    if (hits.isEmpty()) {
        static const QRegularExpression img(QStringLiteral("<img[^>]+src=\"(https?://[^\"]*gstatic\\.com/images\\?[^\"]+)\""));
        for (auto it = img.globalMatch(QString::fromUtf8(html)); it.hasNext();) {
            Hit h;
            h.url = unescapeHtml(it.next().captured(1));
            h.thumbUrl = h.url;
            h.source = sourceName(Google);
            addHit(&hits, &seen, h);
        }
    }
    while (hits.size() > kMaxHits)
        hits.removeLast();
    return hits;
}

Reply searchGoogle(const QString &article, const Settings &settings, const Cancel &cancelled)
{
    Reply r;
    // Из «DD1391-100 site:poizon.com» проверять артикул нужно по первой части
    QString plain = article;
    QString site;
    static const QRegularExpression siteRe(QStringLiteral("^(.*?)\\s+site:(\\S+)$"));
    if (const auto m = siteRe.match(article); m.hasMatch()) {
        plain = m.captured(1);
        site = m.captured(2);
    }
    if (settings.hasGoogleApi()) {
        QUrl url(envOr("INVENTORY_GOOGLE_CSE_URL", QStringLiteral("https://www.googleapis.com/customsearch/v1")));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("key"), settings.googleKey.trimmed());
        q.addQueryItem(QStringLiteral("cx"), settings.googleCx.trimmed());
        q.addQueryItem(QStringLiteral("q"), QLatin1Char('"') + plain + QLatin1Char('"'));
        q.addQueryItem(QStringLiteral("searchType"), QStringLiteral("image"));
        q.addQueryItem(QStringLiteral("num"), QStringLiteral("10"));
        q.addQueryItem(QStringLiteral("safe"), QStringLiteral("off"));
        q.addQueryItem(QStringLiteral("hl"), QStringLiteral("ru"));
        if (!site.isEmpty()) {
            q.addQueryItem(QStringLiteral("siteSearch"), site);
            q.addQueryItem(QStringLiteral("siteSearchFilter"), QStringLiteral("i"));
        }
        url.setQuery(q);
        const Page p = fetch(&r, QStringLiteral("Google API"), url, {{"Accept", "application/json"}}, cancelled);
        if (r.stopped) {
            r.error = QString::fromUtf8(kStopped);
            return r;
        }
        QString apiError;
        r.hits = parseGoogleCse(p.body, plain, &apiError);
        if (!apiError.isEmpty() || !p.error.isEmpty()) {
            r.error = apiError.isEmpty() ? p.error : apiError;
            r.trace << QStringLiteral("Google API: отказ - %1").arg(r.error);
        } else {
            r.trace << QStringLiteral("Google API: найдено картинок %1 (проверенных %2)").arg(r.hits.size()).arg(std::count_if(r.hits.cbegin(), r.hits.cend(), [](const Hit &h) { return h.verified; }));
            return r;
        }
        // Ключ не подошёл или закончилась суточная квота - пробуем страницу поиска без ключа.
    }
    QUrl url(envOr("INVENTORY_GOOGLE_URL", QStringLiteral("https://www.google.com/search")));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), QLatin1Char('"') + plain + QLatin1Char('"') + (site.isEmpty() ? QString() : QStringLiteral(" site:") + site));
    q.addQueryItem(QStringLiteral("tbm"), QStringLiteral("isch"));
    q.addQueryItem(QStringLiteral("hl"), QStringLiteral("ru"));
    q.addQueryItem(QStringLiteral("gl"), QStringLiteral("ru"));
    q.addQueryItem(QStringLiteral("safe"), QStringLiteral("off"));
    url.setQuery(q);
    const Page p = fetch(&r, QStringLiteral("Google"), url, {{"Accept", "text/html"}}, cancelled);
    if (r.stopped) {
        r.error = QString::fromUtf8(kStopped);
        return r;
    }
    if (!p.error.isEmpty()) {
        QString msg = p.error;
        if (p.status == 429 || p.status == 503)
            msg += QStringLiteral(" (Google ограничил запросы без ключа - задайте ключ Google API)");
        r.error = r.error.isEmpty() ? msg : r.error + QStringLiteral("; страница поиска: ") + msg;
        return r;
    }
    r.error.clear(); // страница ответила: отказ API остаётся только в trace
    QList<Hit> pageHits = parseGooglePage(p.body, plain);
    r.trace << QStringLiteral("Google: найдено картинок %1 (проверенных %2)").arg(pageHits.size()).arg(std::count_if(pageHits.cbegin(), pageHits.cend(), [](const Hit &h) { return h.verified; }));
    if (pageHits.isEmpty() && (p.body.contains("captcha") || p.body.contains("unusual traffic") || p.body.contains("enablejs")))
        r.error = QStringLiteral("Google просит подтвердить, что вы не робот, или требует включить скрипты - задайте ключ Google API");
    r.hits = pageHits;
    return r;
}

// ---------------------------------------------------------------- Wildberries

QList<WbProduct> parseWbSearch(const QByteArray &json)
{
    QList<WbProduct> out;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    QJsonArray products = root.value(QStringLiteral("data")).toObject().value(QStringLiteral("products")).toArray();
    if (products.isEmpty())
        products = root.value(QStringLiteral("products")).toArray();
    for (const QJsonValue &v : products) {
        const QJsonObject o = v.toObject();
        WbProduct p;
        p.id = o.value(QStringLiteral("id")).toVariant().toLongLong();
        p.name = o.value(QStringLiteral("name")).toString();
        p.brand = o.value(QStringLiteral("brand")).toString();
        if (p.id > 0)
            out << p;
    }
    return out;
}

int wbBasketNumber(qint64 id)
{
    const qint64 vol = id / 100000;
    int n = 1;
    for (int limit : kBasketRanges) {
        if (vol <= limit)
            return n;
        ++n;
    }
    // дальше каждая корзина - ещё 216 томов
    return n + int((vol - kBasketRanges[sizeof(kBasketRanges) / sizeof(kBasketRanges[0]) - 1] - 1) / 216);
}

QString wbBasketBase(qint64 id)
{
    const QString nn = QStringLiteral("%1").arg(wbBasketNumber(id), 2, 10, QLatin1Char('0'));
    QString tpl = envOr("INVENTORY_WB_BASKET_URL", QStringLiteral("https://basket-{nn}.wbbasket.ru"));
    return tpl.replace(QLatin1String("{nn}"), nn);
}

QString wbImageUrl(qint64 id, int number)
{
    return QStringLiteral("%1/vol%2/part%3/%4/images/big/%5.webp").arg(wbBasketBase(id)).arg(id / 100000).arg(id / 1000).arg(id).arg(number);
}

Reply searchWildberries(const QString &article, const Cancel &cancelled)
{
    Reply r;
    QUrl url(envOr("INVENTORY_WB_SEARCH_URL", QStringLiteral("https://search.wb.ru/exactmatch/ru/common/v9/search")));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("appType"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("curr"), QStringLiteral("rub"));
    q.addQueryItem(QStringLiteral("dest"), QStringLiteral("-1257786"));
    q.addQueryItem(QStringLiteral("lang"), QStringLiteral("ru"));
    q.addQueryItem(QStringLiteral("page"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("query"), article);
    q.addQueryItem(QStringLiteral("resultset"), QStringLiteral("catalog"));
    q.addQueryItem(QStringLiteral("sort"), QStringLiteral("popular"));
    q.addQueryItem(QStringLiteral("spp"), QStringLiteral("30"));
    url.setQuery(q);
    const Page p = fetch(&r, QStringLiteral("Wildberries"), url, {{"Accept", "application/json"}}, cancelled);
    if (r.stopped) {
        r.error = QString::fromUtf8(kStopped);
        return r;
    }
    if (!p.error.isEmpty()) {
        r.error = p.error;
        return r;
    }
    const QList<WbProduct> products = parseWbSearch(p.body);
    r.trace << QStringLiteral("Wildberries: товаров в выдаче %1").arg(products.size());
    QSet<QString> seen;
    bool anyVerified = false;
    int looked = 0;
    for (const WbProduct &prod : products) {
        if (looked++ >= 5)
            break;
        // Карточка товара: артикул продавца и название - по ним проверяем, тот ли это товар.
        const QString card = QStringLiteral("%1/vol%2/part%3/%4/info/ru/card.json").arg(wbBasketBase(prod.id)).arg(prod.id / 100000).arg(prod.id / 1000).arg(prod.id);
        const Page cp = fetch(&r, QStringLiteral("Wildberries карточка %1").arg(prod.id), QUrl(card), {{"Accept", "application/json"}}, cancelled);
        if (r.stopped) {
            r.error = QString::fromUtf8(kStopped);
            return r;
        }
        QString title = prod.brand + QLatin1Char(' ') + prod.name;
        int photoCount = 3;
        bool verified = mentions(title, article);
        if (cp.error.isEmpty()) {
            const QJsonObject c = QJsonDocument::fromJson(cp.body).object();
            const QString vendor = c.value(QStringLiteral("vendor_code")).toString();
            const QString imt = c.value(QStringLiteral("imt_name")).toString();
            if (!imt.isEmpty())
                title = prod.brand + QLatin1Char(' ') + imt;
            verified = verified || mentions(vendor, article) || mentions(imt, article) || (!vendor.isEmpty() && normalize(vendor) == normalize(article));
            const int count = c.value(QStringLiteral("media")).toObject().value(QStringLiteral("photo_count")).toInt();
            if (count > 0)
                photoCount = qMin(count, 4);
        }
        if (!verified && anyVerified)
            continue;
        if (!verified && !r.hits.isEmpty())
            continue;
        anyVerified = anyVerified || verified;
        const int photos = verified ? photoCount : 1; // у непроверенного берём только главное фото
        for (int n = 1; n <= photos; ++n) {
            Hit h;
            h.url = wbImageUrl(prod.id, n);
            h.pageUrl = QStringLiteral("https://www.wildberries.ru/catalog/%1/detail.aspx").arg(prod.id);
            h.title = title.trimmed();
            h.source = sourceName(Wildberries);
            h.verified = verified;
            addHit(&r.hits, &seen, h);
        }
        if (verified && r.hits.size() >= 6)
            break;
    }
    // Проверенные в начало
    std::stable_sort(r.hits.begin(), r.hits.end(), [](const Hit &a, const Hit &b) { return a.verified && !b.verified; });
    r.trace << QStringLiteral("Wildberries: найдено картинок %1 (проверенных %2)").arg(r.hits.size()).arg(std::count_if(r.hits.cbegin(), r.hits.cend(), [](const Hit &h) { return h.verified; }));
    return r;
}

Reply search(Source source, const QString &article, const Settings &settings, const Cancel &cancelled)
{
    switch (source) {
    case Poizon: return searchPoizon(article, settings, cancelled);
    case Google: return searchGoogle(article, settings, cancelled);
    case Wildberries: return searchWildberries(article, cancelled);
    default: return {};
    }
}

} // namespace ImageSearch

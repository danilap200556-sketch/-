#include "imagesearch.h"

#include <QDateTime>
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

// --- Pinterest: пин - объект с картой images (orig, 736x, 564x, 474x, 236x ...) и подписями.
QString jsonString(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? v.toString() : QString();
}

QString imageUrlOf(const QJsonObject &images, const char *key)
{
    return images.value(QLatin1String(key)).toObject().value(QStringLiteral("url")).toString();
}

void collectPins(const QJsonValue &v, const QString &article, QList<Hit> *hits, QSet<QString> *seen, int depth = 0)
{
    if (depth > 40 || hits->size() >= 60)
        return;
    if (v.isArray()) {
        for (const QJsonValue &x : v.toArray())
            collectPins(x, article, hits, seen, depth + 1);
        return;
    }
    if (!v.isObject())
        return;
    const QJsonObject o = v.toObject();
    const QJsonObject images = o.value(QStringLiteral("images")).toObject();
    if (!images.isEmpty()) {
        // Оригинал; нет его - самый крупный из доступных размеров по ширине в названии ключа («736x»).
        QString url = imageUrlOf(images, "orig");
        if (url.isEmpty()) {
            int best = 0;
            for (auto it = images.begin(); it != images.end(); ++it) {
                const QString u = it.value().toObject().value(QStringLiteral("url")).toString();
                const int w = it.key().section(QLatin1Char('x'), 0, 0).toInt();
                if (!u.isEmpty() && w >= best) {
                    best = w;
                    url = u;
                }
            }
        }
        QString thumb;
        for (const char *key : {"474x", "564x", "236x", "736x"}) {
            thumb = imageUrlOf(images, key);
            if (!thumb.isEmpty())
                break;
        }
        if (!url.isEmpty()) {
            Hit h;
            h.url = url;
            h.thumbUrl = thumb;
            h.title = jsonString(o, "grid_title");
            if (h.title.isEmpty())
                h.title = jsonString(o, "title");
            const QString desc = jsonString(o, "description");
            h.pageUrl = jsonString(o, "link");
            const QString id = o.value(QStringLiteral("id")).toVariant().toString();
            if (h.pageUrl.isEmpty() && !id.isEmpty())
                h.pageUrl = QStringLiteral("https://www.pinterest.com/pin/%1/").arg(id);
            h.source = sourceName(Pinterest);
            const QString text = h.title + QLatin1Char(' ') + desc + QLatin1Char(' ') + jsonString(o, "seo_title") + QLatin1Char(' ')
                                 + jsonString(o, "domain") + QLatin1Char(' ') + h.pageUrl + QLatin1Char(' ') + h.url;
            h.verified = mentions(text, article);
            if (h.title.isEmpty())
                h.title = desc.left(120);
            addHit(hits, seen, h);
        }
        return; // внутри пина больше нет пинов
    }
    for (auto it = o.begin(); it != o.end(); ++it)
        if (it.value().isObject() || it.value().isArray())
            collectPins(it.value(), article, hits, seen, depth + 1);
}

QString pinterestBase()
{
    QString base = envOr("INVENTORY_PINTEREST_URL", QStringLiteral("https://www.pinterest.com"));
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base;
}

} // namespace

QString sourceName(Source source)
{
    switch (source) {
    case Pinterest: return QStringLiteral("Pinterest");
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

// ---------------------------------------------------------------- Pinterest

QList<Hit> parsePinterestJson(const QByteArray &json, const QString &article, QString *error)
{
    QList<Hit> hits;
    QSet<QString> seen;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (doc.isObject()) {
        const QJsonObject resp = doc.object().value(QStringLiteral("resource_response")).toObject();
        const QJsonObject err = resp.value(QStringLiteral("error")).toObject();
        if (!err.isEmpty() && error)
            *error = err.value(QStringLiteral("message")).toString(QStringLiteral("Pinterest вернул ошибку"));
        collectPins(doc.object(), article, &hits, &seen);
    } else if (doc.isArray()) {
        collectPins(doc.array(), article, &hits, &seen);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.verified && !b.verified; });
    while (hits.size() > kMaxHits)
        hits.removeLast();
    return hits;
}

QList<Hit> parsePinterestHtml(const QByteArray &html, const QString &article)
{
    const QString text = QString::fromUtf8(html);
    QList<Hit> hits;
    QSet<QString> seen;
    // 1) JSON внутри страницы: __PWS_INITIAL_PROPS__ / __PWS_DATA__ / любой <script type="application/json">
    static const QRegularExpression scripts(QStringLiteral("<script[^>]*type=[\"']application/(?:ld\\+)?json[\"'][^>]*>(.*?)</script>"), QRegularExpression::DotMatchesEverythingOption);
    for (auto it = scripts.globalMatch(text); it.hasNext();) {
        const QJsonDocument doc = QJsonDocument::fromJson(it.next().captured(1).toUtf8());
        if (doc.isObject())
            collectPins(doc.object(), article, &hits, &seen);
        else if (doc.isArray())
            collectPins(doc.array(), article, &hits, &seen);
    }
    // 2) Адреса картинок i.pinimg.com где угодно в тексте: приводим к оригиналам (тот же хэш, другой размер в пути).
    if (hits.isEmpty()) {
        static const QRegularExpression pin(QStringLiteral("https://i\\.pinimg\\.com/(?:originals|\\d+x(?:\\d+)?)/([0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]{2}/[0-9a-f]+\\.(?:jpg|jpeg|png|webp))"));
        for (auto it = pin.globalMatch(unescapeJs(text)); it.hasNext() && hits.size() < kMaxHits * 2;) {
            const QString tail = it.next().captured(1);
            Hit h;
            h.url = QStringLiteral("https://i.pinimg.com/originals/") + tail;
            h.thumbUrl = QStringLiteral("https://i.pinimg.com/474x/") + tail;
            h.source = sourceName(Pinterest);
            h.verified = false; // подписи вместе с картинкой не разобрать
            addHit(&hits, &seen, h);
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.verified && !b.verified; });
    while (hits.size() > kMaxHits)
        hits.removeLast();
    return hits;
}

Reply searchPinterest(const QString &article, const Cancel &cancelled)
{
    Reply r;
    QStringList problems;
    const QString base = pinterestBase();
    const QString sourceUrl = QStringLiteral("/search/pins/?q=%1&rs=typed").arg(QString::fromLatin1(QUrl::toPercentEncoding(article)));
    const QList<QPair<QByteArray, QByteArray>> browserHeaders = {{"Accept-Language", "ru-RU,ru;q=0.9,en;q=0.5"}};

    // 1) JSON-запрос поиска - тот, что делает сам сайт при выдаче результатов.
    {
        QJsonObject options{{QStringLiteral("query"), article}, {QStringLiteral("scope"), QStringLiteral("pins")}, {QStringLiteral("rs"), QStringLiteral("typed")},
                            {QStringLiteral("page_size"), 25}, {QStringLiteral("auto_correction_disabled"), false}, {QStringLiteral("redux_normalize_feed"), true},
                            {QStringLiteral("source_url"), sourceUrl}};
        const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("options"), options}, {QStringLiteral("context"), QJsonObject()}}).toJson(QJsonDocument::Compact);
        QUrl url(base + QStringLiteral("/resource/BaseSearchResource/get/"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("source_url"), sourceUrl);
        q.addQueryItem(QStringLiteral("data"), QString::fromUtf8(data));
        q.addQueryItem(QStringLiteral("_"), QString::number(QDateTime::currentMSecsSinceEpoch()));
        url.setQuery(q);
        QList<QPair<QByteArray, QByteArray>> headers = browserHeaders;
        headers.append(qMakePair(QByteArray("Accept"), QByteArray("application/json, text/javascript, */*; q=0.01")));
        headers.append(qMakePair(QByteArray("X-Requested-With"), QByteArray("XMLHttpRequest")));
        headers.append(qMakePair(QByteArray("X-Pinterest-AppState"), QByteArray("active")));
        headers.append(qMakePair(QByteArray("X-Pinterest-Source-Url"), sourceUrl.toUtf8()));
        const Page p = fetch(&r, QStringLiteral("Pinterest"), url, headers, cancelled);
        if (r.stopped) {
            r.error = QString::fromUtf8(kStopped);
            return r;
        }
        if (!p.error.isEmpty()) {
            problems << QStringLiteral("запрос поиска: %1").arg(p.error);
        } else {
            QString apiError;
            r.hits = parsePinterestJson(p.body, article, &apiError);
            if (!apiError.isEmpty())
                problems << QStringLiteral("запрос поиска: %1").arg(apiError);
            r.trace << QStringLiteral("Pinterest (запрос поиска): найдено картинок %1 (проверенных %2)").arg(r.hits.size()).arg(std::count_if(r.hits.cbegin(), r.hits.cend(), [](const Hit &h) { return h.verified; }));
            if (!r.hits.isEmpty())
                return r;
        }
    }
    // 2) Страница поиска.
    {
        QUrl url(base + QStringLiteral("/search/pins/"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"), article);
        q.addQueryItem(QStringLiteral("rs"), QStringLiteral("typed"));
        url.setQuery(q);
        QList<QPair<QByteArray, QByteArray>> headers = browserHeaders;
        headers.append(qMakePair(QByteArray("Accept"), QByteArray("text/html,application/xhtml+xml;q=0.9,*/*;q=0.5")));
        const Page p = fetch(&r, QStringLiteral("Pinterest страница"), url, headers, cancelled);
        if (r.stopped) {
            r.error = QString::fromUtf8(kStopped);
            return r;
        }
        if (!p.error.isEmpty()) {
            QString msg = p.error;
            if (p.status == 429 || p.status == 403)
                msg += QStringLiteral(" (Pinterest ограничил запросы - повторите позже)");
            problems << QStringLiteral("страница поиска: %1").arg(msg);
        } else {
            r.hits = parsePinterestHtml(p.body, article);
            r.trace << QStringLiteral("Pinterest (страница): найдено картинок %1 (проверенных %2)").arg(r.hits.size()).arg(std::count_if(r.hits.cbegin(), r.hits.cend(), [](const Hit &h) { return h.verified; }));
            if (r.hits.isEmpty() && (p.body.contains("login") || p.body.contains("Log in")) && !p.body.contains("pinimg"))
                problems << QStringLiteral("страница поиска: без входа Pinterest не показал результаты");
        }
    }
    // Ответил хоть один запрос, пусть и пустым списком - это «ничего не нашли», а не отказ.
    const bool answered = r.trace.size() > 0 && r.trace.join(QLatin1Char('\n')).contains(QStringLiteral("найдено картинок"));
    if (r.hits.isEmpty() && !answered && !problems.isEmpty())
        r.error = problems.join(QStringLiteral("; "));
    else if (r.hits.isEmpty() && !problems.isEmpty())
        r.trace << QStringLiteral("Pinterest: ") + problems.join(QStringLiteral("; "));
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
    const QString plain = article;
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
    q.addQueryItem(QStringLiteral("q"), QLatin1Char('"') + plain + QLatin1Char('"'));
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

Reply search(Source source, const QString &article, const Settings &settings, const Cancel &cancelled)
{
    switch (source) {
    case Pinterest: return searchPinterest(article, cancelled);
    case Google: return searchGoogle(article, settings, cancelled);
    default: return {};
    }
}

} // namespace ImageSearch

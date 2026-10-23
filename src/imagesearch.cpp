#include "imagesearch.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace ImageSearch {

namespace {

constexpr int kTimeoutMs = 20000;
const char kUserAgent[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
const char kStopped[] = "остановлено";

QString bingBase()
{
    const QString v = qEnvironmentVariable("INVENTORY_BING_URL");
    return v.isEmpty() ? QStringLiteral("https://www.bing.com/images/async") : v;
}

QString ddgBase()
{
    const QString v = qEnvironmentVariable("INVENTORY_DDG_URL");
    return v.isEmpty() ? QStringLiteral("https://duckduckgo.com") : v;
}

struct Page {
    QByteArray body;
    int status = 0;
    QString error; // пусто - получили ответ 200
};

Page get(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers, const std::function<bool()> &cancelled)
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

QString unescapeHtml(QString s)
{
    s.replace(QLatin1String("&quot;"), QLatin1String("\""));
    s.replace(QLatin1String("&#39;"), QLatin1String("'"));
    s.replace(QLatin1String("&lt;"), QLatin1String("<"));
    s.replace(QLatin1String("&gt;"), QLatin1String(">"));
    s.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return s;
}

QString trimmed(QString s)
{
    s = s.trimmed();
    while (s.contains(QLatin1String("  ")))
        s.replace(QLatin1String("  "), QLatin1String(" "));
    return s;
}

} // namespace

QString cleanQuery(const QString &name, const QString &sku)
{
    QString q = trimmed(name);
    if (q.isEmpty())
        return trimmed(sku);
    // Размер в конце названия ("... 42", "... размер 42", "... EU 42", "... р. 42") только мешает искать модель.
    static const QRegularExpression size(QStringLiteral("(?:\\s+(?:размер|разм\\.?|р\\.|size|eu|eur|us))?\\s+(?:3[4-9]|4[0-9]|50)(?:[.,]5)?$"),
                                         QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    const QString without = q.remove(size).trimmed();
    return without.isEmpty() ? trimmed(name) : without;
}

QList<Hit> parseBing(const QByteArray &html)
{
    // Каждая картинка - ссылка с атрибутом m="{...}": плоский JSON с murl (оригинал), turl (миниатюра), purl (страница).
    const QString text = unescapeHtml(QString::fromUtf8(html));
    static const QRegularExpression object(QStringLiteral("\\{[^{}]*\"murl\"[^{}]*\\}"));
    QList<Hit> hits;
    QSet<QString> seen;
    for (auto it = object.globalMatch(text); it.hasNext();) {
        const QJsonObject o = QJsonDocument::fromJson(it.next().captured().toUtf8()).object();
        Hit h;
        h.url = o.value(QStringLiteral("murl")).toString();
        h.thumbUrl = o.value(QStringLiteral("turl")).toString();
        h.pageUrl = o.value(QStringLiteral("purl")).toString();
        if (h.url.isEmpty() || seen.contains(h.url))
            continue;
        seen.insert(h.url);
        hits << h;
    }
    return hits;
}

QString parseDdgToken(const QByteArray &html)
{
    static const QRegularExpression re(QStringLiteral("vqd=[\"']?([0-9]-[0-9-]+)"));
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(html));
    return m.hasMatch() ? m.captured(1) : QString();
}

QList<Hit> parseDdg(const QByteArray &json)
{
    QList<Hit> hits;
    QSet<QString> seen;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).object().value(QStringLiteral("results")).toArray()) {
        const QJsonObject o = v.toObject();
        Hit h;
        h.url = o.value(QStringLiteral("image")).toString();
        h.thumbUrl = o.value(QStringLiteral("thumbnail")).toString();
        h.pageUrl = o.value(QStringLiteral("url")).toString();
        if (h.url.isEmpty() || seen.contains(h.url))
            continue;
        seen.insert(h.url);
        hits << h;
    }
    return hits;
}

bool search(const QString &query, int limit, QList<Hit> *hits, QString *error, const std::function<bool()> &cancelled)
{
    hits->clear();
    QStringList problems;

    // --- Bing. Фильтр «фото» сужает выдачу; если с ним пусто - пробуем без него.
    for (int attempt = 0; attempt < 2; ++attempt) {
        QUrl url(bingBase());
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"), query);
        q.addQueryItem(QStringLiteral("first"), QStringLiteral("0"));
        q.addQueryItem(QStringLiteral("count"), QString::number(limit));
        q.addQueryItem(QStringLiteral("mmasync"), QStringLiteral("1"));
        q.addQueryItem(QStringLiteral("setlang"), QStringLiteral("ru"));
        if (attempt == 0)
            q.addQueryItem(QStringLiteral("qft"), QStringLiteral("+filterui:photo-photo"));
        url.setQuery(q);
        const Page page = get(url, {}, cancelled);
        if (page.error == QString::fromUtf8(kStopped)) {
            if (error)
                *error = page.error;
            return false;
        }
        if (!page.error.isEmpty()) {
            problems << QStringLiteral("Bing: %1").arg(page.error);
            break;
        }
        *hits = parseBing(page.body);
        if (!hits->isEmpty()) {
            while (hits->size() > limit)
                hits->removeLast();
            return true;
        }
    }

    // --- DuckDuckGo: сначала страница с ключом запроса (vqd), потом сам список.
    {
        QUrl url(ddgBase() + QStringLiteral("/"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"), query);
        q.addQueryItem(QStringLiteral("iax"), QStringLiteral("images"));
        q.addQueryItem(QStringLiteral("ia"), QStringLiteral("images"));
        url.setQuery(q);
        const Page first = get(url, {}, cancelled);
        if (first.error == QString::fromUtf8(kStopped)) {
            if (error)
                *error = first.error;
            return false;
        }
        const QString token = first.error.isEmpty() ? parseDdgToken(first.body) : QString();
        if (!first.error.isEmpty())
            problems << QStringLiteral("DuckDuckGo: %1").arg(first.error);
        else if (token.isEmpty())
            problems << QStringLiteral("DuckDuckGo: нет ключа запроса");
        else {
            QUrl list(ddgBase() + QStringLiteral("/i.js"));
            QUrlQuery lq;
            lq.addQueryItem(QStringLiteral("l"), QStringLiteral("ru-ru"));
            lq.addQueryItem(QStringLiteral("o"), QStringLiteral("json"));
            lq.addQueryItem(QStringLiteral("q"), query);
            lq.addQueryItem(QStringLiteral("vqd"), token);
            lq.addQueryItem(QStringLiteral("f"), QStringLiteral(",,,,,"));
            lq.addQueryItem(QStringLiteral("p"), QStringLiteral("1"));
            list.setQuery(lq);
            const Page second = get(list, {{"Referer", "https://duckduckgo.com/"}, {"Accept", "application/json, text/javascript, */*"}}, cancelled);
            if (second.error == QString::fromUtf8(kStopped)) {
                if (error)
                    *error = second.error;
                return false;
            }
            if (!second.error.isEmpty()) {
                problems << QStringLiteral("DuckDuckGo: %1").arg(second.error);
            } else {
                *hits = parseDdg(second.body);
                while (hits->size() > limit)
                    hits->removeLast();
                return true; // ответил - даже если пусто, это «ничего не нашли»
            }
        }
    }

    // Bing не дал ничего (но ответил) и DuckDuckGo не ответил: считаем, что искали и не нашли.
    if (problems.size() == 1 && problems.first().startsWith(QLatin1String("DuckDuckGo")))
        return true;
    if (error)
        *error = problems.join(QStringLiteral("; "));
    return false;
}

} // namespace ImageSearch

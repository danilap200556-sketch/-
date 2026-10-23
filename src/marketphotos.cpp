#include "marketphotos.h"

#include "marketapi.h"
#include "photostore.h"

#include <QCryptographicHash>
#include <QEventLoop>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVector>

namespace MarketPhotos {

namespace {

constexpr int kParallel = 6;               // одновременных скачиваний
constexpr int kBatchProducts = 10;         // товаров за раз (фото одного пакета держатся в памяти)
constexpr int kTimeoutMs = 30000;
constexpr qint64 kMaxBytes = 20 * 1024 * 1024;

struct Item {
    int id = 0;
    QString sku;
    QString offerId;   // как товар называется на Маркете
    int photos = 0;    // фото уже в базе
    QStringList links;
};

struct Downloaded {
    QByteArray data;
    QString error;
    bool stopped = false; // до этой ссылки не дошли или остановили скачивание
};

QString httpError(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() == QNetworkReply::TimeoutError)
        return QStringLiteral("не ответил за %1 с").arg(kTimeoutMs / 1000);
    if (reply->error() == QNetworkReply::OperationCanceledError)
        return QStringLiteral("файл больше %1 МБ").arg(kMaxBytes / (1024 * 1024));
    return status ? QStringLiteral("HTTP %1").arg(status) : reply->errorString();
}

// Скачивает все ссылки (до kParallel одновременно). cancelled проверяется несколько раз в секунду.
QVector<Downloaded> downloadAll(const QList<QUrl> &urls, const std::function<bool()> &cancelled)
{
    QVector<Downloaded> out(urls.size());
    if (urls.isEmpty())
        return out;
    QNetworkAccessManager nam;
    QEventLoop loop;
    QList<QNetworkReply *> active;
    int next = 0, finished = 0;
    bool stop = false;

    std::function<void()> pump = [&]() {
        while (!stop && static_cast<int>(active.size()) < kParallel && next < urls.size()) {
            const int index = next++;
            QNetworkRequest req(urls[index]);
            req.setTransferTimeout(kTimeoutMs);
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            req.setRawHeader("Accept", "image/*,*/*;q=0.5");
            req.setRawHeader("User-Agent", "Mozilla/5.0 (compatible; InventoryManager)");
            QNetworkReply *reply = nam.get(req);
            active << reply;
            QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 got, qint64 total) {
                if (got > kMaxBytes || total > kMaxBytes)
                    reply->abort();
            });
            QObject::connect(reply, &QNetworkReply::finished, &loop, [&, reply, index]() {
                active.removeOne(reply);
                ++finished;
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (reply->error() == QNetworkReply::NoError && status < 400)
                    out[index].data = reply->readAll();
                else if (stop && reply->error() == QNetworkReply::OperationCanceledError)
                    out[index].stopped = true;
                else
                    out[index].error = httpError(reply);
                reply->deleteLater();
                pump();
                if (active.isEmpty() && (stop || next >= urls.size()))
                    loop.quit();
            });
        }
    };

    QTimer watch;
    watch.setInterval(150);
    bool polling = false; // cancelled() может обрабатывать события - не заходим сюда повторно
    QObject::connect(&watch, &QTimer::timeout, &loop, [&]() {
        if (stop || polling || !cancelled)
            return;
        polling = true;
        const bool asked = cancelled();
        polling = false;
        if (!asked)
            return;
        stop = true;
        const QList<QNetworkReply *> running = active;
        for (QNetworkReply *r : running)
            r->abort();
        if (active.isEmpty())
            loop.quit();
    });
    watch.start();
    pump();
    if (!active.isEmpty())
        loop.exec();
    for (Downloaded &d : out)
        if (d.data.isEmpty() && d.error.isEmpty())
            d.stopped = true;
    return out;
}

QStringList uniqueLinks(const QStringList &links)
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &l : links) {
        const QString u = usableLink(l);
        if (u.isEmpty() || seen.contains(u))
            continue;
        seen.insert(u);
        result << u;
    }
    return result;
}

void addProblem(Result *r, const QString &text)
{
    if (r->problems.size() < kMaxProblems)
        r->problems << text;
}

} // namespace

QString usableLink(const QString &link)
{
    QString s = link.trimmed();
    if (s.startsWith(QLatin1String("//")))
        s.prepend(QStringLiteral("https:"));
    const QUrl url(s, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty())
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return {};
    if (url.path().contains(QLatin1String("/pub/photo/")))
        return {};
    return s;
}

Result run(const Options &options, const Progress &progress)
{
    Result res;
    auto report = [&](int done, int total, const QString &text) {
        if (progress && !progress(done, total, text))
            res.cancelled = true;
        return !res.cancelled;
    };

    // --- Какие товары нужны
    const QSet<int> wanted(options.productIds.begin(), options.productIds.end());
    QList<Item> items;
    QHash<int, int> indexById;
    QHash<QString, int> indexByOffer; // артикул на Маркете (строчными) -> позиция в items
    {
        QSqlQuery q("SELECT p.id, p.sku, COALESCE(NULLIF(TRIM(p.market_sku), ''), p.sku), "
                    "(SELECT COUNT(*) FROM product_photos f WHERE f.product_id = p.id) FROM products p ORDER BY p.sku");
        while (q.next()) {
            Item it;
            it.id = q.value(0).toInt();
            if (!wanted.isEmpty() && !wanted.contains(it.id))
                continue;
            it.sku = q.value(1).toString();
            it.offerId = q.value(2).toString();
            it.photos = q.value(3).toInt();
            if (options.onlyWithoutPhotos && it.photos > 0)
                continue;
            if (it.photos >= PhotoStore::kMaxPhotosPerProduct)
                continue;
            indexById.insert(it.id, items.size());
            indexByOffer.insert(it.offerId.toLower(), items.size());
            items.append(it);
        }
    }
    res.considered = items.size();
    if (items.isEmpty())
        return res;

    // --- Ссылки с Маркета (в первую очередь: это то, что там показано сейчас)
    if (options.fromMarket) {
        QStringList offerIds;
        for (const Item &it : std::as_const(items))
            offerIds << it.offerId;
        offerIds.removeDuplicates();
        const QList<MarketAccount> accounts = MarketAccount::loadAll();
        int n = 0;
        for (const MarketAccount &acc : accounts) {
            ++n;
            if (acc.apiKey.isEmpty() || acc.businessId <= 0)
                continue;
            if (!report(0, 0, QStringLiteral("Спрашиваю у Маркета (%1, %2 из %3)…").arg(acc.name).arg(n).arg(accounts.size())))
                return res;
            MarketApi api(acc.apiKey);
            QHash<QString, QStringList> pictures;
            QString err;
            if (!api.offerPictures(acc, offerIds, &pictures, &err)) {
                res.marketErrors << QStringLiteral("%1: %2").arg(acc.name, err);
                continue;
            }
            for (auto it = pictures.cbegin(); it != pictures.cend(); ++it) {
                const int pos = indexByOffer.value(it.key().toLower(), -1);
                if (pos >= 0)
                    items[pos].links << it.value();
            }
        }
    }

    // --- Ссылки из карточек Маркета в программе
    if (options.fromCards) {
        QSqlQuery q("SELECT product_id, offer_id, pictures FROM market_cards WHERE pictures <> '' ORDER BY updated_at DESC, id");
        while (q.next()) {
            int pos = -1;
            if (!q.value(0).isNull())
                pos = indexById.value(q.value(0).toInt(), -1);
            if (pos < 0)
                pos = indexByOffer.value(q.value(1).toString().toLower(), -1);
            if (pos >= 0)
                items[pos].links << q.value(2).toString().split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        }
    }

    QList<int> work; // позиции в items, для которых есть что скачивать
    for (int i = 0; i < items.size(); ++i) {
        items[i].links = uniqueLinks(items[i].links);
        const int room = PhotoStore::kMaxPhotosPerProduct - items[i].photos;
        while (items[i].links.size() > room)
            items[i].links.removeLast();
        if (items[i].links.isEmpty())
            res.noLinks << items[i].sku;
        else
            work << i;
    }
    res.withLinks = work.size();
    if (work.isEmpty())
        return res;

    // --- Скачивание и сохранение пакетами товаров
    int done = 0;
    for (int start = 0; start < work.size(); start += kBatchProducts) {
        const QList<int> batch = work.mid(start, kBatchProducts);
        QList<QUrl> urls;
        QList<QPair<int, int>> owner; // (позиция в items, номер ссылки)
        for (int pos : batch)
            for (int k = 0; k < items[pos].links.size(); ++k) {
                urls << QUrl(items[pos].links[k]);
                owner << qMakePair(pos, k);
            }
        const QString text = QStringLiteral("Скачиваю фото: товар %1 из %2, добавлено фото: %3").arg(done + 1).arg(work.size()).arg(res.added);
        if (!report(done, work.size(), text))
            return res;
        // Нажали «Остановить» во время скачивания: то, что успело скачаться, всё равно сохраняем.
        const QVector<Downloaded> got = downloadAll(urls, [&]() { return !report(done, work.size(), text); });

        int g = 0;
        for (int pos : batch) {
            Item &it = items[pos];
            PhotoStore::Changes changes;
            QSet<QByteArray> seen;
            for (int k = 0; k < it.links.size(); ++k, ++g) {
                const Downloaded &d = got[g];
                if (d.stopped)
                    continue;
                if (!d.error.isEmpty()) {
                    ++res.failed;
                    addProblem(&res, QStringLiteral("%1: %2 - %3").arg(it.sku, it.links[k], d.error));
                    continue;
                }
                PhotoStore::Prepared p;
                QString err;
                if (!PhotoStore::prepare(d.data, it.links[k], &p, &err)) {
                    ++res.failed;
                    addProblem(&res, QStringLiteral("%1: %2 - %3").arg(it.sku, it.links[k], err));
                    continue;
                }
                const QByteArray hash = QCryptographicHash::hash(p.data, QCryptographicHash::Md5);
                if (seen.contains(hash) || (it.photos > 0 && PhotoStore::exists(it.id, p))) {
                    ++res.duplicates;
                    continue;
                }
                seen.insert(hash);
                changes.added << p;
            }
            if (!changes.added.isEmpty()) {
                QString err;
                if (PhotoStore::apply(it.id, changes, &err)) {
                    res.added += changes.added.size();
                    ++res.productsFilled;
                } else {
                    res.failed += changes.added.size();
                    addProblem(&res, QStringLiteral("%1: не удалось сохранить фото - %2").arg(it.sku, err));
                }
            }
            ++done;
        }
        if (res.cancelled)
            return res;
    }
    report(work.size(), work.size(), QStringLiteral("Готово"));
    return res;
}

} // namespace MarketPhotos

#include "photofetch.h"

#include "imagesearch.h"
#include "marketapi.h"
#include "photostore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QPainter>
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

#include <algorithm>

namespace PhotoFetch {

namespace {

constexpr int kParallel = 6;               // одновременных скачиваний
constexpr int kBatchProducts = 10;         // товаров за раз (фото одного пакета держатся в памяти)
constexpr int kTimeoutMs = 30000;
constexpr qint64 kMaxBytes = 20 * 1024 * 1024;

struct Item {
    int id = 0;
    QString sku;
    QString name;
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
            // Часть сайтов не отдаёт картинки без Referer своего же адреса (защита от «чужих» ссылок).
            req.setRawHeader("Referer", (urls[index].adjusted(QUrl::RemoveUserInfo | QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toString() + QLatin1Char('/')).toUtf8());
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

namespace {

constexpr int kWebHits = 12;         // сколько результатов просим у поисковика
constexpr int kWebTries = 9;         // сколько из них пробуем скачать
constexpr int kWebChunk = 3;         // скачиваем по три, и если среди них уже есть хороший снимок - дальше не идём
constexpr int kMinSide = 300;        // меньше - слишком мелко для карточки
constexpr int kMinThumbSide = 200;   // для запасных миниатюр поисковика
constexpr double kLightBackground = 0.6; // доля светлых пикселей по краю, начиная с которой снимок считается «на белом фоне»
constexpr int kBlockedAfter = 4;     // столько запросов подряд без ответа поисковиков - считаем, что нас ограничили

struct Scored {
    PhotoStore::Prepared photo;
    double light = 0; // доля светлых пикселей по краю картинки
    bool thumb = false;
    bool good() const { return !thumb && light >= kLightBackground; }
};

// Подходит ли картинка для карточки (не слишком мелкая, не баннер) и насколько светлый у неё фон по краям.
bool analyse(const QByteArray &data, int minSide, double *light)
{
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || qMin(size.width(), size.height()) < minSide)
        return false;
    const double ratio = double(size.width()) / size.height();
    if (ratio < 0.5 || ratio > 2.0)
        return false;
    if (size.width() > 800 || size.height() > 800)
        reader.setScaledSize(size.scaled(800, 800, Qt::KeepAspectRatio));
    const QImage img = reader.read();
    if (img.isNull())
        return false;
    QImage flat(img.size(), QImage::Format_RGB32); // прозрачное - белое
    flat.fill(Qt::white);
    {
        QPainter painter(&flat);
        painter.drawImage(0, 0, img);
    }
    constexpr int kGrid = 48;
    const QImage small = flat.scaled(kGrid, kGrid, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    int ring = 0, bright = 0;
    for (int y = 0; y < kGrid; ++y)
        for (int x = 0; x < kGrid; ++x) {
            if (x >= 2 && y >= 2 && x < kGrid - 2 && y < kGrid - 2)
                continue;
            const QColor c = small.pixelColor(x, y);
            ++ring;
            bright += qMin(c.red(), qMin(c.green(), c.blue())) >= 230;
        }
    *light = double(bright) / ring;
    return true;
}

void waitMs(int ms)
{
    if (ms <= 0)
        return;
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Ищет в интернете и выбирает до `want` картинок для запроса: сначала снимки на светлом фоне в порядке выдачи, потом самые
// светлые из остальных, в последнюю очередь - миниатюры поисковика. searchError непустой, если поисковики не ответили.
QList<PhotoStore::Prepared> pickFromWeb(const QString &query, int want, QString *searchError, const std::function<bool()> &cancelled)
{
    QList<ImageSearch::Hit> hits;
    if (!ImageSearch::search(query, kWebHits, &hits, searchError, cancelled))
        return {};

    QList<Scored> found;
    QSet<QByteArray> seen;
    auto consider = [&](const QByteArray &data, const QString &source, int minSide, bool thumb) {
        double light = 0;
        if (!analyse(data, minSide, &light))
            return;
        PhotoStore::Prepared p;
        if (!PhotoStore::prepare(data, source, &p))
            return;
        const QByteArray hash = QCryptographicHash::hash(p.data, QCryptographicHash::Md5);
        if (seen.contains(hash))
            return;
        seen.insert(hash);
        found.append({p, light, thumb});
    };
    auto goodCount = [&]() { return int(std::count_if(found.cbegin(), found.cend(), [](const Scored &s) { return s.good(); })); };

    int tried = 0;
    for (int start = 0; start < hits.size() && tried < kWebTries; start += kWebChunk) {
        QList<QUrl> urls;
        QStringList sources;
        for (int k = start; k < qMin(int(hits.size()), start + kWebChunk) && tried < kWebTries; ++k, ++tried) {
            const QString link = usableLink(hits[k].url);
            if (link.isEmpty())
                continue;
            urls << QUrl(link);
            sources << link;
        }
        const QVector<Downloaded> got = downloadAll(urls, cancelled);
        for (int i = 0; i < got.size(); ++i)
            if (!got[i].stopped && got[i].error.isEmpty())
                consider(got[i].data, sources[i], kMinSide, false);
        if (goodCount() >= want || (cancelled && cancelled()))
            break;
    }
    if (found.size() < want && !(cancelled && cancelled())) { // оригиналы не скачались - берём миниатюры поисковика
        QList<QUrl> urls;
        QStringList sources;
        for (int k = 0; k < qMin(int(hits.size()), kWebChunk); ++k) {
            const QString link = usableLink(hits[k].thumbUrl);
            if (!link.isEmpty()) {
                urls << QUrl(link);
                sources << link;
            }
        }
        const QVector<Downloaded> got = downloadAll(urls, cancelled);
        for (int i = 0; i < got.size(); ++i)
            if (!got[i].stopped && got[i].error.isEmpty())
                consider(got[i].data, sources[i], kMinThumbSide, true);
    }

    std::stable_sort(found.begin(), found.end(), [](const Scored &a, const Scored &b) {
        if (a.thumb != b.thumb)
            return !a.thumb;
        if (a.good() != b.good())
            return a.good();
        return !a.good() && a.light > b.light; // светлых хватает - порядок выдачи; нет - сначала самые светлые
    });
    QList<PhotoStore::Prepared> result;
    for (const Scored &s : std::as_const(found)) {
        if (result.size() >= want)
            break;
        result << s.photo;
    }
    return result;
}

} // namespace

Result run(const Options &options, const Progress &progress)
{
    Result res;
    auto report = [&](int done, int total, const QString &text) {
        if (progress && !progress(done, total, text))
            res.cancelled = true;
        return !res.cancelled;
    };
    const int perProduct = qBound(1, options.perProduct, PhotoStore::kMaxPhotosPerProduct);

    // --- Какие товары нужны
    const QSet<int> wanted(options.productIds.begin(), options.productIds.end());
    QList<Item> items;
    QHash<int, int> indexById;
    QHash<QString, int> indexByOffer; // артикул на Маркете (строчными) -> позиция в items
    {
        QSqlQuery q("SELECT p.id, p.sku, p.name, COALESCE(NULLIF(TRIM(p.market_sku), ''), p.sku), "
                    "(SELECT COUNT(*) FROM product_photos f WHERE f.product_id = p.id) FROM products p ORDER BY p.sku");
        while (q.next()) {
            Item it;
            it.id = q.value(0).toInt();
            if (!wanted.isEmpty() && !wanted.contains(it.id))
                continue;
            it.sku = q.value(1).toString();
            it.name = q.value(2).toString();
            it.offerId = q.value(3).toString();
            it.photos = q.value(4).toInt();
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

    QList<int> work;    // позиции в items, для которых есть что скачивать с Маркета
    QList<int> webWork; // ...и те, у кого ссылок нет - их ищем в интернете
    QStringList noLinks;
    for (int i = 0; i < items.size(); ++i) {
        items[i].links = uniqueLinks(items[i].links);
        // Запас в пару ссылок: часть может не скачаться. Если у товара уже есть фото, часть новых окажется повторами.
        const int take = qMin(PhotoStore::kMaxPhotosPerProduct - items[i].photos, perProduct + items[i].photos + 2);
        while (items[i].links.size() > take)
            items[i].links.removeLast();
        if (!items[i].links.isEmpty())
            work << i;
        else if (options.fromWeb)
            webWork << i;
        else
            noLinks << items[i].sku;
    }
    res.withLinks = work.size();
    res.noLinks = noLinks;
    int total = work.size() + webWork.size();
    if (total == 0)
        return res;

    // Сохраняет найденные фото товара одной транзакцией; повторы того, что уже есть у товара, пропускает.
    auto store = [&](Item &it, const QList<PhotoStore::Prepared> &photos) -> int {
        PhotoStore::Changes changes;
        QSet<QByteArray> seen;
        for (const PhotoStore::Prepared &p : photos) {
            if (changes.added.size() >= perProduct)
                break;
            const QByteArray hash = QCryptographicHash::hash(p.data, QCryptographicHash::Md5);
            if (seen.contains(hash) || (it.photos > 0 && PhotoStore::exists(it.id, p))) {
                ++res.duplicates;
                continue;
            }
            seen.insert(hash);
            changes.added << p;
        }
        if (changes.added.isEmpty())
            return 0;
        QString err;
        if (!PhotoStore::apply(it.id, changes, &err)) {
            res.failed += changes.added.size();
            addProblem(&res, QStringLiteral("%1: не удалось сохранить фото - %2").arg(it.sku, err));
            return 0;
        }
        res.added += changes.added.size();
        ++res.productsFilled;
        return changes.added.size();
    };

    // --- Скачивание с Маркета и сохранение пакетами товаров
    int done = 0;
    for (int start = 0; start < work.size(); start += kBatchProducts) {
        const QList<int> batch = work.mid(start, kBatchProducts);
        QList<QUrl> urls;
        for (int pos : batch)
            for (const QString &link : std::as_const(items[pos].links))
                urls << QUrl(link);
        const QString text = QStringLiteral("Скачиваю фото: товар %1 из %2, добавлено фото: %3").arg(done + 1).arg(total).arg(res.added);
        if (!report(done, total, text))
            return res;
        // Нажали «Остановить» во время скачивания: то, что успело скачаться, всё равно сохраняем.
        const QVector<Downloaded> got = downloadAll(urls, [&]() { return !report(done, total, text); });

        int g = 0;
        for (int pos : batch) {
            Item &it = items[pos];
            QList<PhotoStore::Prepared> ready;
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
                ready << p;
            }
            // Ссылки на Маркете были, но ни одна не скачалась - ищем в интернете, как и тем, у кого ссылок нет.
            if (store(it, ready) == 0 && ready.isEmpty() && options.fromWeb && !res.cancelled) {
                webWork << pos;
                ++total;
            }
            ++done;
        }
        if (res.cancelled)
            return res;
    }

    // --- Поиск в интернете по названию для товаров, которых нигде нет
    if (!webWork.isEmpty()) {
        const int pauseMs = qEnvironmentVariableIsSet("INVENTORY_WEB_PAUSE_MS") ? qEnvironmentVariableIntValue("INVENTORY_WEB_PAUSE_MS") : 500;
        QHash<QString, QList<PhotoStore::Prepared>> byQuery; // товары с одним названием (размеры) ищем один раз
        int failedInRow = 0;
        bool searchedBefore = false;
        for (int w = 0; w < webWork.size(); ++w) {
            Item &it = items[webWork[w]];
            const QString text = QStringLiteral("Ищу фото в интернете: товар %1 из %2, найдено: %3").arg(w + 1).arg(webWork.size()).arg(res.fromWeb);
            if (!report(done, total, text))
                return res;
            const QString query = ImageSearch::cleanQuery(it.name, it.sku);
            QList<PhotoStore::Prepared> photos;
            if (byQuery.contains(query)) {
                photos = byQuery.value(query);
            } else if (!query.isEmpty()) {
                if (searchedBefore)
                    waitMs(pauseMs); // не частим: поисковики ограничивают запросы
                searchedBefore = true;
                QString err;
                photos = pickFromWeb(query, perProduct, &err, [&]() { return !report(done, total, text); });
                if (res.cancelled)
                    return res;
                if (!err.isEmpty()) {
                    if (res.webErrors.size() < 5 && !res.webErrors.contains(err))
                        res.webErrors << err;
                    res.noLinks << it.sku;
                    if (++failedInRow >= kBlockedAfter) {
                        res.webBlocked = true;
                        for (int rest = w + 1; rest < webWork.size(); ++rest)
                            res.noLinks << items[webWork[rest]].sku;
                        break;
                    }
                    ++done;
                    continue;
                }
                failedInRow = 0;
                byQuery.insert(query, photos);
            }
            if (store(it, photos) > 0) {
                ++res.fromWeb;
                res.sources << QStringLiteral("%1 ← %2").arg(it.sku, photos.first().sourceName);
            } else if (photos.isEmpty()) {
                res.noLinks << it.sku;
            }
            ++done;
        }
    }
    report(total, total, QStringLiteral("Готово"));
    return res;
}

} // namespace PhotoFetch

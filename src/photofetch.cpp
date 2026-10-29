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

constexpr int kTriesPerSource = 6;   // сколько результатов одного источника пробуем скачать
constexpr int kWebChunk = 3;         // скачиваем по три, и если среди них уже хватает хороших снимков - дальше не идём
constexpr int kMinSide = 300;        // меньше - слишком мелко для карточки
constexpr int kMinThumbSide = 200;   // для запасных миниатюр
constexpr double kLightBackground = 0.6; // доля светлых пикселей по краю, начиная с которой снимок считается «на белом фоне»
constexpr int kBlockedAfter = 4;     // столько товаров подряд, для которых ни один источник не ответил - считаем, что нас ограничили

struct Scored {
    PhotoStore::Prepared photo;
    ImageSearch::Hit hit;
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

QByteArray md5(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5);
}

struct WebFound {
    QList<Candidate> candidates;   // лучшие первыми
    bool answered = false;         // хоть один источник ответил (пусть и пустым списком)
    QStringList errors;            // что ответили источники, которые не ответили
};

// Ищет фото товара по артикулу во всех включённых источниках и выбирает до `want` снимков: проверенные раньше
// непроверенных, снимки на светлом фоне раньше остальных, в последнюю очередь - миниатюры.
// Что искать у товара: артикул на Маркете (если задан), затем обычный; у каждого сначала вариант без размера.
QStringList searchArticles(const QString &sku, const QString &offerId)
{
    QStringList articles;
    for (const QString &base : {offerId, sku})
        for (const QString &v : ImageSearch::articleVariants(base)) {
            bool dup = false;
            for (const QString &a : std::as_const(articles))
                dup = dup || a.compare(v, Qt::CaseInsensitive) == 0;
            if (!dup)
                articles << v;
        }
    while (articles.size() > 3)
        articles.removeLast();
    return articles;
}

WebFound pickFromWeb(const QStringList &articles, const Options &options, int want, const std::function<bool()> &cancelled)
{
    WebFound out;
    if (articles.isEmpty())
        return out;

    QList<Scored> found;
    QSet<QByteArray> seen;
    auto consider = [&](const QByteArray &data, const ImageSearch::Hit &hit, const QString &link, int minSide, bool thumb) {
        double light = 0;
        if (!analyse(data, minSide, &light))
            return;
        PhotoStore::Prepared p;
        if (!PhotoStore::prepare(data, link, &p))
            return;
        const QByteArray hash = md5(p.data);
        if (seen.contains(hash))
            return;
        seen.insert(hash);
        found.append({p, hit, light, thumb});
    };
    auto goodVerified = [&]() {
        int n = 0;
        for (const Scored &s : std::as_const(found))
            n += s.good() && s.hit.verified;
        return n;
    };
    auto stopped = [&]() { return cancelled && cancelled(); };

    QList<ImageSearch::Hit> allHits;
    for (const ImageSearch::Source src : {ImageSearch::Pinterest, ImageSearch::Google}) {
        if (!(options.webSources & src))
            continue;
        QList<ImageSearch::Hit> hits;
        QSet<QString> urls;
        bool answered = false;
        for (const QString &article : std::as_const(articles)) {
            const ImageSearch::Reply reply = ImageSearch::search(src, article, options.google, cancelled);
            if (reply.stopped || stopped())
                return out;
            if (!reply.error.isEmpty())
                out.errors << QStringLiteral("%1: %2").arg(ImageSearch::sourceName(src), reply.error);
            else
                answered = true;
            bool anyVerified = false;
            for (const ImageSearch::Hit &h : reply.hits) {
                anyVerified = anyVerified || h.verified;
                if (!urls.contains(h.url)) {
                    urls.insert(h.url);
                    hits << h;
                }
            }
            if (anyVerified)
                break; // нашлось по этому варианту артикула - другие варианты не нужны
        }
        out.answered = out.answered || answered;
        std::stable_sort(hits.begin(), hits.end(), [](const ImageSearch::Hit &a, const ImageSearch::Hit &b) { return a.verified && !b.verified; });
        allHits << hits;

        int tried = 0;
        for (int start = 0; start < hits.size() && tried < kTriesPerSource; start += kWebChunk) {
            QList<QUrl> links;
            QList<ImageSearch::Hit> chunk;
            for (int k = start; k < qMin(int(hits.size()), start + kWebChunk) && tried < kTriesPerSource; ++k, ++tried) {
                const QString link = usableLink(hits[k].url);
                if (link.isEmpty())
                    continue;
                links << QUrl(link);
                chunk << hits[k];
            }
            const QVector<Downloaded> got = downloadAll(links, cancelled);
            for (int i = 0; i < got.size(); ++i)
                if (!got[i].stopped && got[i].error.isEmpty())
                    consider(got[i].data, chunk[i], links[i].toString(), kMinSide, false);
            if (goodVerified() >= want || stopped())
                break;
        }
        if (goodVerified() >= want || stopped())
            break;
    }
    if (stopped())
        return out;

    // Ни один оригинал не скачался - берём миниатюры проверенных результатов (или первых, если проверенных нет).
    if (found.isEmpty() && !allHits.isEmpty()) {
        QList<QUrl> links;
        QList<ImageSearch::Hit> chunk;
        for (const ImageSearch::Hit &h : std::as_const(allHits)) {
            if (links.size() >= kWebChunk)
                break;
            const QString link = usableLink(h.thumbUrl);
            if (!link.isEmpty()) {
                links << QUrl(link);
                chunk << h;
            }
        }
        const QVector<Downloaded> got = downloadAll(links, cancelled);
        for (int i = 0; i < got.size(); ++i)
            if (!got[i].stopped && got[i].error.isEmpty())
                consider(got[i].data, chunk[i], links[i].toString(), kMinThumbSide, true);
    }

    std::stable_sort(found.begin(), found.end(), [](const Scored &a, const Scored &b) {
        if (a.hit.verified != b.hit.verified)
            return a.hit.verified;
        if (a.thumb != b.thumb)
            return !a.thumb;
        if (a.good() != b.good())
            return a.good();
        return !a.good() && a.light > b.light; // светлых хватает - порядок выдачи; нет - сначала самые светлые
    });
    for (const Scored &s : std::as_const(found)) {
        Candidate c;
        c.photo = s.photo;
        c.source = s.hit.source;
        c.origin = s.hit.url;
        c.title = s.hit.title;
        c.verified = s.hit.verified;
        out.candidates << c;
    }
    return out;
}

} // namespace

namespace {

// Общий ход работы для run() и collect(): собирает товары и ссылки, скачивает, и каждому товару отдаёт найденное
// снимки в deliver. deliver возвращает, сколько снимков принято (0 - ничего не принято).
using Deliver = std::function<int(Item &item, const QList<Candidate> &candidates, Result &res)>;

Result process(const Options &options, const Progress &progress, bool preview, const Deliver &deliver)
{
    Result res;
    auto report = [&](int done, int total, const QString &text) {
        if (progress && !progress(done, total, text))
            res.cancelled = true;
        return !res.cancelled;
    };
    const int perProduct = qBound(1, options.perProduct, PhotoStore::kMaxPhotosPerProduct);
    const bool onlyEmpty = options.onlyWithoutPhotos && !options.replaceExisting;

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
            if (onlyEmpty && it.photos > 0)
                continue;
            if (it.photos >= PhotoStore::kMaxPhotosPerProduct && !options.replaceExisting)
                continue;
            indexById.insert(it.id, items.size());
            indexByOffer.insert(it.offerId.toLower(), items.size());
            items.append(it);
        }
    }
    res.considered = items.size();
    if (items.isEmpty())
        return res;
    if (preview && items.size() > kMaxProposals) { // для просмотра слишком много: берём первые, остальные - в следующий раз
        res.skippedForPreview = items.size() - kMaxProposals;
        while (items.size() > kMaxProposals)
            items.removeLast();
        indexById.clear();
        indexByOffer.clear();
        for (int i = 0; i < items.size(); ++i) {
            indexById.insert(items[i].id, i);
            indexByOffer.insert(items[i].offerId.toLower(), i);
        }
    }

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
    const bool web = options.fromWeb && (options.webSources & ImageSearch::AllSources);
    for (int i = 0; i < items.size(); ++i) {
        items[i].links = uniqueLinks(items[i].links);
        // Запас в пару ссылок: часть может не скачаться. Если у товара уже есть фото, часть новых окажется повторами.
        const int take = options.replaceExisting ? qMin(PhotoStore::kMaxPhotosPerProduct, perProduct + 2)
                                                 : qMin(PhotoStore::kMaxPhotosPerProduct - items[i].photos, perProduct + items[i].photos + 2);
        while (items[i].links.size() > take)
            items[i].links.removeLast();
        if (!items[i].links.isEmpty())
            work << i;
        else if (web)
            webWork << i;
        else
            noLinks << items[i].sku;
    }
    res.withLinks = work.size();
    res.noLinks = noLinks;
    int total = work.size() + webWork.size();
    if (total == 0)
        return res;

    // --- Скачивание с Маркета и передача пакетами товаров
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
            QList<Candidate> ready;
            for (int k = 0; k < it.links.size(); ++k, ++g) {
                const Downloaded &d = got[g];
                if (d.stopped)
                    continue;
                if (!d.error.isEmpty()) {
                    ++res.failed;
                    addProblem(&res, QStringLiteral("%1: %2 - %3").arg(it.sku, it.links[k], d.error));
                    continue;
                }
                Candidate c;
                QString err;
                if (!PhotoStore::prepare(d.data, it.links[k], &c.photo, &err)) {
                    ++res.failed;
                    addProblem(&res, QStringLiteral("%1: %2 - %3").arg(it.sku, it.links[k], err));
                    continue;
                }
                c.source = QStringLiteral("Яндекс Маркет");
                c.origin = it.links[k];
                c.verified = true;
                ready << c;
            }
            // Ссылки на Маркете были, но ни одна не скачалась - ищем в интернете, как и тем, у кого ссылок нет.
            if (deliver(it, ready, res) == 0 && ready.isEmpty() && web && !res.cancelled) {
                webWork << pos;
                ++total;
            }
            ++done;
        }
        if (res.cancelled)
            return res;
    }

    // --- Поиск в интернете по артикулу для товаров, которых нигде нет
    if (!webWork.isEmpty()) {
        const int pauseMs = qEnvironmentVariableIsSet("INVENTORY_WEB_PAUSE_MS") ? qEnvironmentVariableIntValue("INVENTORY_WEB_PAUSE_MS") : 500;
        QHash<QString, WebFound> byArticle; // найденное по артикулу без размера
        int failedInRow = 0;
        bool searchedBefore = false;
        const int want = preview ? qMax(perProduct, 4) : perProduct;
        for (int w = 0; w < webWork.size(); ++w) {
            Item &it = items[webWork[w]];
            const QString text = QStringLiteral("Ищу фото в интернете по артикулу: товар %1 из %2, найдено: %3").arg(w + 1).arg(webWork.size()).arg(res.fromWeb);
            if (!report(done, total, text))
                return res;
            const QStringList articles = searchArticles(it.sku, it.offerId);
            // Размеры одной модели («DD1391-100-42», «DD1391-100-43») ищутся один раз: ключ - артикул без размера.
            const QString key = articles.isEmpty() ? QString() : ImageSearch::normalize(articles.first());
            WebFound found;
            if (!key.isEmpty() && byArticle.contains(key)) {
                found = byArticle.value(key);
            } else if (!articles.isEmpty()) {
                if (searchedBefore)
                    waitMs(pauseMs); // не частим: источники ограничивают запросы
                searchedBefore = true;
                found = pickFromWeb(articles, options, want, [&]() { return !report(done, total, text); });
                if (res.cancelled)
                    return res;
                for (const QString &e : std::as_const(found.errors))
                    if (res.webErrors.size() < 8 && !res.webErrors.contains(e))
                        res.webErrors << e;
                if (!found.answered && found.candidates.isEmpty()) {
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
                byArticle.insert(key, found);
            }
            if (deliver(it, found.candidates, res) > 0)
                ++res.fromWeb;
            else if (found.candidates.isEmpty())
                res.noLinks << it.sku;
            ++done;
        }
    }
    report(total, total, QStringLiteral("Готово"));
    return res;
}

} // namespace

Result run(const Options &options, const Progress &progress)
{
    const int perProduct = qBound(1, options.perProduct, PhotoStore::kMaxPhotosPerProduct);
    // Сохраняет найденные фото товара одной транзакцией; повторы того, что уже есть у товара, пропускает.
    const Deliver store = [&](Item &it, const QList<Candidate> &cands, Result &res) -> int {
        PhotoStore::Changes changes;
        QStringList origins;
        QSet<QByteArray> seen;
        bool sawUnverified = false;
        for (const Candidate &c : cands) {
            if (changes.added.size() >= perProduct)
                break;
            if (!c.verified && !options.acceptUnverified) {
                sawUnverified = true;
                continue;
            }
            const QByteArray hash = md5(c.photo.data);
            if (seen.contains(hash) || (!options.replaceExisting && it.photos > 0 && PhotoStore::exists(it.id, c.photo))) {
                ++res.duplicates;
                continue;
            }
            seen.insert(hash);
            changes.added << c.photo;
            origins << QStringLiteral("%1, %2").arg(c.source, c.origin);
        }
        if (changes.added.isEmpty()) {
            if (sawUnverified)
                res.unverified << it.sku;
            return 0;
        }
        if (options.replaceExisting) {
            changes.removed = PhotoStore::ids(it.id);
            changes.coverAddedIndex = 0;
        }
        QString err;
        if (!PhotoStore::apply(it.id, changes, &err)) {
            res.failed += changes.added.size();
            addProblem(&res, QStringLiteral("%1: не удалось сохранить фото - %2").arg(it.sku, err));
            return 0;
        }
        res.replaced += changes.removed.size();
        res.added += changes.added.size();
        ++res.productsFilled;
        if (!cands.isEmpty() && cands.first().source != QLatin1String("Яндекс Маркет"))
            res.sources << QStringLiteral("%1 ← %2").arg(it.sku, origins.first());
        return changes.added.size();
    };
    return process(options, progress, false, store);
}

Result collect(const Options &options, QList<Proposal> *proposals, const Progress &progress)
{
    proposals->clear();
    const Deliver gather = [&](Item &it, const QList<Candidate> &cands, Result &res) -> int {
        Q_UNUSED(res)
        if (cands.isEmpty())
            return 0;
        Proposal p;
        p.productId = it.id;
        p.sku = it.sku;
        p.name = it.name;
        p.existingPhotos = it.photos;
        p.candidates = cands;
        proposals->append(p);
        return cands.size();
    };
    return process(options, progress, true, gather);
}

Result save(const QList<Choice> &choices, bool replaceExisting)
{
    Result res;
    res.considered = choices.size();
    for (const Choice &ch : choices) {
        if (ch.chosen.isEmpty())
            continue;
        PhotoStore::Changes changes;
        QSet<QByteArray> seen;
        const int existing = PhotoStore::count(ch.productId);
        for (const Candidate &c : ch.chosen) {
            const QByteArray hash = md5(c.photo.data);
            if (seen.contains(hash) || (!replaceExisting && existing > 0 && PhotoStore::exists(ch.productId, c.photo))) {
                ++res.duplicates;
                continue;
            }
            seen.insert(hash);
            if (!replaceExisting && existing + changes.added.size() >= PhotoStore::kMaxPhotosPerProduct) {
                addProblem(&res, QStringLiteral("%1: уже %2 фото, лишнее не добавлено").arg(ch.sku).arg(PhotoStore::kMaxPhotosPerProduct));
                continue;
            }
            changes.added << c.photo;
        }
        if (changes.added.isEmpty())
            continue;
        if (replaceExisting) {
            changes.removed = PhotoStore::ids(ch.productId);
            changes.coverAddedIndex = 0;
        }
        QString err;
        if (!PhotoStore::apply(ch.productId, changes, &err)) {
            res.failed += changes.added.size();
            addProblem(&res, QStringLiteral("%1: не удалось сохранить фото - %2").arg(ch.sku, err));
            continue;
        }
        res.replaced += changes.removed.size();
        res.added += changes.added.size();
        ++res.productsFilled;
    }
    return res;
}

} // namespace PhotoFetch

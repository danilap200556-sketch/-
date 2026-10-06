#include "photostore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace PhotoStore {

namespace {

QByteArray encodeJpeg(const QImage &img, int quality)
{
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", quality);
    return bytes;
}

} // namespace

bool prepare(const QString &path, Prepared *out, QString *error)
{
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead())
        return fail(QStringLiteral("не удалось открыть как картинку (%1)").arg(reader.errorString()));
    const QSize original = reader.size();
    if (original.isValid() && (original.width() > kFullSize || original.height() > kFullSize))
        reader.setScaledSize(original.scaled(kFullSize, kFullSize, Qt::KeepAspectRatio));
    QImage img = reader.read();
    if (img.isNull())
        return fail(reader.errorString());

    // JPEG не хранит прозрачность - подкладываем белый фон.
    QImage flat(img.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    {
        QPainter p(&flat);
        p.drawImage(0, 0, img);
    }
    if (flat.width() > kFullSize || flat.height() > kFullSize)
        flat = flat.scaled(kFullSize, kFullSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    out->data = encodeJpeg(flat, 82);
    out->thumb = encodeJpeg(flat.scaled(kThumbSize, kThumbSize, Qt::KeepAspectRatio, Qt::SmoothTransformation), 78);
    out->sourceName = QFileInfo(path).fileName();
    if (out->data.isEmpty() || out->thumb.isEmpty())
        return fail(QStringLiteral("не удалось сохранить картинку в JPEG (нет поддержки JPEG в Qt?)"));
    return true;
}

QList<int> ids(int productId)
{
    QList<int> result;
    QSqlQuery q;
    q.prepare("SELECT id FROM product_photos WHERE product_id = ? ORDER BY position, id");
    q.addBindValue(productId);
    if (q.exec())
        while (q.next())
            result << q.value(0).toInt();
    return result;
}

int count(int productId)
{
    QSqlQuery q;
    q.prepare("SELECT COUNT(*) FROM product_photos WHERE product_id = ?");
    q.addBindValue(productId);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

QByteArray fullData(int photoId)
{
    QSqlQuery q;
    q.prepare("SELECT data FROM product_photos WHERE id = ?");
    q.addBindValue(photoId);
    return q.exec() && q.next() ? q.value(0).toByteArray() : QByteArray();
}

QByteArray thumbData(int photoId)
{
    QSqlQuery q;
    q.prepare("SELECT thumb FROM product_photos WHERE id = ?");
    q.addBindValue(photoId);
    return q.exec() && q.next() ? q.value(0).toByteArray() : QByteArray();
}

int add(int productId, const Prepared &p, QString *error)
{
    if (count(productId) >= kMaxPhotosPerProduct) {
        if (error)
            *error = QStringLiteral("у товара уже %1 фото - это максимум").arg(kMaxPhotosPerProduct);
        return -1;
    }
    QSqlQuery q;
    q.prepare("INSERT INTO product_photos (product_id, position, data, thumb) "
              "VALUES (?, COALESCE((SELECT MAX(position) FROM product_photos WHERE product_id = ?), 0) + 1, ?, ?) "
              "RETURNING id");
    q.addBindValue(productId);
    q.addBindValue(productId);
    q.addBindValue(p.data);
    q.addBindValue(p.thumb);
    if (!q.exec() || !q.next()) {
        if (error)
            *error = q.lastError().text();
        return -1;
    }
    PhotoCache::invalidate(productId);
    return q.value(0).toInt();
}

bool exists(int productId, const Prepared &p)
{
    QSqlQuery q;
    q.prepare("SELECT 1 FROM product_photos WHERE product_id = ? AND md5(data) = ? LIMIT 1");
    q.addBindValue(productId);
    q.addBindValue(QString::fromLatin1(QCryptographicHash::hash(p.data, QCryptographicHash::Md5).toHex()));
    return q.exec() && q.next();
}

bool remove(int photoId)
{
    QSqlQuery q;
    q.prepare("DELETE FROM product_photos WHERE id = ? RETURNING product_id");
    q.addBindValue(photoId);
    if (!q.exec())
        return false;
    if (q.next())
        PhotoCache::invalidate(q.value(0).toInt());
    return true;
}

bool makeCover(int photoId)
{
    QSqlQuery q;
    q.prepare("UPDATE product_photos SET position = "
              "(SELECT COALESCE(MIN(p2.position), 0) - 1 FROM product_photos p2 "
              " WHERE p2.product_id = product_photos.product_id) "
              "WHERE id = ? RETURNING product_id");
    q.addBindValue(photoId);
    if (!q.exec())
        return false;
    if (q.next())
        PhotoCache::invalidate(q.value(0).toInt());
    return true;
}

bool apply(int productId, const Changes &changes, QString *error)
{
    if (changes.isEmpty())
        return true;
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    auto rollback = [&](const QString &msg) {
        db.rollback();
        if (error)
            *error = msg;
        PhotoCache::invalidate(productId);
        return false;
    };
    for (int id : changes.removed) {
        QSqlQuery q;
        q.prepare("DELETE FROM product_photos WHERE id = ? AND product_id = ?");
        q.addBindValue(id);
        q.addBindValue(productId);
        if (!q.exec())
            return rollback(q.lastError().text());
    }
    int coverId = changes.coverExistingId;
    for (int i = 0; i < changes.added.size(); ++i) {
        if (changes.added[i].isNull())
            continue; // добавили и тут же убрали в диалоге
        QString err;
        const int id = add(productId, changes.added[i], &err);
        if (id < 0)
            return rollback(QStringLiteral("%1: %2").arg(changes.added[i].sourceName, err));
        if (i == changes.coverAddedIndex)
            coverId = id;
    }
    if (coverId > 0 && !makeCover(coverId))
        return rollback(QStringLiteral("не удалось сделать фото главным"));
    if (!db.commit())
        return rollback(db.lastError().text());
    PhotoCache::invalidate(productId);
    return true;
}

SkuIndex::SkuIndex()
{
    // Порядок важен: более надёжные ключи перезаписывают менее надёжные.
    QSqlQuery codes("SELECT barcode, product_id FROM product_barcodes");
    while (codes.next())
        m_byKey.insert(codes.value(0).toString().toLower(), codes.value(1).toInt());
    QSqlQuery products("SELECT id, sku, COALESCE(NULLIF(TRIM(market_sku), ''), '') FROM products");
    QList<QPair<int, QString>> skus;
    while (products.next()) {
        const int id = products.value(0).toInt();
        const QString sku = products.value(1).toString();
        m_skuById.insert(id, sku);
        const QString market = products.value(2).toString().toLower();
        if (!market.isEmpty())
            m_byKey.insert(market, id);
        skus.append({id, sku.toLower()});
    }
    for (const auto &s : skus)
        m_byKey.insert(s.second, s.first);
}

SkuIndex::Hit SkuIndex::match(const QString &fileName) const
{
    Hit hit;
    const QString base = QFileInfo(fileName).completeBaseName().trimmed().toLower();
    if (base.isEmpty())
        return hit;
    auto found = [&](const QString &key, int order) {
        const auto it = m_byKey.constFind(key);
        if (it == m_byKey.constEnd())
            return false;
        hit.productId = it.value();
        hit.sku = m_skuById.value(hit.productId);
        hit.order = order;
        return true;
    };
    if (found(base, 0))
        return hit;
    // Номер фото в конце имени: "_2", "-2", " 2", ".2", " (2)".
    static const QRegularExpression suffix(QStringLiteral("^(.+?)(?:[\\s_\\-.]+(\\d{1,2})|\\s*\\((\\d{1,2})\\))$"));
    const auto m = suffix.match(base);
    if (m.hasMatch()) {
        const QString n = m.captured(2).isEmpty() ? m.captured(3) : m.captured(2);
        found(m.captured(1).trimmed(), n.toInt());
    }
    return hit;
}

} // namespace PhotoStore

// ---------------------------------------------------------------------------

namespace PhotoCache {

namespace {

// Запись есть всегда после prefetch: null-пиксмап = у товара нет фото.
QHash<int, QPixmap> &cache()
{
    static QHash<int, QPixmap> c;
    return c;
}
QHash<int, QByteArray> &coverCache()
{
    static QHash<int, QByteArray> c;
    return c;
}

QPixmap squareIcon(const QByteArray &jpeg)
{
    QImage img;
    if (!img.loadFromData(jpeg))
        return QPixmap();
    QImage square(kIconSize, kIconSize, QImage::Format_RGB32);
    square.fill(Qt::white);
    const QImage scaled = img.scaled(kIconSize, kIconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter p(&square);
    p.drawImage((kIconSize - scaled.width()) / 2, (kIconSize - scaled.height()) / 2, scaled);
    return QPixmap::fromImage(square);
}

} // namespace

QPixmap placeholder()
{
    static QPixmap pm;
    if (pm.isNull()) {
        QImage img(kIconSize, kIconSize, QImage::Format_RGB32);
        img.fill(QColor(0xee, 0xee, 0xee));
        QPainter p(&img);
        p.setPen(QColor(0x99, 0x99, 0x99));
        QFont f = p.font();
        f.setPixelSize(10);
        p.setFont(f);
        p.drawRect(0, 0, kIconSize - 1, kIconSize - 1);
        p.drawText(QRect(0, 0, kIconSize, kIconSize), Qt::AlignCenter, QStringLiteral("нет\nфото"));
        pm = QPixmap::fromImage(img);
    }
    return pm;
}

QPixmap thumb(int productId)
{
    const auto it = cache().constFind(productId);
    return it != cache().constEnd() && !it.value().isNull() ? it.value() : placeholder();
}

bool hasPhoto(int productId)
{
    const auto it = cache().constFind(productId);
    return it != cache().constEnd() && !it.value().isNull();
}

void prefetch(const QList<int> &productIds)
{
    QList<int> missing;
    for (int id : productIds)
        if (!cache().contains(id))
            missing << id;
    constexpr int kChunk = 150;
    for (int start = 0; start < missing.size(); start += kChunk) {
        const QList<int> chunk = missing.mid(start, kChunk);
        QStringList marks;
        for (int i = 0; i < chunk.size(); ++i)
            marks << QStringLiteral("?");
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT DISTINCT ON (product_id) product_id, thumb FROM product_photos "
                                 "WHERE product_id IN (%1) ORDER BY product_id, position, id")
                      .arg(marks.join(',')));
        for (int id : chunk)
            q.addBindValue(id);
        for (int id : chunk)
            cache().insert(id, QPixmap());
        if (q.exec())
            while (q.next())
                cache().insert(q.value(0).toInt(), squareIcon(q.value(1).toByteArray()));
    }
}

void invalidate(int productId)
{
    cache().remove(productId);
    coverCache().remove(productId);
}

void clear()
{
    cache().clear();
    coverCache().clear();
}

QByteArray coverJpeg(int productId)
{
    const auto it = coverCache().constFind(productId);
    if (it != coverCache().constEnd())
        return it.value();
    QByteArray bytes;
    QSqlQuery q;
    q.prepare("SELECT data FROM product_photos WHERE product_id = ? ORDER BY position, id LIMIT 1");
    q.addBindValue(productId);
    if (q.exec() && q.next())
        bytes = q.value(0).toByteArray();
    if (coverCache().size() > 40)
        coverCache().clear();
    coverCache().insert(productId, bytes);
    return bytes;
}

} // namespace PhotoCache

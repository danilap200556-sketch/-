#include "marketcategories.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

#include <algorithm>

namespace {

constexpr qint64 kMaxAgeSeconds = 14LL * 24 * 3600;

QString treePath() { return MarketCategories::cacheDir() + QStringLiteral("/categories.json"); }
QString paramsPath(qint64 id) { return MarketCategories::cacheDir() + QStringLiteral("/params_%1.json").arg(id); }

bool fresh(const QString &path)
{
    const QFileInfo fi(path);
    return fi.exists() && fi.lastModified().secsTo(QDateTime::currentDateTime()) < kMaxAgeSeconds;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(data);
}

bool pathRec(const QJsonObject &node, qint64 id, QStringList *trail)
{
    trail->append(node.value("name").toString());
    if (node.value("id").toInteger() == id)
        return true;
    for (const QJsonValue &c : node.value("children").toArray())
        if (pathRec(c.toObject(), id, trail))
            return true;
    trail->removeLast();
    return false;
}

const QJsonObject *find(const QJsonObject &node, qint64 id, QJsonObject *holder)
{
    if (node.value("id").toInteger() == id) {
        *holder = node;
        return holder;
    }
    for (const QJsonValue &c : node.value("children").toArray())
        if (find(c.toObject(), id, holder))
            return holder;
    return nullptr;
}

QList<MarketCategoryParam> parseParams(const QJsonArray &arr)
{
    QList<MarketCategoryParam> list;
    for (const QJsonValue &v : arr)
        list.append(MarketCategoryParam::fromJson(v.toObject()));
    // Обязательные первыми, внутри - как отдал Маркет.
    std::stable_sort(list.begin(), list.end(), [](const MarketCategoryParam &a, const MarketCategoryParam &b) {
        return a.required && !b.required;
    });
    return list;
}

} // namespace

MarketCategoryParam MarketCategoryParam::fromJson(const QJsonObject &o)
{
    MarketCategoryParam p;
    p.id = o.value("id").toInteger();
    p.name = o.value("name").toString();
    p.type = o.value("type").toString();
    p.description = o.value("description").toString();
    p.required = o.value("required").toBool();
    p.multivalue = o.value("multivalue").toBool();
    p.allowCustomValues = o.value("allowCustomValues").toBool();
    p.distinctive = o.value("distinctive").toBool();
    for (const QJsonValue &v : o.value("values").toArray()) {
        const QJsonObject vo = v.toObject();
        p.values.append({vo.value("id").toInteger(), vo.value("value").toString()});
    }
    const QJsonObject unit = o.value("unit").toObject();
    p.defaultUnitId = unit.value("defaultUnitId").toInteger();
    for (const QJsonValue &v : unit.value("units").toArray()) {
        const QJsonObject uo = v.toObject();
        p.units.append({uo.value("id").toInteger(), uo.value("name").toString()});
    }
    const QJsonObject c = o.value("constraints").toObject();
    p.hasMin = c.contains("minValue");
    p.hasMax = c.contains("maxValue");
    p.minValue = c.value("minValue").toDouble();
    p.maxValue = c.value("maxValue").toDouble();
    p.maxLength = c.value("maxLength").toInt();
    return p;
}

QString MarketCategories::cacheDir()
{
    const QByteArray env = qgetenv("INVENTORY_CACHE_DIR");
    if (!env.isEmpty())
        return QString::fromUtf8(env);
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    return base + QStringLiteral("/market-cache");
}

bool MarketCategories::loadTree(MarketApi &api, bool refresh, QJsonObject *root, QString *error)
{
    if (!refresh && fresh(treePath())) {
        const QJsonDocument doc = QJsonDocument::fromJson(readFile(treePath()));
        if (doc.isObject() && !doc.object().isEmpty()) {
            *root = doc.object();
            return true;
        }
    }
    if (!api.categoriesTree(root, error))
        return false;
    writeFile(treePath(), QJsonDocument(*root).toJson(QJsonDocument::Compact));
    return true;
}

QString MarketCategories::pathOf(const QJsonObject &root, qint64 id)
{
    QStringList trail;
    if (id <= 0 || !pathRec(root, id, &trail))
        return QString();
    // Корень («Все товары») в пути не нужен.
    if (trail.size() > 1)
        trail.removeFirst();
    return trail.join(QStringLiteral(" › "));
}

bool MarketCategories::isLeaf(const QJsonObject &root, qint64 id)
{
    QJsonObject holder;
    return find(root, id, &holder) && holder.value("children").toArray().isEmpty();
}

bool MarketCategories::cachedParams(qint64 categoryId, QList<MarketCategoryParam> *out)
{
    if (!fresh(paramsPath(categoryId)))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(readFile(paramsPath(categoryId)));
    if (!doc.isArray())
        return false;
    *out = parseParams(doc.array());
    return true;
}

bool MarketCategories::loadParams(MarketApi &api, qint64 businessId, qint64 categoryId, bool refresh,
                                  QList<MarketCategoryParam> *out, QString *error)
{
    if (!refresh && cachedParams(categoryId, out))
        return true;
    QJsonArray arr;
    if (!api.categoryParameters(categoryId, businessId, &arr, error))
        return false;
    writeFile(paramsPath(categoryId), QJsonDocument(arr).toJson(QJsonDocument::Compact));
    *out = parseParams(arr);
    return true;
}

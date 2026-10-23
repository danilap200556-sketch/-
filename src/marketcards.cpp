#include "marketcards.h"
#include "barcode.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace MarketCards {

namespace {

const char kSelect[] =
    "SELECT id, account_id, COALESCE(product_id, 0), offer_id, name, description, vendor, vendor_code, "
    "COALESCE(category_id, 0), category_name, barcodes, pictures, videos, countries, "
    "length_cm::float8, width_cm::float8, height_cm::float8, weight_kg::float8, commodity_code, tags, price::float8, adult, "
    "params, status, market_status, COALESCE(market_sku, 0), last_result, sent_at, updated_at FROM market_cards ";

MarketCard readCard(const QSqlQuery &q)
{
    MarketCard c;
    int i = 0;
    c.id = q.value(i++).toInt();
    c.accountId = q.value(i++).toInt();
    c.productId = q.value(i++).toInt();
    c.offerId = q.value(i++).toString();
    c.name = q.value(i++).toString();
    c.description = q.value(i++).toString();
    c.vendor = q.value(i++).toString();
    c.vendorCode = q.value(i++).toString();
    c.categoryId = q.value(i++).toLongLong();
    c.categoryName = q.value(i++).toString();
    c.barcodes = splitList(q.value(i++).toString());
    c.pictures = splitLines(q.value(i++).toString());
    c.videos = splitLines(q.value(i++).toString());
    c.countries = splitList(q.value(i++).toString());
    c.lengthCm = q.value(i++).toDouble();
    c.widthCm = q.value(i++).toDouble();
    c.heightCm = q.value(i++).toDouble();
    c.weightKg = q.value(i++).toDouble();
    c.commodityCode = q.value(i++).toString();
    c.tags = splitList(q.value(i++).toString());
    c.price = q.value(i++).toDouble();
    c.adult = q.value(i++).toBool();
    c.params = paramsFromJson(q.value(i++).toString());
    c.status = q.value(i++).toString();
    c.marketStatus = q.value(i++).toString();
    c.marketSku = q.value(i++).toLongLong();
    c.lastResult = q.value(i++).toString();
    c.sentAt = q.value(i++).toDateTime();
    c.updatedAt = q.value(i++).toDateTime();
    return c;
}

// Пустая, но не «нулевая» строка: нулевая QString уходит в базу как NULL, а столбцы карточки NOT NULL.
QString nn(const QString &s) { return s.isNull() ? QStringLiteral("") : s; }

void bindCard(QSqlQuery &q, const MarketCard &c)
{
    q.addBindValue(c.accountId);
    q.addBindValue(c.productId > 0 ? QVariant(c.productId) : QVariant(QMetaType(QMetaType::Int)));
    q.addBindValue(c.offerId.trimmed());
    q.addBindValue(nn(c.name));
    q.addBindValue(nn(c.description));
    q.addBindValue(nn(c.vendor));
    q.addBindValue(nn(c.vendorCode));
    q.addBindValue(c.categoryId > 0 ? QVariant(c.categoryId) : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(nn(c.categoryName));
    q.addBindValue(nn(c.barcodes.join(QStringLiteral(";"))));
    q.addBindValue(nn(c.pictures.join(QLatin1Char('\n'))));
    q.addBindValue(nn(c.videos.join(QLatin1Char('\n'))));
    q.addBindValue(nn(c.countries.join(QStringLiteral(";"))));
    q.addBindValue(c.lengthCm);
    q.addBindValue(c.widthCm);
    q.addBindValue(c.heightCm);
    q.addBindValue(c.weightKg);
    q.addBindValue(nn(c.commodityCode));
    q.addBindValue(nn(c.tags.join(QStringLiteral(";"))));
    q.addBindValue(c.price);
    q.addBindValue(c.adult);
    q.addBindValue(nn(paramsToJson(c.params)));
    q.addBindValue(nn(c.status));
    q.addBindValue(nn(c.marketStatus));
    q.addBindValue(c.marketSku > 0 ? QVariant(c.marketSku) : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(nn(c.lastResult));
    q.addBindValue(c.sentAt.isValid() ? QVariant(c.sentAt) : QVariant(QMetaType(QMetaType::QDateTime)));
}

} // namespace

QStringList splitLines(const QString &text)
{
    QStringList out;
    for (const QString &l : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        const QString t = l.trimmed();
        if (!t.isEmpty() && !out.contains(t))
            out << t;
    }
    return out;
}

QStringList splitList(const QString &text)
{
    QStringList out;
    for (const QString &l : text.split(QRegularExpression(QStringLiteral("[;\\r\\n]+")), Qt::SkipEmptyParts)) {
        const QString t = l.trimmed();
        if (!t.isEmpty() && !out.contains(t))
            out << t;
    }
    return out;
}

QString paramsToJson(const QMap<qint64, QList<MarketParamValue>> &params)
{
    QJsonObject o;
    for (auto it = params.cbegin(); it != params.cend(); ++it) {
        QJsonArray arr;
        for (const MarketParamValue &v : it.value()) {
            QJsonObject vo;
            vo.insert("value", v.value);
            if (v.valueId > 0)
                vo.insert("valueId", v.valueId);
            if (v.unitId > 0)
                vo.insert("unitId", v.unitId);
            arr.append(vo);
        }
        if (!arr.isEmpty())
            o.insert(QString::number(it.key()), arr);
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QMap<qint64, QList<MarketParamValue>> paramsFromJson(const QString &json)
{
    QMap<qint64, QList<MarketParamValue>> params;
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        QList<MarketParamValue> list;
        for (const QJsonValue &v : it.value().toArray()) {
            const QJsonObject vo = v.toObject();
            list.append({vo.value("value").toString(), vo.value("valueId").toInteger(), vo.value("unitId").toInteger()});
        }
        if (!list.isEmpty())
            params.insert(it.key().toLongLong(), list);
    }
    return params;
}

QList<MarketCard> loadAll(int accountId, QString *error)
{
    QList<MarketCard> list;
    QSqlQuery q;
    q.prepare(QString::fromLatin1(kSelect) + QStringLiteral("WHERE account_id = ? ORDER BY offer_id"));
    q.addBindValue(accountId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return list;
    }
    while (q.next())
        list << readCard(q);
    return list;
}

bool save(MarketCard *c, QString *error)
{
    if (c->offerId.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Укажите артикул на Маркете (offerId).");
        return false;
    }
    QSqlQuery q;
    if (c->id > 0) {
        q.prepare("UPDATE market_cards SET account_id = ?, product_id = ?, offer_id = ?, name = ?, description = ?, vendor = ?, "
                  "vendor_code = ?, category_id = ?, category_name = ?, barcodes = ?, pictures = ?, videos = ?, countries = ?, "
                  "length_cm = ?, width_cm = ?, height_cm = ?, weight_kg = ?, commodity_code = ?, tags = ?, price = ?, adult = ?, "
                  "params = ?, status = ?, market_status = ?, market_sku = ?, last_result = ?, sent_at = ?, updated_at = NOW() "
                  "WHERE id = ?");
        bindCard(q, *c);
        q.addBindValue(c->id);
    } else {
        q.prepare("INSERT INTO market_cards (account_id, product_id, offer_id, name, description, vendor, vendor_code, category_id, "
                  "category_name, barcodes, pictures, videos, countries, length_cm, width_cm, height_cm, weight_kg, commodity_code, "
                  "tags, price, adult, params, status, market_status, market_sku, last_result, sent_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id");
        bindCard(q, *c);
    }
    if (!q.exec()) {
        if (error)
            *error = q.lastError().nativeErrorCode() == QLatin1String("23505")
                         ? QStringLiteral("Карточка с артикулом «%1» в этом кабинете уже есть.").arg(c->offerId.trimmed())
                         : q.lastError().text();
        return false;
    }
    if (c->id <= 0 && q.next())
        c->id = q.value(0).toInt();
    c->updatedAt = QDateTime::currentDateTime();
    return true;
}

bool remove(const QList<int> &ids, QString *error)
{
    if (ids.isEmpty())
        return true;
    QStringList marks(ids.size(), QStringLiteral("?"));
    QSqlQuery q;
    q.prepare(QStringLiteral("DELETE FROM market_cards WHERE id IN (%1)").arg(marks.join(QLatin1Char(','))));
    for (int id : ids)
        q.addBindValue(id);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

QString guessVendor(const QString &productName)
{
    // Бренды обуви и одежды, которые чаще всего встречаются в названиях. Первое слово названия, если оно из списка.
    static const QStringList brands = {
        "Nike", "Adidas", "Puma", "Reebok", "New Balance", "Asics", "Converse", "Vans", "Jordan", "Fila", "Under Armour",
        "Skechers", "Timberland", "Dr. Martens", "Salomon", "The North Face", "Yeezy", "Balenciaga", "Gucci", "Mizuno",
        "Saucony", "Hoka", "On", "Crocs", "UGG", "Lacoste", "Hugo Boss", "Tommy Hilfiger", "Calvin Klein", "Columbia",
        "Merrell", "Ecco", "Geox", "Kappa", "Umbro", "Diadora", "Brooks", "Champion", "Levi's", "Zara", "Stone Island"};
    const QString name = productName.trimmed();
    QString best;
    for (const QString &b : brands)
        if (name.startsWith(b, Qt::CaseInsensitive) && (name.size() == b.size() || !name.at(b.size()).isLetterOrNumber()) &&
            b.size() > best.size())
            best = b;
    return best;
}

QString offerIdOf(const QString &sku, const QString &marketSku)
{
    const QString m = marketSku.trimmed();
    return m.isEmpty() ? sku.trimmed() : m;
}

QStringList publicPhotoLinks(int productId, const QString &siteUrl, QString *error)
{
    QStringList links;
    QString base = siteUrl.trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    if (base.isEmpty()) {
        if (error)
            *error = QStringLiteral("не задан адрес сайта (меню «Сайт» → «Адрес сайта…»): Маркет скачивает фото по ссылке с сайта");
        return links;
    }
    // Токен - секретная часть ссылки: без неё фото не отдаётся. Создаём тем фото, у которых его ещё нет.
    QSqlQuery up;
    up.prepare("UPDATE product_photos SET public_token = replace(gen_random_uuid()::text || gen_random_uuid()::text, '-', '') "
               "WHERE product_id = ? AND public_token IS NULL");
    up.addBindValue(productId);
    if (!up.exec()) {
        if (error)
            *error = up.lastError().text();
        return links;
    }
    QSqlQuery q;
    q.prepare("SELECT id, public_token FROM product_photos WHERE product_id = ? ORDER BY position, id");
    q.addBindValue(productId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return links;
    }
    while (q.next())
        links << QStringLiteral("%1/pub/photo/%2/%3.jpg").arg(base).arg(q.value(0).toInt()).arg(q.value(1).toString());
    return links;
}

bool fillFromProduct(MarketCard *c, int productId, int fields, const QString &siteUrl, QString *error)
{
    QSqlQuery q;
    q.prepare("SELECT sku, name, COALESCE(description, ''), price::float8, COALESCE(market_sku, '') FROM products WHERE id = ?");
    q.addBindValue(productId);
    if (!q.exec() || !q.next()) {
        if (error)
            *error = QStringLiteral("товара уже нет в базе");
        return false;
    }
    const QString sku = q.value(0).toString();
    const QString name = q.value(1).toString();
    c->productId = productId;
    if (c->offerId.trimmed().isEmpty())
        c->offerId = offerIdOf(sku, q.value(4).toString());
    if (fields & FName)
        c->name = name;
    if (fields & FDescription)
        c->description = q.value(2).toString();
    if (fields & FPrice)
        c->price = qRound(q.value(3).toDouble() * 100) / 100.0;
    if (fields & FBarcodes)
        c->barcodes = Barcode::forProduct(productId);
    if (fields & FVendor) {
        if (c->vendor.trimmed().isEmpty())
            c->vendor = guessVendor(name);
        if (c->vendorCode.trimmed().isEmpty())
            c->vendorCode = sku;
    }
    if (fields & FPhotos) {
        QString why;
        const QStringList links = publicPhotoLinks(productId, siteUrl, &why);
        if (!why.isEmpty()) {
            if (error)
                *error = why;
        } else if (!links.isEmpty()) {
            c->pictures = links;
        } else if (error) {
            *error = QStringLiteral("у товара нет фото в базе");
        }
    }
    return true;
}

bool newFromProduct(MarketCard *c, int accountId, int productId, const QString &siteUrl, QString *error)
{
    *c = MarketCard();
    c->accountId = accountId;
    return fillFromProduct(c, productId, FAll, siteUrl, error);
}

QString describeCardStatus(const QString &s)
{
    if (s == QLatin1String("HAS_CARD_CAN_UPDATE")) return QStringLiteral("есть карточка, можно править");
    if (s == QLatin1String("HAS_CARD_CAN_NOT_UPDATE")) return QStringLiteral("есть карточка, править нельзя");
    if (s == QLatin1String("HAS_CARD_CAN_UPDATE_ERRORS")) return QStringLiteral("есть карточка, в ней ошибки");
    if (s == QLatin1String("HAS_CARD_CAN_UPDATE_PROCESSING")) return QStringLiteral("есть карточка, обновляется");
    if (s == QLatin1String("NO_CARD_NEED_CONTENT")) return QStringLiteral("нет карточки, нужно дополнить данные");
    if (s == QLatin1String("NO_CARD_MARKET_WILL_CREATE")) return QStringLiteral("Маркет создаст карточку сам");
    if (s == QLatin1String("NO_CARD_ERRORS")) return QStringLiteral("нет карточки, есть ошибки");
    if (s == QLatin1String("NO_CARD_PROCESSING")) return QStringLiteral("на обработке");
    if (s == QLatin1String("NO_CARD_ADD_TO_CAMPAIGN")) return QStringLiteral("нужно добавить в магазин");
    return s;
}

void applyMarketInfo(MarketCard *c, const MarketOfferInfo &info)
{
    const QJsonObject o = info.offer;
    auto text = [&](const char *k) { return o.value(QLatin1String(k)).toString(); };
    auto list = [&](const char *k) {
        QStringList l;
        for (const QJsonValue &v : o.value(QLatin1String(k)).toArray())
            l << v.toString();
        return l;
    };
    if (!text("name").isEmpty()) c->name = text("name");
    if (!text("description").isEmpty()) c->description = text("description");
    if (!text("vendor").isEmpty()) c->vendor = text("vendor");
    if (!text("vendorCode").isEmpty()) c->vendorCode = text("vendorCode");
    if (!list("pictures").isEmpty()) c->pictures = list("pictures");
    if (!list("videos").isEmpty()) c->videos = list("videos");
    if (!list("barcodes").isEmpty()) c->barcodes = list("barcodes");
    if (!list("manufacturerCountries").isEmpty()) c->countries = list("manufacturerCountries");
    if (!list("tags").isEmpty()) c->tags = list("tags");
    if (!text("customsCommodityCode").isEmpty()) c->commodityCode = text("customsCommodityCode");
    const QJsonObject wd = o.value("weightDimensions").toObject();
    if (!wd.isEmpty()) {
        c->lengthCm = wd.value("length").toDouble();
        c->widthCm = wd.value("width").toDouble();
        c->heightCm = wd.value("height").toDouble();
        c->weightKg = wd.value("weight").toDouble();
    }
    const double price = o.value("basicPrice").toObject().value("value").toDouble();
    if (price > 0)
        c->price = price;
    if (o.contains("adult"))
        c->adult = o.value("adult").toBool();
    qint64 cat = info.marketCategoryId > 0 ? info.marketCategoryId : o.value("marketCategoryId").toInteger();
    if (cat > 0) {
        c->categoryId = cat;
        if (!info.marketCategoryName.isEmpty())
            c->categoryName = info.marketCategoryName;
    }
    if (!info.parameterValues.isEmpty()) {
        c->params.clear();
        for (const QJsonValue &v : info.parameterValues) {
            const QJsonObject po = v.toObject();
            c->params[po.value("parameterId").toInteger()].append(
                {po.value("value").toString(), po.value("valueId").toInteger(), po.value("unitId").toInteger()});
        }
    }
    c->marketSku = info.marketSku;
    c->marketStatus = describeCardStatus(info.cardStatus);
}

QJsonObject toOffer(const MarketCard &c)
{
    QJsonObject offer;
    offer.insert("offerId", c.offerId.trimmed());
    auto putText = [&](const char *k, const QString &v) {
        if (!v.trimmed().isEmpty())
            offer.insert(QLatin1String(k), v.trimmed());
    };
    auto putList = [&](const char *k, const QStringList &l) {
        if (!l.isEmpty())
            offer.insert(QLatin1String(k), QJsonArray::fromStringList(l));
    };
    putText("name", c.name);
    if (c.categoryId > 0)
        offer.insert("marketCategoryId", c.categoryId);
    putList("pictures", c.pictures);
    putList("videos", c.videos);
    putText("vendor", c.vendor);
    putText("vendorCode", c.vendorCode);
    putList("barcodes", c.barcodes);
    putText("description", c.description);
    putList("manufacturerCountries", c.countries);
    if (c.lengthCm > 0 && c.widthCm > 0 && c.heightCm > 0 && c.weightKg > 0) {
        offer.insert("weightDimensions", QJsonObject{{"length", c.lengthCm}, {"width", c.widthCm}, {"height", c.heightCm},
                                                     {"weight", c.weightKg}});
    }
    putText("customsCommodityCode", c.commodityCode);
    putList("tags", c.tags);
    offer.insert("adult", c.adult);
    if (c.price > 0)
        offer.insert("basicPrice", QJsonObject{{"value", c.price}, {"currencyId", "RUR"}});
    // Характеристики всегда уходят вместе с категорией.
    if (c.categoryId > 0 && !c.params.isEmpty()) {
        QJsonArray values;
        for (auto it = c.params.cbegin(); it != c.params.cend(); ++it) {
            for (const MarketParamValue &v : it.value()) {
                if (v.value.trimmed().isEmpty() && v.valueId <= 0)
                    continue;
                QJsonObject po;
                po.insert("parameterId", it.key());
                po.insert("value", v.value.trimmed());
                if (v.valueId > 0)
                    po.insert("valueId", v.valueId);
                if (v.unitId > 0)
                    po.insert("unitId", v.unitId);
                values.append(po);
            }
        }
        if (!values.isEmpty())
            offer.insert("parameterValues", values);
    }
    QJsonObject mapping;
    return QJsonObject{{"offer", offer}};
}

namespace {
QString issueText(const MarketOfferIssue &i, const QList<MarketCategoryParam> &params)
{
    QString where;
    if (i.parameterId > 0) {
        for (const MarketCategoryParam &p : params)
            if (p.id == i.parameterId)
                where = QStringLiteral("«%1»: ").arg(p.name);
        if (where.isEmpty())
            where = QStringLiteral("характеристика %1: ").arg(i.parameterId);
    }
    return where + i.message;
}
} // namespace

SendReport send(MarketApi &api, const MarketAccount &acc, const QList<MarketCard *> &cards,
                const QHash<qint64, QList<MarketCategoryParam>> &params, QString *error)
{
    SendReport report;
    auto finish = [&](MarketCard *c, const MarketOfferResult *res, const QString &fallbackError) {
        const auto &ps = params.value(c->categoryId);
        QStringList errs, warns;
        if (res) {
            for (const MarketOfferIssue &i : res->errors) errs << issueText(i, ps);
            for (const MarketOfferIssue &i : res->warnings) warns << issueText(i, ps);
        }
        if (!fallbackError.isEmpty())
            errs << fallbackError;
        if (errs.isEmpty()) {
            c->status = QStringLiteral("sent");
            c->sentAt = QDateTime::currentDateTime();
            c->lastResult = warns.isEmpty() ? QStringLiteral("принято Маркетом")
                                            : QStringLiteral("принято, предупреждения: ") + warns.join(QStringLiteral("; "));
            ++report.accepted;
            report.lines << QStringLiteral("%1: принято%2").arg(c->offerId, warns.isEmpty() ? QString() : QStringLiteral(" (предупреждения: %1)").arg(warns.join(QStringLiteral("; "))));
        } else {
            c->status = QStringLiteral("error");
            c->lastResult = QStringLiteral("ошибка: ") + errs.join(QStringLiteral("; "));
            ++report.rejected;
            report.lines << QStringLiteral("%1: %2").arg(c->offerId, c->lastResult);
        }
        QString saveErr;
        save(c, &saveErr);
    };

    constexpr int kBatch = 100;
    for (int start = 0; start < cards.size(); start += kBatch) {
        QList<MarketCard *> pending = cards.mid(start, kBatch);
        for (int pass = 0; pass < 2 && !pending.isEmpty(); ++pass) {
            QJsonArray arr;
            for (const MarketCard *c : std::as_const(pending))
                arr.append(toOffer(*c));
            QList<MarketOfferResult> results;
            QString err;
            const bool ok = api.updateOfferMappings(acc.businessId, arr, &results, &err);
            QHash<QString, const MarketOfferResult *> byId;
            for (const MarketOfferResult &r : std::as_const(results))
                byId.insert(r.offerId, &r);
            if (!ok && results.isEmpty()) {
                // Вся пачка не принята по общей причине (ключ, права, связь) - у каждой карточки одна и та же ошибка.
                for (MarketCard *c : std::as_const(pending))
                    finish(c, nullptr, err);
                if (error)
                    *error = err;
                pending.clear();
                break;
            }
            QList<MarketCard *> retry;
            bool anyFailed = false;
            for (MarketCard *c : std::as_const(pending)) {
                const MarketOfferResult *r = byId.value(c->offerId.trimmed(), nullptr);
                if (r && !r->errors.isEmpty()) {
                    anyFailed = true;
                    finish(c, r, QString());
                } else if (ok) {
                    finish(c, r, QString());
                } else if (pass == 0) {
                    retry << c; // без своих ошибок, но пачка отклонена из-за соседей
                } else {
                    finish(c, nullptr, err);
                }
            }
            pending = (anyFailed && pass == 0) ? retry : QList<MarketCard *>();
        }
    }
    return report;
}

QStringList missingForSend(const MarketCard &c, const QList<MarketCategoryParam> *params)
{
    QStringList miss;
    if (c.offerId.trimmed().isEmpty()) miss << QStringLiteral("артикул на Маркете");
    if (c.name.trimmed().isEmpty()) miss << QStringLiteral("название");
    if (c.categoryId <= 0) miss << QStringLiteral("категория");
    if (c.pictures.isEmpty()) miss << QStringLiteral("фото (ссылки)");
    if (c.vendor.trimmed().isEmpty()) miss << QStringLiteral("бренд");
    if (c.description.trimmed().isEmpty()) miss << QStringLiteral("описание");
    if (params) {
        for (const MarketCategoryParam &p : *params) {
            if (!p.required)
                continue;
            bool filled = false;
            for (const MarketParamValue &v : c.params.value(p.id))
                filled = filled || !v.value.trimmed().isEmpty() || v.valueId > 0;
            if (!filled)
                miss << QStringLiteral("характеристика «%1»").arg(p.name);
        }
    }
    return miss;
}

} // namespace MarketCards

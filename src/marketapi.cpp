#include "marketapi.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

constexpr int kTimeoutMs = 60000;
constexpr int kMaxPages = 2000;          // страховка от бесконечной пагинации
constexpr int kStocksBatch = 2000;       // maxItems в UpdateStocks*Request
constexpr int kPricesBatch = 500;        // maxItems в UpdateBusinessPricesRequest

QString describeHttpStatus(int status)
{
    switch (status) {
    case 400: return QStringLiteral("запрос содержит неправильные данные");
    case 401: return QStringLiteral("неверный или отозванный API-ключ");
    case 403: return QStringLiteral("у API-ключа нет доступа к этому кабинету или методу "
                                    "(проверьте доступы ключа в настройках кабинета)");
    case 404: return QStringLiteral("кабинет, магазин или склад не найден - проверьте ID");
    case 420: return QStringLiteral("превышен лимит запросов, попробуйте через минуту");
    case 423: return QStringLiteral("метод временно заблокирован для этого кабинета");
    default:
        if (status >= 500)
            return QStringLiteral("ошибка на стороне Маркета, попробуйте позже");
        return QString();
    }
}

QString errorsFromBody(const QJsonDocument &doc)
{
    QStringList parts;
    const QJsonArray errors = doc.object().value("errors").toArray();
    for (const QJsonValue &e : errors) {
        const QJsonObject o = e.toObject();
        const QString code = o.value("code").toString();
        const QString msg = o.value("message").toString();
        parts << (msg.isEmpty() ? code : QStringLiteral("%1: %2").arg(code, msg));
    }
    return parts.join("; ");
}

QString nextPageToken(const QJsonObject &container)
{
    return container.value("paging").toObject().value("nextPageToken").toString();
}

} // namespace

// ---------------------------------------------------------------------------
// MarketAccount (хранение в БД)

QList<MarketAccount> MarketAccount::loadAll()
{
    QList<MarketAccount> result;
    QSqlQuery q("SELECT id, name, api_key, business_id, campaign_id, warehouse_groups, "
                "market_warehouse_id FROM market_accounts ORDER BY name");
    while (q.next()) {
        MarketAccount a;
        a.id = q.value(0).toInt();
        a.name = q.value(1).toString();
        a.apiKey = q.value(2).toString();
        a.businessId = q.value(3).toLongLong();
        a.campaignId = q.value(4).toLongLong();
        a.warehouseGroups = q.value(5).toBool();
        a.marketWarehouseId = q.value(6).toLongLong();
        result.append(a);
    }
    QSqlQuery w("SELECT account_id, warehouse_id FROM market_account_warehouses");
    QHash<int, QList<int>> byAccount;
    while (w.next())
        byAccount[w.value(0).toInt()].append(w.value(1).toInt());
    for (auto &a : result)
        a.localWarehouseIds = byAccount.value(a.id);
    return result;
}

bool MarketAccount::save(QString *error)
{
    QSqlDatabase db = QSqlDatabase::database();
    auto fail = [&](const QSqlQuery &q) {
        if (error)
            *error = q.lastError().text();
        db.rollback();
        return false;
    };
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }

    QSqlQuery q;
    if (id == 0) {
        q.prepare("INSERT INTO market_accounts (name, api_key, business_id, campaign_id, "
                  "warehouse_groups, market_warehouse_id) VALUES (?, ?, ?, ?, ?, ?) RETURNING id");
    } else {
        q.prepare("UPDATE market_accounts SET name = ?, api_key = ?, business_id = ?, campaign_id = ?, "
                  "warehouse_groups = ?, market_warehouse_id = ? WHERE id = ?");
    }
    q.addBindValue(name);
    q.addBindValue(apiKey);
    q.addBindValue(businessId);
    q.addBindValue(campaignId);
    q.addBindValue(warehouseGroups);
    q.addBindValue(marketWarehouseId > 0 ? QVariant(marketWarehouseId) : QVariant(QMetaType(QMetaType::LongLong)));
    if (id != 0)
        q.addBindValue(id);
    if (!q.exec())
        return fail(q);
    if (id == 0) {
        if (!q.next())
            return fail(q);
        id = q.value(0).toInt();
    }

    QSqlQuery del;
    del.prepare("DELETE FROM market_account_warehouses WHERE account_id = ?");
    del.addBindValue(id);
    if (!del.exec())
        return fail(del);
    for (int whId : localWarehouseIds) {
        QSqlQuery ins;
        ins.prepare("INSERT INTO market_account_warehouses (account_id, warehouse_id) VALUES (?, ?)");
        ins.addBindValue(id);
        ins.addBindValue(whId);
        if (!ins.exec())
            return fail(ins);
    }
    if (!db.commit()) {
        if (error)
            *error = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

bool MarketAccount::remove(int id, QString *error)
{
    QSqlQuery q;
    q.prepare("DELETE FROM market_accounts WHERE id = ?");
    q.addBindValue(id);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// MarketApi

MarketApi::MarketApi(const QString &apiKey, Log log)
    : m_apiKey(apiKey.trimmed()), m_log(std::move(log)), m_nam(new QNetworkAccessManager)
{
}

MarketApi::~MarketApi()
{
    delete m_nam;
}

QString MarketApi::baseUrl()
{
    const QByteArray env = qgetenv("MARKET_API_BASE_URL");
    return env.isEmpty() ? QStringLiteral("https://api.partner.market.yandex.ru") : QString::fromUtf8(env);
}

MarketApi::Reply MarketApi::request(const QByteArray &method, const QString &path, const QUrlQuery &query,
                                    const QJsonDocument &body)
{
    QUrl url(baseUrl() + path);
    url.setQuery(query);
    QNetworkRequest req(url);
    req.setRawHeader("Api-Key", m_apiKey.toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(kTimeoutMs);

    QNetworkReply *reply;
    if (body.isNull()) {
        reply = m_nam->sendCustomRequest(req, method);
    } else {
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_nam->sendCustomRequest(req, method, body.toJson(QJsonDocument::Compact));
    }

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    Reply r;
    r.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    r.json = QJsonDocument::fromJson(data);

    if (r.httpStatus == 0) {
        r.error = QStringLiteral("нет связи с Маркетом: %1").arg(reply->errorString());
    } else if (r.httpStatus >= 400 || r.json.object().value("status").toString() == QLatin1String("ERROR")) {
        QStringList parts;
        parts << QStringLiteral("HTTP %1").arg(r.httpStatus);
        const QString human = describeHttpStatus(r.httpStatus);
        if (!human.isEmpty())
            parts << human;
        const QString details = errorsFromBody(r.json);
        if (!details.isEmpty())
            parts << details;
        else if (r.json.isNull() && !data.isEmpty())
            parts << QString::fromUtf8(data.left(300));
        r.error = parts.join(" - ");
    }
    reply->deleteLater();

    if (m_log && !r.error.isEmpty())
        m_log(QStringLiteral("%1 %2: %3").arg(QString::fromLatin1(method), path, r.error));
    return r;
}

bool MarketApi::campaigns(QList<MarketCampaign> *out, QString *error)
{
    out->clear();
    QString token;
    for (int page = 0; page < kMaxPages; ++page) {
        QUrlQuery query;
        query.addQueryItem("limit", "100");
        if (!token.isEmpty())
            query.addQueryItem("page_token", token);
        const Reply r = request("GET", "/v2/campaigns", query, QJsonDocument());
        if (!r.error.isEmpty()) {
            if (error)
                *error = r.error;
            return false;
        }
        const QJsonObject root = r.json.object();
        for (const QJsonValue &v : root.value("campaigns").toArray()) {
            const QJsonObject c = v.toObject();
            MarketCampaign mc;
            mc.id = c.value("id").toInteger();
            mc.domain = c.value("domain").toString();
            const QJsonObject b = c.value("business").toObject();
            mc.businessId = b.value("id").toInteger();
            mc.businessName = b.value("name").toString();
            mc.placementType = c.value("placementType").toString();
            mc.apiAvailability = c.value("apiAvailability").toString();
            out->append(mc);
        }
        token = nextPageToken(root);
        if (token.isEmpty())
            return true;
    }
    return true;
}

bool MarketApi::partnerWarehouses(qint64 businessId, QList<MarketWarehouse> *out, QString *error)
{
    out->clear();
    QString token;
    for (int page = 0; page < kMaxPages; ++page) {
        QUrlQuery query;
        query.addQueryItem("limit", "30");
        if (!token.isEmpty())
            query.addQueryItem("page_token", token);
        const Reply r = request("POST", QStringLiteral("/v3/businesses/%1/warehouses").arg(businessId), query,
                                QJsonDocument(QJsonObject()));
        if (!r.error.isEmpty()) {
            if (error)
                *error = r.error;
            return false;
        }
        const QJsonObject result = r.json.object().value("result").toObject();
        for (const QJsonValue &v : result.value("warehouses").toArray()) {
            const QJsonObject w = v.toObject();
            MarketWarehouse mw;
            mw.id = w.value("id").toInteger();
            mw.name = w.value("name").toString();
            for (const QJsonValue &m : w.value("models").toArray())
                mw.models << m.toObject().value("placementType").toString();
            out->append(mw);
        }
        token = nextPageToken(result);
        if (token.isEmpty())
            return true;
    }
    return true;
}

bool MarketApi::catalog(const MarketAccount &acc, QHash<QString, MarketOffer> *out, QString *error)
{
    out->clear();
    QString token;
    for (int page = 0; page < kMaxPages; ++page) {
        QUrlQuery query;
        query.addQueryItem("limit", "100");
        if (!token.isEmpty())
            query.addQueryItem("page_token", token);
        const Reply r = request("POST", QStringLiteral("/v2/businesses/%1/offer-mappings").arg(acc.businessId), query,
                                QJsonDocument(QJsonObject()));
        if (!r.error.isEmpty()) {
            if (error)
                *error = r.error;
            return false;
        }
        const QJsonObject result = r.json.object().value("result").toObject();
        for (const QJsonValue &v : result.value("offerMappings").toArray()) {
            const QJsonObject offer = v.toObject().value("offer").toObject();
            MarketOffer mo;
            mo.name = offer.value("name").toString();
            mo.archived = offer.value("archived").toBool();
            // Список магазинов приходит не всегда; если он есть - товар должен
            // быть размещён именно в магазине этого кабинета.
            const QJsonArray campaigns = offer.value("campaigns").toArray();
            if (!campaigns.isEmpty()) {
                mo.inCampaign = false;
                for (const QJsonValue &c : campaigns)
                    if (c.toObject().value("campaignId").toInteger() == acc.campaignId)
                        mo.inCampaign = true;
            }
            out->insert(offer.value("offerId").toString(), mo);
        }
        token = nextPageToken(result);
        if (token.isEmpty())
            return true;
    }
    return true;
}

namespace {

MarketStock parseStocks(const QJsonArray &stocks)
{
    MarketStock s;
    for (const QJsonValue &v : stocks) {
        const QJsonObject o = v.toObject();
        const QString type = o.value("type").toString();
        if (type == QLatin1String("FIT"))
            s.fit = o.value("count").toInteger();
        else if (type == QLatin1String("AVAILABLE"))
            s.available = o.value("count").toInteger();
    }
    return s;
}

} // namespace

bool MarketApi::stocks(const MarketAccount &acc, QHash<QString, MarketStock> *out, QString *error)
{
    out->clear();
    QString path;
    QJsonObject body;
    if (acc.warehouseGroups) {
        path = QStringLiteral("/v2/campaigns/%1/offers/stocks").arg(acc.campaignId);
    } else {
        if (acc.marketWarehouseId <= 0) {
            if (error)
                *error = QStringLiteral("в настройках кабинета не выбран склад Маркета");
            return false;
        }
        path = QStringLiteral("/v3/businesses/%1/offers/stocks").arg(acc.businessId);
        body.insert("partnerWarehouseId", acc.marketWarehouseId);
    }

    QString token;
    for (int page = 0; page < kMaxPages; ++page) {
        QUrlQuery query;
        query.addQueryItem("limit", "100");
        if (!token.isEmpty())
            query.addQueryItem("page_token", token);
        const Reply r = request("POST", path, query, QJsonDocument(body));
        if (!r.error.isEmpty()) {
            if (error)
                *error = r.error;
            return false;
        }
        const QJsonObject result = r.json.object().value("result").toObject();
        if (acc.warehouseGroups) {
            // У склада из группы остатки общие со всей группой, поэтому один и тот
            // же товар может прийти с нескольких складов с одинаковым числом -
            // складывать их нельзя, берём максимум.
            for (const QJsonValue &wv : result.value("warehouses").toArray()) {
                for (const QJsonValue &ov : wv.toObject().value("offers").toArray()) {
                    const QJsonObject o = ov.toObject();
                    const MarketStock s = parseStocks(o.value("stocks").toArray());
                    MarketStock &cur = (*out)[o.value("offerId").toString()];
                    cur.fit = qMax(cur.fit, s.fit);
                    cur.available = qMax(cur.available, s.available);
                }
            }
        } else {
            for (const QJsonValue &ov : result.value("offers").toArray()) {
                const QJsonObject o = ov.toObject();
                out->insert(o.value("offerId").toString(), parseStocks(o.value("stocks").toArray()));
            }
        }
        token = nextPageToken(result);
        if (token.isEmpty())
            return true;
    }
    return true;
}

bool MarketApi::updateStocks(const MarketAccount &acc, const QList<QPair<QString, qint64>> &items,
                             QString *error)
{
    if (!acc.warehouseGroups && acc.marketWarehouseId <= 0) {
        if (error)
            *error = QStringLiteral("в настройках кабинета не выбран склад Маркета");
        return false;
    }
    for (int start = 0; start < items.size(); start += kStocksBatch) {
        const auto batch = items.mid(start, kStocksBatch);
        QJsonArray arr;
        for (const auto &[sku, count] : batch) {
            QJsonObject o;
            o.insert("sku", sku);
            if (acc.warehouseGroups) {
                QJsonObject item;
                item.insert("count", count);
                o.insert("items", QJsonArray{item});
            } else {
                o.insert("partnerWarehouseId", acc.marketWarehouseId);
                o.insert("count", count);
            }
            arr.append(o);
        }
        Reply r;
        if (acc.warehouseGroups) {
            r = request("PUT", QStringLiteral("/v2/campaigns/%1/offers/stocks").arg(acc.campaignId), {},
                        QJsonDocument(QJsonObject{{"skus", arr}}));
        } else {
            r = request("POST", QStringLiteral("/v3/businesses/%1/offers/stocks/update").arg(acc.businessId), {},
                        QJsonDocument(QJsonObject{{"skuItems", arr}}));
        }
        if (!r.error.isEmpty()) {
            if (error)
                *error = start > 0 ? QStringLiteral("%1 (первые %2 позиций уже переданы)").arg(r.error).arg(start)
                                   : r.error;
            return false;
        }
        if (m_log)
            m_log(QStringLiteral("Передано остатков: %1 из %2").arg(start + batch.size()).arg(items.size()));
    }
    return true;
}

bool MarketApi::prices(qint64 businessId, QHash<QString, MarketPrice> *out, QString *error)
{
    out->clear();
    QString token;
    for (int page = 0; page < kMaxPages; ++page) {
        QUrlQuery query;
        query.addQueryItem("limit", "500");
        if (!token.isEmpty())
            query.addQueryItem("page_token", token);
        const Reply r = request("POST", QStringLiteral("/v2/businesses/%1/offer-prices").arg(businessId), query,
                                QJsonDocument(QJsonObject()));
        if (!r.error.isEmpty()) {
            if (error)
                *error = r.error;
            return false;
        }
        const QJsonObject result = r.json.object().value("result").toObject();
        for (const QJsonValue &v : result.value("offers").toArray()) {
            const QJsonObject o = v.toObject();
            const QJsonObject p = o.value("price").toObject();
            MarketPrice mp;
            mp.value = p.value("value").toDouble();
            mp.discountBase = p.value("discountBase").toDouble();
            mp.currencyId = p.value("currencyId").toString();
            out->insert(o.value("offerId").toString(), mp);
        }
        token = nextPageToken(result);
        if (token.isEmpty())
            return true;
    }
    return true;
}

bool MarketApi::updatePrices(qint64 businessId, const QList<MarketPriceUpdate> &items, QString *error)
{
    for (int start = 0; start < items.size(); start += kPricesBatch) {
        const auto batch = items.mid(start, kPricesBatch);
        QJsonArray arr;
        for (const auto &u : batch) {
            QJsonObject price;
            price.insert("value", u.value);
            price.insert("currencyId", "RUR");
            // Зачёркнутую цену сохраняем, только если скидка от неё остаётся
            // заметной - иначе Маркет отклонит всю пачку из-за одной позиции.
            if (u.discountBase > 0 && u.value <= u.discountBase * 0.95)
                price.insert("discountBase", u.discountBase);
            arr.append(QJsonObject{{"offerId", u.offerId}, {"price", price}});
        }
        const Reply r = request("POST", QStringLiteral("/v2/businesses/%1/offer-prices/updates").arg(businessId), {},
                                QJsonDocument(QJsonObject{{"offers", arr}}));
        if (!r.error.isEmpty()) {
            if (error)
                *error = start > 0 ? QStringLiteral("%1 (первые %2 цен уже переданы)").arg(r.error).arg(start)
                                   : r.error;
            return false;
        }
        if (m_log)
            m_log(QStringLiteral("Передано цен: %1 из %2").arg(start + batch.size()).arg(items.size()));
    }
    return true;
}

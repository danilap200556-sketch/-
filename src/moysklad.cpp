#include "moysklad.h"

#include "appsettings.h"
#include "orderwriteoff.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace MoySklad {

namespace {

const char kConfigKey[] = "moysklad_config";
const char kStoresKey[] = "moysklad_stores";
constexpr int kTimeoutMs = 60 * 1000;
constexpr int kPageSize = 1000;
constexpr int kMaxPages = 500;          // 500 000 товаров - заведомо больше любого склада
constexpr int kRetries = 4;             // при 429 («слишком много запросов») ждём и повторяем
constexpr qint64 kMaxQty = 100'000'000; // как в инвентаризации

// Для автоматических тестов ожидание между повторами можно сократить (в обычной работе не задаётся).
int retryWaitMs()
{
    const int v = qEnvironmentVariableIntValue("MOYSKLAD_RETRY_MS");
    return v > 0 ? v : 3000;
}

// Последняя часть адреса сущности: https://api.moysklad.ru/api/remap/1.2/entity/store/<id>?expand=... -> <id>.
QString idFromHref(const QString &href)
{
    QString path = href.section(QLatin1Char('?'), 0, 0);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path.section(QLatin1Char('/'), -1);
}

QString errorText(int status, const QJsonDocument &json, const QByteArray &data)
{
    QStringList parts;
    parts << QStringLiteral("HTTP %1").arg(status);
    switch (status) {
    case 401:
        parts << QStringLiteral("МойСклад не принял токен (или логин и пароль): проверьте, что токен скопирован целиком и не отозван");
        break;
    case 403:
        parts << QStringLiteral("доступ запрещён: у этого токена нет прав на склады и отчёт «Остатки»");
        break;
    case 404:
        parts << QStringLiteral("адрес не найден");
        break;
    case 429:
        parts << QStringLiteral("МойСклад просит не торопиться (слишком много запросов): повторите через минуту");
        break;
    default:
        if (status >= 500)
            parts << QStringLiteral("сервер МойСклад сейчас недоступен");
        break;
    }
    QStringList details;
    for (const QJsonValue &v : json.object().value(QStringLiteral("errors")).toArray()) {
        const QString text = v.toObject().value(QStringLiteral("error")).toString().trimmed();
        if (!text.isEmpty() && !details.contains(text))
            details << text;
    }
    if (!details.isEmpty())
        parts << details.join(QStringLiteral("; "));
    else if (json.isNull() && !data.isEmpty())
        parts << QString::fromUtf8(data.left(200));
    return parts.join(QStringLiteral(" - "));
}

// Товар программы по артикулу/коду МоегоСклада: те же правила, что у заказов Маркета (артикул на Маркете, артикул, штрихкод).
QString normKey(const QString &s) { return s.trimmed().toLower(); }

} // namespace

// ---------------------------------------------------------------------------
// Настройки

QStringList matchByValues()
{
    return {QStringLiteral("name"), QStringLiteral("article"), QStringLiteral("code")};
}

QString matchByTitle(const QString &value)
{
    if (value == QLatin1String("article"))
        return QStringLiteral("Артикул в МойСклад (если его нет - код)");
    if (value == QLatin1String("code"))
        return QStringLiteral("Код в МойСклад");
    return QStringLiteral("Наименование в МойСклад");
}

int Settings::mappedStores() const
{
    int n = 0;
    for (auto it = storeMap.cbegin(); it != storeMap.cend(); ++it)
        if (it.value() > 0)
            ++n;
    return n;
}

Settings loadSettings()
{
    Settings s;
    const QJsonObject o = QJsonDocument::fromJson(AppSettings::get(QString::fromLatin1(kConfigKey)).toUtf8()).object();
    s.token = o.value(QStringLiteral("token")).toString();
    s.freeStock = o.value(QStringLiteral("free")).toBool();
    s.zeroUnknown = o.value(QStringLiteral("zeroUnknown")).toBool();
    s.matchBy = o.value(QStringLiteral("matchBy")).toString();
    if (!matchByValues().contains(s.matchBy))
        s.matchBy = QStringLiteral("name");
    const QJsonObject map = o.value(QStringLiteral("storeMap")).toObject();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        if (it.value().toInt() > 0)
            s.storeMap.insert(it.key(), it.value().toInt());
    return s;
}

bool saveSettings(const Settings &s)
{
    QJsonObject map;
    for (auto it = s.storeMap.cbegin(); it != s.storeMap.cend(); ++it)
        if (it.value() > 0)
            map.insert(it.key(), it.value());
    const QJsonObject o{{QStringLiteral("token"), s.token.trimmed()},
                        {QStringLiteral("free"), s.freeStock},
                        {QStringLiteral("zeroUnknown"), s.zeroUnknown},
                        {QStringLiteral("matchBy"), matchByValues().contains(s.matchBy) ? s.matchBy : QStringLiteral("name")},
                        {QStringLiteral("storeMap"), map}};
    return AppSettings::set(QString::fromLatin1(kConfigKey), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

QList<Store> cachedStores()
{
    QList<Store> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(AppSettings::get(QString::fromLatin1(kStoresKey)).toUtf8()).array()) {
        const QJsonObject o = v.toObject();
        out.append({o.value(QStringLiteral("id")).toString(), o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("archived")).toBool()});
    }
    return out;
}

void cacheStores(const QList<Store> &stores)
{
    QJsonArray a;
    for (const Store &s : stores)
        a.append(QJsonObject{{QStringLiteral("id"), s.id}, {QStringLiteral("name"), s.name}, {QStringLiteral("archived"), s.archived}});
    AppSettings::set(QString::fromLatin1(kStoresKey), QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
}

// ---------------------------------------------------------------------------
// API

struct Api::Reply {
    int status = 0;
    QJsonDocument json;
    QString error;
};

QString Api::baseUrl()
{
    const QByteArray env = qgetenv("MOYSKLAD_API_BASE_URL");
    return env.isEmpty() ? QStringLiteral("https://api.moysklad.ru/api/remap/1.2") : QString::fromUtf8(env);
}

Api::Api(const QString &token, Log log)
    : m_token(token.trimmed()), m_log(std::move(log)), m_nam(new QNetworkAccessManager)
{
}

Api::~Api()
{
    delete m_nam;
}

QByteArray Api::authHeader() const
{
    // «логин:пароль» - обычная авторизация (Basic), иначе токен (Bearer).
    if (m_token.contains(QLatin1Char(':')))
        return "Basic " + m_token.toUtf8().toBase64();
    return "Bearer " + m_token.toUtf8();
}

Api::Reply Api::get(const QString &path, const QUrlQuery &query)
{
    Reply r;
    for (int attempt = 0; attempt < kRetries; ++attempt) {
        QUrl url(baseUrl() + path);
        url.setQuery(query);
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", authHeader());
        req.setRawHeader("Accept", "application/json;charset=utf-8");
        req.setTransferTimeout(kTimeoutMs);
        QNetworkReply *reply = m_nam->get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        r = Reply();
        r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        r.json = QJsonDocument::fromJson(data);
        if (r.status == 0)
            r.error = QStringLiteral("нет связи с сервисом МойСклад: %1").arg(reply->errorString());
        else if (r.status >= 400)
            r.error = errorText(r.status, r.json, data);
        reply->deleteLater();

        if (r.status == 429 && attempt + 1 < kRetries) {
            if (m_log)
                m_log(QStringLiteral("МойСклад просит подождать (429), повторяю через %1 с").arg(retryWaitMs() / 1000.0, 0, 'f', 1));
            QEventLoop wait;
            QTimer::singleShot(retryWaitMs(), &wait, &QEventLoop::quit);
            wait.exec();
            continue;
        }
        break;
    }
    if (m_log && !r.error.isEmpty())
        m_log(QStringLiteral("GET %1: %2").arg(path, r.error));
    return r;
}

bool Api::stores(QList<Store> *out, QString *error)
{
    out->clear();
    for (int page = 0, offset = 0; page < kMaxPages; ++page) {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("limit"), QString::number(kPageSize));
        q.addQueryItem(QStringLiteral("offset"), QString::number(offset));
        const Reply r = get(QStringLiteral("/entity/store"), q);
        if (!r.error.isEmpty()) {
            if (error)
                *error = r.error;
            return false;
        }
        const QJsonArray rows = r.json.object().value(QStringLiteral("rows")).toArray();
        for (const QJsonValue &v : rows) {
            const QJsonObject o = v.toObject();
            QString id = o.value(QStringLiteral("id")).toString();
            if (id.isEmpty())
                id = idFromHref(o.value(QStringLiteral("meta")).toObject().value(QStringLiteral("href")).toString());
            if (!id.isEmpty())
                out->append({id, o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("archived")).toBool()});
        }
        offset += int(rows.size());
        const int size = r.json.object().value(QStringLiteral("meta")).toObject().value(QStringLiteral("size")).toInt();
        if (rows.isEmpty() || offset >= size)
            break;
    }
    return true;
}

bool Api::stockReport(QList<Position> *out, QString *error)
{
    out->clear();
    // stockMode=all - все товары, в том числе с нулевым остатком: товар, которого в отчёте нет, МойСклад «не знает», а товар с нулём
    // должен обнулиться в программе. Если МойСклад такой фильтр не принимает, отчёт запрашивается как есть.
    bool withMode = true;
    for (int page = 0, offset = 0; page < kMaxPages; ++page) {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("limit"), QString::number(kPageSize));
        q.addQueryItem(QStringLiteral("offset"), QString::number(offset));
        if (withMode)
            q.addQueryItem(QStringLiteral("filter"), QStringLiteral("stockMode=all"));
        Reply r = get(QStringLiteral("/report/stock/bystore"), q);
        if (r.status == 400 && withMode && page == 0) {
            if (m_log)
                m_log(QStringLiteral("МойСклад не принял фильтр stockMode=all, запрашиваю отчёт без него"));
            withMode = false;
            --page;
            continue;
        }
        if (!r.error.isEmpty()) {
            if (error)
                *error = r.error;
            return false;
        }
        const QJsonArray rows = r.json.object().value(QStringLiteral("rows")).toArray();
        for (const QJsonValue &v : rows) {
            const QJsonObject o = v.toObject();
            Position p;
            p.article = o.value(QStringLiteral("article")).toString().trimmed();
            p.code = o.value(QStringLiteral("code")).toString().trimmed();
            p.name = o.value(QStringLiteral("name")).toString();
            for (const QJsonValue &sv : o.value(QStringLiteral("stockByStore")).toArray()) {
                const QJsonObject so = sv.toObject();
                const QString id = idFromHref(so.value(QStringLiteral("meta")).toObject().value(QStringLiteral("href")).toString());
                if (!id.isEmpty())
                    p.byStore.insert(id, {so.value(QStringLiteral("stock")).toDouble(), so.value(QStringLiteral("reserve")).toDouble()});
            }
            out->append(p);
        }
        offset += int(rows.size());
        const int size = r.json.object().value(QStringLiteral("meta")).toObject().value(QStringLiteral("size")).toInt();
        if (rows.isEmpty() || offset >= size)
            break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Расхождения и запись

qint64 Plan::unitsBefore() const
{
    qint64 n = 0;
    for (const Change &c : changes)
        n += c.was;
    return n;
}

qint64 Plan::unitsAfter() const
{
    qint64 n = 0;
    for (const Change &c : changes)
        n += c.will;
    return n;
}

namespace {

bool fail(QString *error, const QString &text)
{
    if (error)
        *error = text;
    return false;
}

} // namespace

bool buildPlan(const Settings &settings, Api &api, Plan *plan, QString *error)
{
    *plan = Plan();
    plan->zeroUnknown = settings.zeroUnknown;
    if (!settings.isConfigured())
        return fail(error, QStringLiteral("Не указан токен МойСклад."));
    // Склады: только существующие склады программы.
    QHash<int, QString> whNames;
    {
        QSqlQuery q(QStringLiteral("SELECT id, name FROM warehouses"));
        while (q.next())
            whNames.insert(q.value(0).toInt(), q.value(1).toString());
    }
    QHash<QString, int> storeMap;
    for (auto it = settings.storeMap.cbegin(); it != settings.storeMap.cend(); ++it)
        if (it.value() > 0 && whNames.contains(it.value()))
            storeMap.insert(it.key(), it.value());
    if (storeMap.isEmpty())
        return fail(error, QStringLiteral("Не сопоставлен ни один склад: укажите, какой склад МойСклад - какой склад программы."));
    plan->mappedWarehouses = int(QSet<int>(storeMap.begin(), storeMap.end()).size());

    QList<Position> report;
    QString err;
    if (!api.stockReport(&report, &err))
        return fail(error, err);
    plan->positions = int(report.size());
    if (report.isEmpty())
        return fail(error, QStringLiteral("МойСклад вернул пустой отчёт «Остатки»: в нём нет ни одного товара. Ничего не меняю."));

    // Что в МоемСкладе считается артикулом товара: наименование, артикул (если пуст - код) или код.
    const QString by = matchByValues().contains(settings.matchBy) ? settings.matchBy : QStringLiteral("name");
    auto keyOf = [&](const Position &p) -> QString {
        if (by == QLatin1String("name"))
            return p.name.trimmed();
        if (by == QLatin1String("code"))
            return p.code;
        return p.article.isEmpty() ? p.code : p.article;
    };
    QStringList keys;
    for (const Position &p : std::as_const(report)) {
        const QString k = keyOf(p);
        if (!k.isEmpty())
            keys << k;
    }
    QStringList ambiguousKeys;
    const QHash<QString, int> products = OrderWriteoff::productsForKeys(keys, &ambiguousKeys);
    const QSet<QString> ambiguousSet(ambiguousKeys.begin(), ambiguousKeys.end());

    QHash<int, QHash<int, int>> desired;    // склад программы -> товар -> остаток
    int negative = 0, fractional = 0;
    auto toQty = [&](double v) {
        if (v < 0) {
            ++negative;
            return 0;
        }
        const qint64 r = qRound64(v);
        if (qAbs(v - double(r)) > 0.0005)
            ++fractional;
        return int(qMin<qint64>(r, kMaxQty));
    };
    for (const Position &p : std::as_const(report)) {
        int pid = 0;
        bool amb = false;
        const QString key = normKey(keyOf(p));
        if (!key.isEmpty()) {
            if (ambiguousSet.contains(key))
                amb = true;
            else if (products.contains(key))
                pid = products.value(key);
        }
        // Остаток этого товара на сопоставленных складах.
        double onMapped = 0;
        for (auto it = storeMap.cbegin(); it != storeMap.cend(); ++it) {
            const auto s = p.byStore.value(it.key());
            onMapped += settings.freeStock ? s.first - s.second : s.first;
        }
        if (pid == 0) {
            if (amb)
                ++plan->ambiguous;
            else if (onMapped > 0) {
                ++plan->unknown;
                if (plan->unknownSamples.size() < 8)
                    plan->unknownSamples << keyOf(p);
            }
            continue;
        }
        ++plan->matched;
        for (auto it = storeMap.cbegin(); it != storeMap.cend(); ++it) {
            // Склада нет в строке отчёта - на нём у товара нуль.
            const auto s = p.byStore.value(it.key());
            const int qty = toQty(settings.freeStock ? s.first - s.second : s.first);
            desired[it.value()][pid] += qty;    // два склада МоегоСклада на один склад программы складываются; два товара МоегоСклада на один товар - тоже
        }
    }
    if (plan->matched == 0)
        return fail(error, QStringLiteral("Ни один из %1 товаров МойСклад не найден в программе: «%2» не совпало с артикулами товаров в программе "
                                          "(или артикулом на Маркете). Проверьте, что в МойСклад считается артикулом (вкладка «Подключение» → «Сопоставлять "
                                          "товары по»). Ничего не меняю.").arg(plan->positions).arg(matchByTitle(by)));
    if (negative > 0)
        plan->notes << QStringLiteral("отрицательных остатков в МойСклад: %1 (приняты за 0)").arg(negative);
    if (fractional > 0)
        plan->notes << QStringLiteral("дробных остатков: %1 (округлены до целых)").arg(fractional);
    if (plan->ambiguous > 0)
        plan->notes << QStringLiteral("артикул подходит нескольким товарам программы, пропущено: %1").arg(plan->ambiguous);

    // Текущие остатки программы на сопоставленных складах.
    QSet<int> changedProducts;
    QHash<int, QHash<int, int>> have;       // склад -> товар -> остаток
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT product_id, quantity FROM stock WHERE warehouse_id = ?"));
        q.addBindValue(it.key());
        if (!q.exec())
            return fail(error, q.lastError().text());
        while (q.next())
            have[it.key()].insert(q.value(0).toInt(), q.value(1).toInt());
    }
    QList<int> whIds = desired.keys();
    std::sort(whIds.begin(), whIds.end());
    for (int wh : std::as_const(whIds)) {
        WarehouseLevels wl;
        wl.warehouseId = wh;
        wl.warehouse = whNames.value(wh);
        const QHash<int, int> &want = desired.value(wh);
        const QHash<int, int> &cur = have.value(wh);
        QList<int> pids = want.keys();
        std::sort(pids.begin(), pids.end());
        for (int pid : std::as_const(pids)) {
            wl.levels.append({pid, want.value(pid)});
            const int was = cur.value(pid, 0);
            if (was > 0)
                ++plan->positiveNow;
            if (was != want.value(pid)) {
                plan->changes.append({pid, QString(), QString(), wh, wl.warehouse, was, want.value(pid)});
                changedProducts.insert(pid);
                if (want.value(pid) == 0)
                    ++plan->zeroed;
            }
        }
        if (settings.zeroUnknown) {
            QList<int> others;
            for (auto it = cur.cbegin(); it != cur.cend(); ++it)
                if (it.value() > 0 && !want.contains(it.key()))
                    others << it.key();
            std::sort(others.begin(), others.end());
            for (int pid : std::as_const(others)) {
                ++plan->positiveNow;
                ++plan->zeroed;
                plan->changes.append({pid, QString(), QString(), wh, wl.warehouse, cur.value(pid), 0});
                changedProducts.insert(pid);
            }
        } else {
            for (auto it = cur.cbegin(); it != cur.cend(); ++it)
                if (it.value() > 0 && !want.contains(it.key()))
                    ++plan->positiveNow;
        }
        plan->warehouses.append(wl);
    }

    // Артикулы и названия для показа.
    {
        QHash<int, QPair<QString, QString>> info;
        const QList<int> ids = changedProducts.values();
        for (int start = 0; start < ids.size(); start += 400) {
            QStringList marks;
            const QList<int> part = ids.mid(start, 400);
            for (int i = 0; i < part.size(); ++i)
                marks << QStringLiteral("?");
            QSqlQuery q;
            q.prepare(QStringLiteral("SELECT id, sku, name FROM products WHERE id IN (%1)").arg(marks.join(QLatin1Char(','))));
            for (int id : part)
                q.addBindValue(id);
            if (q.exec())
                while (q.next())
                    info.insert(q.value(0).toInt(), {q.value(1).toString(), q.value(2).toString()});
        }
        for (Change &c : plan->changes) {
            c.sku = info.value(c.productId).first;
            c.name = info.value(c.productId).second;
        }
        std::sort(plan->changes.begin(), plan->changes.end(), [](const Change &a, const Change &b) {
            const int c = QString::compare(a.warehouse, b.warehouse, Qt::CaseInsensitive);
            return c != 0 ? c < 0 : QString::compare(a.sku, b.sku, Qt::CaseInsensitive) < 0;
        });
    }

    // Похоже на ошибку сопоставления, а не на настоящие остатки: сразу много товаров обнуляется.
    if (plan->zeroed >= 20 && plan->zeroed * 2 > plan->positiveNow) {
        plan->suspicious = true;
        plan->suspicion = QStringLiteral("обнуляется %1 из %2 позиций с остатком - это больше половины").arg(plan->zeroed).arg(plan->positiveNow);
    }
    return true;
}

bool apply(const Plan &plan, const QString &comment, ApplyResult *result, QString *error)
{
    if (result)
        *result = ApplyResult();
    if (plan.warehouses.isEmpty())
        return true;
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction())
        return fail(error, db.lastError().text());
    int changed = 0;
    {
        // Один документ на все склады: открыть в разделе «Документы» и отменить можно целиком.
        Database::Document doc(QStringLiteral("inventory"), comment);
        for (const WarehouseLevels &wl : plan.warehouses) {
            int c = 0;
            QString err;
            if (!Database::setStockLevelsInTransaction(wl.warehouseId, wl.levels, plan.zeroUnknown, comment, &c, &err)) {
                db.rollback();
                return fail(error, QStringLiteral("склад «%1»: %2").arg(wl.warehouse, err));
            }
            changed += c;
        }
    }
    const int documentId = changed > 0 ? Database::lastDocumentId() : 0;
    if (!db.commit()) {
        db.rollback();
        return fail(error, db.lastError().text());
    }
    if (result) {
        result->changed = changed;
        result->documentId = documentId;
    }
    return true;
}

QString summary(const Plan &plan)
{
    QString s = QStringLiteral("МойСклад: товаров %1, найдено в программе %2, расхождений %3").arg(plan.positions).arg(plan.matched).arg(plan.changes.size());
    if (plan.unknown > 0)
        s += QStringLiteral(", с остатком, но не найдено в программе %1").arg(plan.unknown);
    return s;
}

} // namespace MoySklad

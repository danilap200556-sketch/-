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
#include <QRegularExpression>
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
    return {QStringLiteral("name"), QStringLiteral("base"), QStringLiteral("article"), QStringLiteral("code")};
}

QString matchByTitle(const QString &value)
{
    if (value == QLatin1String("base"))
        return QStringLiteral("Наименование без размера (часть до «_») в МойСклад");
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
    s.createMissing = o.value(QStringLiteral("createMissing")).toBool(true);
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
                        {QStringLiteral("createMissing"), s.createMissing},
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
            if (out->isEmpty() && m_rowKeys.isEmpty())
                m_rowKeys = o.keys();
            Position p;
            p.id = idFromHref(o.value(QStringLiteral("meta")).toObject().value(QStringLiteral("href")).toString());
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
    // Часть аккаунтов отдаёт строки отчёта без названия, кода и артикула (только ссылка на товар): берём их из ассортимента.
    int named = 0;
    for (const Position &p : std::as_const(*out))
        named += (p.name.trimmed().isEmpty() && p.article.isEmpty() && p.code.isEmpty()) ? 0 : 1;
    if (!out->isEmpty() && named * 2 < out->size()) {
        if (m_log)
            m_log(QStringLiteral("В строках отчёта нет названий товаров (поля: %1): читаю их из ассортимента").arg(m_rowKeys.join(QStringLiteral(", "))));
        QString err;
        if (!fillFromAssortment(out, &err) && m_log)
            m_log(QStringLiteral("Не удалось прочитать ассортимент: %1").arg(err));
    }
    return true;
}

bool Api::enrich(QList<Position> *positions, QString *error)
{
    if (m_assortmentLoaded)
        return true;
    return fillFromAssortment(positions, error);
}

bool Api::fillFromAssortment(QList<Position> *positions, QString *error)
{
    QHash<QString, int> byId;
    for (int i = 0; i < positions->size(); ++i)
        if (!positions->at(i).id.isEmpty())
            byId.insert(positions->at(i).id, i);
    if (byId.isEmpty()) {
        if (error)
            *error = QStringLiteral("в строках отчёта нет ссылок на товары");
        return false;
    }
    int filled = 0;
    for (int page = 0, offset = 0; page < kMaxPages; ++page) {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("limit"), QString::number(kPageSize));
        q.addQueryItem(QStringLiteral("offset"), QString::number(offset));
        const Reply r = get(QStringLiteral("/entity/assortment"), q);
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
            const auto it = byId.constFind(id);
            if (it == byId.constEnd())
                continue;
            Position &p = (*positions)[it.value()];
            if (p.name.trimmed().isEmpty())
                p.name = o.value(QStringLiteral("name")).toString();
            if (p.code.isEmpty())
                p.code = o.value(QStringLiteral("code")).toString().trimmed();
            if (p.article.isEmpty())
                p.article = o.value(QStringLiteral("article")).toString().trimmed();
            // Штрихкоды: «barcodes»: [{«ean13»: «...»}, {«code128»: «...»}, ...] - значение в объекте под ключом вида штрихкода.
            for (const QJsonValue &bv : o.value(QStringLiteral("barcodes")).toArray()) {
                const QJsonObject bo = bv.toObject();
                for (auto bit = bo.constBegin(); bit != bo.constEnd(); ++bit) {
                    const QString code = bit.value().toString().trimmed();
                    if (!code.isEmpty() && !p.barcodes.contains(code))
                        p.barcodes << code;
                }
            }
            ++filled;
        }
        offset += int(rows.size());
        const int size = r.json.object().value(QStringLiteral("meta")).toObject().value(QStringLiteral("size")).toInt();
        if (rows.isEmpty() || offset >= size)
            break;
    }
    m_assortmentLoaded = true;
    if (m_log)
        m_log(QStringLiteral("Из ассортимента получены названия и штрихкоды для %1 из %2 товаров").arg(filled).arg(positions->size()));
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

// Связь с базой после долгого ожидания ответа МойСклад: сервер мог закрыть неактивное соединение. Проверяем простым запросом и, если он не
// проходит, открываем соединение заново (параметры подключения у него сохранились). *reopened - соединение пришлось восстановить.
bool ensureDb(QString *why, bool *reopened)
{
    if (reopened)
        *reopened = false;
    QSqlDatabase db = QSqlDatabase::database();
    QString first;
    {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("SELECT 1")))
            return true;
        first = q.lastError().text();
    }
    db.close();
    if (db.open()) {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral("SELECT 1"))) {
            if (reopened)
                *reopened = true;
            return true;
        }
    }
    if (why)
        *why = QStringLiteral("%1 (повторное подключение: %2)").arg(first, db.lastError().text());
    return false;
}

// Сколько товаров в программе; -1 и текст ошибки базы, если прочитать не удалось.
int countProducts(QString *dbError)
{
    QSqlQuery c;
    if (!c.exec(QStringLiteral("SELECT count(*) FROM products")) || !c.next()) {
        if (dbError)
            *dbError = c.lastError().text();
        return -1;
    }
    return c.value(0).toInt();
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
        QSqlQuery q;
        if (!q.exec(QStringLiteral("SELECT id, name FROM warehouses")))
            return fail(error, QStringLiteral("Не удалось прочитать склады программы: %1").arg(q.lastError().text()));
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
    // Без товаров в программе сопоставлять нечего (если не включено создание карточек: тогда они создаются все): узнаём это сразу, не ожидая МойСклад.
    bool emptyApp = false;
    {
        QString dbErr;
        const int total = countProducts(&dbErr);
        if (total < 0)
            return fail(error, QStringLiteral("Не удалось прочитать товары программы: %1").arg(dbErr));
        emptyApp = total == 0;
        if (emptyApp && !settings.createMissing)
            return fail(error, QStringLiteral("В программе нет ни одного товара (таблица товаров пуста или к ней нет доступа): сопоставлять нечего."));
    }

    QList<Position> report;
    QString err;
    if (!api.stockReport(&report, &err))
        return fail(error, err);
    plan->positions = int(report.size());
    {
        QString why;
        bool reopened = false;
        if (!ensureDb(&why, &reopened))
            return fail(error, QStringLiteral("Связь с базой потеряна, пока шло ожидание ответа МойСклад, и восстановить её не удалось: %1").arg(why));
        if (reopened)
            plan->notes << QStringLiteral("связь с базой оборвалась за время ожидания МойСклад и была восстановлена");
    }
    if (report.isEmpty())
        return fail(error, QStringLiteral("МойСклад вернул пустой отчёт «Остатки»: в нём нет ни одного товара. Ничего не меняю."));

    // Что в МоемСкладе считается артикулом товара: наименование, артикул (если пуст - код) или код.
    const QString by = matchByValues().contains(settings.matchBy) ? settings.matchBy : QStringLiteral("name");
    auto keyFor = [](const QString &mode, const Position &p) -> QString {
        if (mode == QLatin1String("name"))
            return p.name.trimmed();
        if (mode == QLatin1String("base"))
            return p.name.trimmed().section(QLatin1Char('_'), 0, 0).trimmed();   // размеры одной модели складываются в один товар
        if (mode == QLatin1String("code"))
            return p.code;
        return p.article.isEmpty() ? p.code : p.article;
    };
    auto keyOf = [&](const Position &p) { return keyFor(by, p); };
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
    // Каждый склад программы, на который идёт загрузка, участвует, даже если ни один товар на нём не сопоставился (нужно для создания карточек).
    for (auto it = storeMap.cbegin(); it != storeMap.cend(); ++it)
        desired[it.value()];
    QHash<QString, int> newByKey;           // ключ товара (в нижнем регистре) -> номер в plan->newProducts
    QList<QList<int>> newPositions;         // строки отчёта, из которых собрана новая карточка (для штрихкодов)
    int unnamed = 0;
    for (int posIndex = 0; posIndex < report.size(); ++posIndex) {
        const Position &p = report.at(posIndex);
        int pid = 0;
        bool amb = false;
        const QString rawKey = keyOf(p);
        const QString key = normKey(rawKey);
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
            if (amb) {
                ++plan->ambiguous;
            } else if (settings.createMissing) {
                // Товара нет в программе: будет создана карточка с остатком из МойСклад (одинаковые ключи склеиваются: размеры одной модели).
                if (key.isEmpty()) {
                    ++unnamed;
                } else {
                    int ni = newByKey.value(key, -1);
                    if (ni < 0) {
                        NewProduct np;
                        np.sku = rawKey;
                        np.name = (by == QLatin1String("base") || p.name.trimmed().isEmpty()) ? rawKey : p.name.trimmed();
                        ni = int(plan->newProducts.size());
                        newByKey.insert(key, ni);
                        plan->newProducts.append(np);
                        newPositions.append(QList<int>());
                    }
                    newPositions[ni].append(posIndex);
                    for (auto it = storeMap.cbegin(); it != storeMap.cend(); ++it) {
                        const auto s = p.byStore.value(it.key());
                        plan->newProducts[ni].qty[it.value()] += toQty(settings.freeStock ? s.first - s.second : s.first);
                    }
                }
            } else if (onMapped > 0) {
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
    if (plan->matched == 0 && !(emptyApp && settings.createMissing)) {
        // Почему не нашлось: сколько совпало бы по каждому полю и как выглядят значения с обеих сторон (чтобы было видно, чем они различаются).
        const int sample = int(qMin<qsizetype>(report.size(), 400));
        QStringList parts;
        QString hint;
        int best = 0;
        for (const QString &mode : matchByValues()) {
            QStringList ks;
            for (int i = 0; i < sample; ++i) {
                const QString k = keyFor(mode, report.at(i));
                if (!k.isEmpty())
                    ks << k;
            }
            const int n = ks.isEmpty() ? 0 : int(OrderWriteoff::productsForKeys(ks).size());
            parts << QStringLiteral("%1 - %2").arg(matchByTitle(mode), QString::number(n));
            if (n > best) {
                best = n;
                hint = QStringLiteral(" Совпадения есть по полю «%1»: выберите его в «Сопоставлять товары по».").arg(matchByTitle(mode));
            }
        }
        QStringList theirs;
        for (int i = 0; i < report.size() && theirs.size() < 4; ++i) {
            const QString k = keyOf(report.at(i));
            if (!k.isEmpty())
                theirs << QStringLiteral("«%1»").arg(k);
        }
        QStringList ours;
        QString dbProblem;
        int productsTotal = 0;
        {
            QSqlQuery q;
            if (q.exec(QStringLiteral("SELECT sku FROM products ORDER BY id LIMIT 4")))
                while (q.next())
                    ours << QStringLiteral("«%1»").arg(q.value(0).toString());
            else
                dbProblem = q.lastError().text();
            productsTotal = qMax(0, countProducts(&dbProblem));
        }
        return fail(error, QStringLiteral("Ни один из %1 товаров МойСклад не найден в программе. Выбрано: «%2». Совпало бы на первых %3 товарах: %4.%5 "
                                          "В МойСклад (%2): %6. В программе (артикулы, всего товаров %7): %8. Ничего не меняю.")
                               .arg(plan->positions).arg(matchByTitle(by)).arg(sample).arg(parts.join(QStringLiteral("; "))).arg(hint)
                               .arg(theirs.isEmpty() ? QStringLiteral("это поле пустое (МойСклад не вернул его в отчёте?)") : theirs.join(QStringLiteral(", ")))
                               .arg(productsTotal).arg(ours.isEmpty() ? QStringLiteral("товаров нет") : ours.join(QStringLiteral(", ")))
                               + QStringLiteral(" Поля строки отчёта МойСклад: %1.").arg(api.lastRowKeys().isEmpty() ? QStringLiteral("(нет)") : api.lastRowKeys().join(QStringLiteral(", ")))
                               + (dbProblem.isEmpty() ? QString() : QStringLiteral(" Ошибка базы: %1.").arg(dbProblem)));
    }
    if (negative > 0)
        plan->notes << QStringLiteral("отрицательных остатков в МойСклад: %1 (приняты за 0)").arg(negative);
    if (fractional > 0)
        plan->notes << QStringLiteral("дробных остатков: %1 (округлены до целых)").arg(fractional);
    if (plan->ambiguous > 0)
        plan->notes << QStringLiteral("артикул подходит нескольким товарам программы, пропущено: %1").arg(plan->ambiguous);
    if (unnamed > 0)
        plan->notes << QStringLiteral("товаров МойСклад без наименования (артикула), для которых нельзя создать карточку: %1").arg(unnamed);

    // Новые карточки: штрихкоды берём из ассортимента МойСклад (в строках отчёта их нет).
    if (!plan->newProducts.isEmpty()) {
        if (!api.assortmentLoaded()) {
            QString eerr;
            if (!api.enrich(&report, &eerr))
                plan->notes << QStringLiteral("штрихкоды новых товаров не получены: %1").arg(eerr);
            QString why;
            bool reopened = false;
            if (!ensureDb(&why, &reopened))
                return fail(error, QStringLiteral("Связь с базой потеряна, пока шло ожидание ответа МойСклад, и восстановить её не удалось: %1").arg(why));
            if (reopened)
                plan->notes << QStringLiteral("связь с базой оборвалась за время ожидания МойСклад и была восстановлена");
        }
        static const QRegularExpression barcodeRe(QStringLiteral("^[0-9]{8,14}$"));
        for (int i = 0; i < plan->newProducts.size(); ++i) {
            NewProduct &np = plan->newProducts[i];
            for (int posIndex : std::as_const(newPositions[i]))
                for (const QString &b : report.at(posIndex).barcodes)
                    if (barcodeRe.match(b).hasMatch() && !np.barcodes.contains(b))
                        np.barcodes << b;
        }
    }

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

    // Новые карточки в таблице расхождений: по строке на каждый склад с остатком (или одна, если остатка нет).
    plan->created = int(plan->newProducts.size());
    for (const NewProduct &np : std::as_const(plan->newProducts)) {
        bool any = false;
        QList<int> whs = np.qty.keys();
        std::sort(whs.begin(), whs.end());
        for (int wh : std::as_const(whs)) {
            if (np.qty.value(wh) <= 0)
                continue;
            any = true;
            Change c;
            c.isNew = true;
            c.sku = np.sku;
            c.name = np.name;
            c.warehouseId = wh;
            c.warehouse = whNames.value(wh);
            c.was = 0;
            c.will = np.qty.value(wh);
            plan->changes.append(c);
        }
        if (!any) {
            Change c;
            c.isNew = true;
            c.sku = np.sku;
            c.name = np.name;
            plan->changes.append(c);
        }
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
            if (c.isNew)
                continue;
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
    // Новых карточек слишком много по сравнению с найденными товарами: скорее всего, артикулы не совпали, а не появились сотни новых товаров.
    if (!plan->suspicious && plan->created >= 50 && (plan->matched == 0 || plan->created * 2 > plan->matched)) {
        plan->suspicious = true;
        plan->suspicion = QStringLiteral("создаётся %1 новых карточек при %2 найденных товарах - похоже, артикулы не совпали").arg(plan->created).arg(plan->matched);
    }
    return true;
}

bool apply(const Plan &plan, const QString &comment, ApplyResult *result, QString *error)
{
    if (result)
        *result = ApplyResult();
    if (plan.warehouses.isEmpty())
        return true;
    {
        // Между расчётом и записью человек читает вопрос (иногда долго): соединение с базой могло закрыться.
        QString why;
        if (!ensureDb(&why, nullptr))
            return fail(error, QStringLiteral("нет связи с базой: %1").arg(why));
    }
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction())
        return fail(error, db.lastError().text());
    auto abort = [&](const QString &text) {
        db.rollback();
        return fail(error, text);
    };

    // 1. Карточки товаров, которых в программе нет: пачками по 100 (база удалённая, по запросу на товар вышло бы очень долго).
    QHash<QString, int> idBySku;
    int created = 0;
    for (int i = 0; i < plan.newProducts.size(); i += 100) {
        const QList<NewProduct> part = plan.newProducts.mid(i, 100);
        QStringList marks;
        for (int k = 0; k < part.size(); ++k)
            marks << QStringLiteral("(?, ?)");
        QSqlQuery q;
        q.prepare(QStringLiteral("INSERT INTO products (sku, name) VALUES %1 ON CONFLICT (sku) DO NOTHING RETURNING id, sku").arg(marks.join(QLatin1Char(','))));
        for (const NewProduct &np : part) {
            q.addBindValue(np.sku);
            q.addBindValue(np.name);
        }
        if (!q.exec())
            return abort(QStringLiteral("не удалось создать карточки товаров: %1").arg(q.lastError().text()));
        while (q.next()) {
            idBySku.insert(q.value(1).toString(), q.value(0).toInt());
            ++created;
        }
        QStringList exist;   // такой артикул уже есть (появился между расчётом и записью): берём этот товар
        for (const NewProduct &np : part)
            if (!idBySku.contains(np.sku))
                exist << np.sku;
        if (!exist.isEmpty()) {
            QStringList em;
            for (int k = 0; k < exist.size(); ++k)
                em << QStringLiteral("?");
            QSqlQuery sq;
            sq.prepare(QStringLiteral("SELECT id, sku FROM products WHERE sku IN (%1)").arg(em.join(QLatin1Char(','))));
            for (const QString &sku : exist)
                sq.addBindValue(sku);
            if (!sq.exec())
                return abort(QStringLiteral("не удалось прочитать существующие товары: %1").arg(sq.lastError().text()));
            while (sq.next())
                idBySku.insert(sq.value(1).toString(), sq.value(0).toInt());
        }
    }
    // Штрихкоды новых карточек: чужой (уже привязанный) штрихкод пропускается, а не останавливает загрузку.
    QList<QPair<QString, int>> codes;
    for (const NewProduct &np : plan.newProducts)
        for (const QString &b : np.barcodes)
            if (idBySku.value(np.sku) > 0)
                codes.append({b, idBySku.value(np.sku)});
    for (int i = 0; i < codes.size(); i += 200) {
        const QList<QPair<QString, int>> part = codes.mid(i, 200);
        QStringList marks;
        for (int k = 0; k < part.size(); ++k)
            marks << QStringLiteral("(?, ?)");
        QSqlQuery q;
        q.prepare(QStringLiteral("INSERT INTO product_barcodes (barcode, product_id) VALUES %1 ON CONFLICT (barcode) DO NOTHING").arg(marks.join(QLatin1Char(','))));
        for (const auto &c : part) {
            q.addBindValue(c.first);
            q.addBindValue(c.second);
        }
        if (!q.exec())
            return abort(QStringLiteral("не удалось привязать штрихкоды: %1").arg(q.lastError().text()));
    }

    // 2. Остатки: и у найденных товаров, и у только что созданных.
    QList<WarehouseLevels> whs = plan.warehouses;
    for (const NewProduct &np : plan.newProducts) {
        const int pid = idBySku.value(np.sku);
        if (pid <= 0)
            continue;
        for (auto it = np.qty.cbegin(); it != np.qty.cend(); ++it)
            if (it.value() > 0)
                for (WarehouseLevels &wl : whs)
                    if (wl.warehouseId == it.key())
                        wl.levels.append({pid, it.value()});
    }
    QString docComment = comment;
    if (created > 0)
        docComment += QStringLiteral(" (создано карточек: %1)").arg(created);
    int changed = 0;
    {
        // Один документ на все склады: открыть в разделе «Документы» и отменить можно целиком.
        Database::Document doc(QStringLiteral("inventory"), docComment);
        for (const WarehouseLevels &wl : std::as_const(whs)) {
            int c = 0;
            QString err;
            if (!Database::setStockLevelsInTransaction(wl.warehouseId, wl.levels, plan.zeroUnknown, docComment, &c, &err))
                return abort(QStringLiteral("склад «%1»: %2").arg(wl.warehouse, err));
            changed += c;
        }
    }
    const int documentId = changed > 0 ? Database::lastDocumentId() : 0;
    if (!db.commit()) {
        db.rollback();
        return fail(error, db.lastError().text());
    }
    if (result) {
        result->created = created;
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
    if (plan.created > 0)
        s += QStringLiteral(", будет создано карточек %1").arg(plan.created);
    return s;
}

} // namespace MoySklad

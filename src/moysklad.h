#pragma once

#include "database.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;
class QUrlQuery;

// Загрузка остатков из «МойСклад» (JSON API 1.2) в программу. МойСклад здесь главный: остатки выбранных складов в программе
// приводятся к его остаткам, а каждое расхождение пишется в журнал движений одним документом («Синхронизация с МойСклад»), который
// можно открыть в разделе «Документы» и отменить целиком. В МойСклад ничего не передаётся - только чтение.
//
// Что нужно: токен (Настройки МоегоСклада → «Токены») либо «логин:пароль» от МоегоСклада; и сопоставление складов: какой склад
// МоегоСклада - какой склад программы. Товары сопоставляются с артикулом товара в программе (или артикулом на Маркете), так же, как
// заказы Маркета. Что в МоемСкладе считать артикулом, выбирается: «Наименование» (так ведутся товары, когда артикул вписан в
// наименование; по умолчанию), «Артикул» (если его нет - код) или «Код».
//
// Формат ответов МоегоСклада взят из документации (отчёт «Остатки по складам», справочник складов); с настоящим аккаунтом
// эта часть не проверялась, поэтому перед записью есть показ расхождений, а автозагрузка не применяет подозрительные результаты.
namespace MoySklad {

struct Settings {
    QString token;                  // токен или «логин:пароль»
    bool freeStock = false;         // переносить доступный остаток (за вычетом резерва), а не физический
    bool zeroUnknown = false;       // обнулять товары программы, которых в отчёте МоегоСклада нет
    QString matchBy = QStringLiteral("name");   // что в МоемСкладе - артикул товара: name (наименование) | article (артикул, иначе код) | code (код)
    QHash<QString, int> storeMap;   // номер склада МоегоСклада -> номер склада программы
    bool isConfigured() const { return !token.trimmed().isEmpty(); }
    int mappedStores() const;
};
// Общие настройки лежат в базе (как токены Честного знака): на всех компьютерах одни и те же.
// Допустимые значения Settings::matchBy и их названия для людей.
QStringList matchByValues();
QString matchByTitle(const QString &value);
Settings loadSettings();
bool saveSettings(const Settings &settings);

struct Store {
    QString id;
    QString name;
    bool archived = false;
};
// Склады МоегоСклада, которые программа видела при последней проверке подключения (чтобы не ходить в сеть при открытии страницы).
QList<Store> cachedStores();
void cacheStores(const QList<Store> &stores);

// Строка отчёта «Остатки по складам»: товар и его остатки по складам (номер склада -> остаток, резерв).
struct Position {
    QString article;
    QString code;
    QString name;
    QHash<QString, QPair<double, double>> byStore;
};

class Api
{
public:
    using Log = std::function<void(const QString &)>;

    explicit Api(const QString &token, Log log = {});
    ~Api();
    Api(const Api &) = delete;
    Api &operator=(const Api &) = delete;

    // Для тестов адрес подменяется переменной окружения MOYSKLAD_API_BASE_URL.
    static QString baseUrl();

    bool stores(QList<Store> *out, QString *error);
    // Весь отчёт «Остатки по складам» (страницами по 1000 товаров).
    bool stockReport(QList<Position> *out, QString *error);

private:
    struct Reply;
    Reply get(const QString &path, const QUrlQuery &query);
    QByteArray authHeader() const;

    QString m_token;
    Log m_log;
    QNetworkAccessManager *m_nam;
};

struct Change {
    int productId = 0;
    QString sku;
    QString name;
    int warehouseId = 0;
    QString warehouse;
    int was = 0;
    int will = 0;
};

struct WarehouseLevels {
    int warehouseId = 0;
    QString warehouse;
    QList<Database::StockLevel> levels;
};

struct Plan {
    QList<Change> changes;                  // расхождения: что изменится
    QList<WarehouseLevels> warehouses;      // что записать (по складам программы)
    bool zeroUnknown = false;
    int positions = 0;                      // товаров в отчёте МоегоСклада
    int matched = 0;                        // из них найдено в программе
    int unknown = 0;                        // не найдено в программе (при этом остаток на сопоставленных складах есть)
    int ambiguous = 0;                      // артикул подходит нескольким товарам - пропущены
    QStringList unknownSamples;             // несколько артикулов из «не найдено»
    QStringList notes;                      // округления, отрицательные остатки и т. п.
    int zeroed = 0;                         // позиций, которые станут нулём
    int positiveNow = 0;                    // позиций с остатком в программе на сопоставленных складах
    int mappedWarehouses = 0;
    bool suspicious = false;                // результат похож на ошибку сопоставления: автозагрузка его не применяет
    QString suspicion;
    qint64 unitsBefore() const;
    qint64 unitsAfter() const;
};

// Считает расхождения (в базу ничего не пишет). false - причина в *error.
bool buildPlan(const Settings &settings, Api &api, Plan *plan, QString *error);

struct ApplyResult {
    int changed = 0;        // записей в журнале
    int documentId = 0;     // документ, 0 - расхождений не было
};
// Записывает остатки одним документом и одной транзакцией по всем складам: либо всё, либо ничего.
bool apply(const Plan &plan, const QString &comment, ApplyResult *result, QString *error);

// Одной строкой для журнала и трея.
QString summary(const Plan &plan);

} // namespace MoySklad

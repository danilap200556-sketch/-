#pragma once

#include <QHash>
#include <QJsonDocument>
#include <QList>
#include <QString>
#include <QUrlQuery>

#include <functional>

class QNetworkAccessManager;

// Кабинет (магазин) Яндекс Маркета, как он хранится в таблице market_accounts.
struct MarketAccount
{
    int id = 0;
    QString name;
    QString apiKey;
    qint64 businessId = 0;
    qint64 campaignId = 0;
    // В кабинете настроены "группы складов" - тогда остатки передаются и
    // читаются по магазину (v2 campaigns), а не по конкретному складу (v3).
    bool warehouseGroups = false;
    qint64 marketWarehouseId = 0; // partnerWarehouseId, нужен без групп складов
    QList<int> localWarehouseIds; // наши склады; пусто - все

    static QList<MarketAccount> loadAll();
    bool save(QString *error);
    static bool remove(int id, QString *error);
};

struct MarketCampaign
{
    qint64 id = 0;
    QString domain;
    qint64 businessId = 0;
    QString businessName;
    QString placementType;
    QString apiAvailability;
};

struct MarketWarehouse
{
    qint64 id = 0;
    QString name;
    QStringList models;
};

struct MarketOffer
{
    QString name;
    bool archived = false;
    bool inCampaign = true; // размещён в магазине этого кабинета
};

struct MarketStock
{
    qint64 fit = 0;       // "Годный": в наличии, включая резерв под заказы
    qint64 available = 0; // "Доступный к заказу"
};

struct MarketPrice
{
    double value = 0;
    double discountBase = 0; // "зачёркнутая" цена, 0 - нет
    QString currencyId;
};

struct MarketPriceUpdate
{
    QString offerId;
    double value = 0;
    double discountBase = 0; // 0 - не передавать
};

// Клиент API Яндекс Маркета для продавцов (https://api.partner.market.yandex.ru),
// схемы запросов - по официальной спецификации
// github.com/yandex-market/yandex-market-partner-api. Запросы синхронные
// (с локальным циклом событий), интерфейс при этом не замирает.
class MarketApi
{
public:
    using Log = std::function<void(const QString &)>;

    explicit MarketApi(const QString &apiKey, Log log = {});
    ~MarketApi();

    // Для тестов адрес можно подменить переменной окружения MARKET_API_BASE_URL.
    static QString baseUrl();

    bool campaigns(QList<MarketCampaign> *out, QString *error);
    bool partnerWarehouses(qint64 businessId, QList<MarketWarehouse> *out, QString *error);

    // Все товары каталога кабинета (offerId -> описание).
    bool catalog(const MarketAccount &acc, QHash<QString, MarketOffer> *out, QString *error);

    bool stocks(const MarketAccount &acc, QHash<QString, MarketStock> *out, QString *error);
    // Передаёт остатки пачками (не больше лимита API на запрос).
    bool updateStocks(const MarketAccount &acc, const QList<QPair<QString, qint64>> &items, QString *error);

    bool prices(qint64 businessId, QHash<QString, MarketPrice> *out, QString *error);
    bool updatePrices(qint64 businessId, const QList<MarketPriceUpdate> &items, QString *error);

private:
    struct Reply {
        int httpStatus = 0;
        QJsonDocument json;
        QString error; // пусто - успех
    };
    Reply request(const QByteArray &method, const QString &path, const QUrlQuery &query,
                  const QJsonDocument &body);

    QString m_apiKey;
    Log m_log;
    QNetworkAccessManager *m_nam;
};

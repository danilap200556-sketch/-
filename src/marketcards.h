#pragma once

#include "marketapi.h"
#include "marketcategories.h"

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// Значение характеристики категории в карточке (ParameterValueDTO).
struct MarketParamValue
{
    QString value;
    qint64 valueId = 0; // из справочника Маркета; 0 - своё значение
    qint64 unitId = 0;  // 0 - единица по умолчанию
};

// Карточка товара для ассортимента Яндекс Маркета: все данные, которые передаются методом
// offer-mappings/update. Хранится в таблице market_cards (одна на товар в кабинете), поэтому её можно
// готовить заранее и отправлять потом.
struct MarketCard
{
    int id = 0;
    int accountId = 0;
    int productId = 0; // 0 - не связана с товаром в базе
    QString offerId;   // артикул на Маркете (SKU у продавца)
    QString name;
    QString description;
    QString vendor;     // бренд
    QString vendorCode; // код производителя
    qint64 categoryId = 0;
    QString categoryName;
    QStringList barcodes;
    QStringList pictures; // ссылки (Маркет скачивает их сам)
    QStringList videos;
    QStringList countries; // страны производства
    QStringList tags;
    double lengthCm = 0, widthCm = 0, heightCm = 0, weightKg = 0;
    QString commodityCode; // ТН ВЭД
    double price = 0;
    bool adult = false;
    QMap<qint64, QList<MarketParamValue>> params; // характеристики категории
    QString status = QStringLiteral("draft");      // draft | sent | error
    QString marketStatus;                          // как Маркет назвал состояние карточки
    qint64 marketSku = 0;
    QString lastResult;
    QDateTime sentAt;
    QDateTime updatedAt;
};

namespace MarketCards {

// Какие данные взять из базы в карточку.
enum Field {
    FName = 1,
    FDescription = 2,
    FBarcodes = 4,
    FPrice = 8,
    FPhotos = 16,
    FVendor = 32,    // бренд (по названию) и код производителя (наш артикул), если они пусты
    FAll = 63
};

// Данные нашего товара, нужные для карточки.
struct ProductData {
    int id = 0;
    QString sku;
    QString name;
    QString description;
    QString marketSku;
    double price = 0;
    QStringList barcodes;
    QStringList photoLinks;
};

QList<MarketCard> loadAll(int accountId, QString *error = nullptr);
// Несколько карточек одним запросом (список из десятков карточек не должен значить десятки обращений к базе).
// overwrite=false: уже существующие (по артикулу в кабинете) пропускаются, их артикулы - в *skipped;
// overwrite=true: существующие обновляются (карточки должны быть уже сохранены, у них заполнен id).
bool saveMany(const QList<MarketCard *> &cards, bool overwrite, QStringList *skipped, QString *error);
// Товары из базы пачкой: данные, штрихкоды и (если withPhotos) ссылки на фото. Если фото запрошены, но заданы не
// полностью (нет адреса сайта) - причина в *photoError.
QHash<int, ProductData> loadProducts(const QList<int> &ids, const QString &siteUrl, bool withPhotos, QString *photoError);
// Новые карточки для списка товаров одним заходом в базу. *skipped - артикулы, для которых карточка уже была;
// *notes - замечания по отдельным товарам (нет фото и т.п.).
QList<MarketCard> createFromProducts(int accountId, const QList<int> &productIds, const QString &siteUrl,
                                     QStringList *skipped, QStringList *notes, QString *error);
// Обновляет данные нескольких карточек из базы (только выбранные поля) и сохраняет их.
bool refillFromProducts(const QList<MarketCard *> &cards, int fields, const QString &siteUrl, QStringList *notes,
                        QString *error);
// Сохраняет (новую - вставляет и заполняет id). Артикул на Маркете в кабинете должен быть уникальным.
bool save(MarketCard *card, QString *error);
bool remove(const QList<int> &ids, QString *error);

// Бренд по названию товара («Nike AF1 42» -> «Nike»); пусто, если не узнали.
QString guessVendor(const QString &productName);
// Артикул на Маркете для нашего товара: market_sku, а если его нет - наш артикул.
QString offerIdOf(const QString &sku, const QString &marketSku);
// Заполняет карточку данными товара из базы (только выбранные поля). Фото берутся ссылками с сайта
// (siteUrl), если адрес не задан - в *error причина, остальное заполняется. true - товар найден.
bool fillFromProduct(MarketCard *card, int productId, int fields, const QString &siteUrl, QString *error);
// Новая карточка для товара: артикул на Маркете, все данные из базы.
bool newFromProduct(MarketCard *card, int accountId, int productId, const QString &siteUrl, QString *error);

// Ссылки на фото товара для Маркета; фото без токена получают его (секретная часть ссылки).
QStringList publicPhotoLinks(int productId, const QString &siteUrl, QString *error);

// Подставляет в карточку данные, которые Маркет хранит у себя.
void applyMarketInfo(MarketCard *card, const MarketOfferInfo &info);
// Состояние карточки на Маркете по-русски.
QString describeCardStatus(const QString &cardStatus);

// Объект для offerMappings (offer + mapping) метода offer-mappings/update.
QJsonObject toOffer(const MarketCard &card);
// Чего не хватает до отправки (по-русски). params - характеристики категории (если известны):
// обязательные из них тоже проверяются.
QStringList missingForSend(const MarketCard &card, const QList<MarketCategoryParam> *params);

// Итог отправки карточек.
struct SendReport {
    int accepted = 0;
    int rejected = 0;
    QStringList lines; // по строке на карточку: что не так / предупреждения
};
// Отправляет карточки на Маркет пачками по 100. Если в пачке есть отклонённые карточки, Маркет не принимает
// ничего из этой пачки, поэтому остальные уходят повторно. Статус каждой карточки сохраняется в базе.
// params - характеристики категорий (для понятных сообщений об ошибках), может быть пустым.
SendReport send(MarketApi &api, const MarketAccount &acc, const QList<MarketCard *> &cards,
                const QHash<qint64, QList<MarketCategoryParam>> &params, QString *error);

// Характеристики <-> JSON в таблице.
QString paramsToJson(const QMap<qint64, QList<MarketParamValue>> &params);
QMap<qint64, QList<MarketParamValue>> paramsFromJson(const QString &json);

// Разбор текстовых полей: ссылки по строкам, списки через «;».
QStringList splitLines(const QString &text);
QStringList splitList(const QString &text);

} // namespace MarketCards

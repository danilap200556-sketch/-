#pragma once

#include "marketapi.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// Штрихкоды (GTIN) карточек Яндекс Маркета из нашей базы: берём штрихкоды товаров, сверяем с тем, что сейчас у карточек на Маркете,
// и передаём ТОЛЬКО штрихкоды методом offer-mappings/update (остальное в карточке не трогается: Маркет принимает только те поля, что
// переданы). Товар на Маркете и в базе связываются по артикулу на Маркете (market_sku, а если его нет - наш артикул).
//
// Маркет принимает штрихкоды EAN-13, EAN-8, UPC-A, UPC-E; для маркируемых товаров они должны быть настоящими GTIN. Свои коды приложения
// (EAN-13 из диапазона 20...) и вообще коды на 2 Маркет GTIN не считает. Какие коды НЕ передавать, решает пользователь: список начал
// («20» по умолчанию) в Options::excludePrefixes - всё, что начинается на любое из них, остаётся в базе и на Маркет не едет.
namespace MarketBarcodes {

enum class Kind {
    Gtin,        // верная контрольная цифра, подходит Маркету
    Internal,    // начинается на 2 или 02: внутренний код, не GTIN
    Invalid,     // не из цифр, неподходящая длина, неверная контрольная цифра
    Unsupported  // GTIN-14 с признаком упаковки: Маркет такой формат не принимает
};

struct Code {
    QString digits;        // код в виде для Маркета (GTIN-14 с нулём впереди приведён к EAN-13 / UPC-A)
    Kind kind = Kind::Invalid;
    QString note;          // почему не подходит (по-русски)
};
Code classify(const QString &barcode);

enum class Mode {
    Replace,  // на карточке остаются только наши штрихкоды
    Add       // наши штрихкоды добавляются к тем, что уже есть на Маркете
};

// Начала штрихкодов, которые по умолчанию не передаются на Маркет: свои коды приложения (20...).
QStringList defaultExcludePrefixes();
// «20, 21;4600» -> {"20","21","4600"}: разделители - запятая, точка с запятой, пробел; только цифры, без повторов.
QStringList parsePrefixes(const QString &text);

struct Options {
    Mode mode = Mode::Replace;
    QStringList excludePrefixes = defaultExcludePrefixes(); // штрихкоды с этим началом не передаём
};

enum class Status {
    Same,      // на Маркете уже то, что нужно
    Change,    // будет отправлено
    NoGtin,    // у нашего товара нет подходящего штрихкода - карточку не трогаем
    Archived   // карточка в архиве Маркета
};

struct Row {
    QString offerId;
    QString name;
    QList<int> productIds;
    QStringList ours;      // наши штрихкоды, которые передадим
    QStringList skipped;   // наши штрихкоды, которые не передаём, с причиной
    int skippedByPrefix = 0; // из них отсеяно по списку «не передавать начинающиеся на...»
    QStringList market;    // что сейчас на Маркете
    QStringList result;    // что будет на Маркете после отправки
    Status status = Status::Same;
    QString note;
};

struct Compared {
    QList<Row> rows;        // только товары, которые есть в каталоге кабинета; сначала те, что изменятся
    int ourOffers = 0;      // сколько артикулов в нашей базе (с учётом выбора)
    int notOnMarket = 0;    // из них нет в каталоге кабинета
};

// То, что загружено из базы и с Маркета: по нему build() быстро пересчитывает сравнение при других настройках (режим, список
// «не передавать начинающиеся на...»), не обращаясь к Маркету заново.
struct Source {
    struct Item {
        QString name;
        QList<int> productIds;
        QStringList barcodes;   // все штрихкоды нашего товара как в базе
        QStringList market;     // что сейчас на карточке Маркета
        bool archived = false;
    };
    QHash<QString, Item> items; // только артикулы из каталога кабинета
    int ourOffers = 0;
    int notOnMarket = 0;
};
// Загрузка: наши штрихкоды + каталог и штрихкоды карточек кабинета. onlyOfferIds - только эти артикулы (пусто - все товары базы).
bool load(const MarketAccount &acc, MarketApi &api, const QStringList &onlyOfferIds, Source *out, QString *error);
// Сравнение по загруженному (без сети).
Compared build(const Source &source, const Options &opt);
// load + build одним вызовом.
bool compare(const MarketAccount &acc, MarketApi &api, const QStringList &onlyOfferIds, const Options &opt, Compared *out, QString *error);

struct Report {
    int accepted = 0;
    int rejected = 0;
    QStringList lines;      // по строке на карточку: принято / причина отказа
    QStringList acceptedOffers;
    QStringList rejectedOffers;
};
// Отправляет строки со статусом Change пачками по 100. Если в пачке есть отклонённые карточки, Маркет не принимает ничего из этой
// пачки, поэтому остальные отправляются повторно. У принятых обновляются штрихкоды в наших карточках Маркета (market_cards).
Report push(MarketApi &api, const MarketAccount &acc, const QList<Row> &rows, QString *error);

QString statusText(Status status);

} // namespace MarketBarcodes

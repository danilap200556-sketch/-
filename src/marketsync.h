#pragma once

#include "marketapi.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

// Сверка остатков кабинета Яндекс Маркета с нашей базой: общая часть вкладки «Яндекс Маркет» и
// автообновления остатков по расписанию.
namespace MarketSync {

enum class Status { Match, Differ, NotInCatalog, NotInCampaign, Archived, NotInApp, NoMarketPrice, NoLocalPrice };

// Наш товар (или несколько, привязанных к одному артикулу Маркета) с остатком и ценой.
struct LocalItem {
    QString name;
    QList<int> productIds;
    qint64 stock = 0;
    double price = 0;
};

struct StockRow {
    QString offerId;
    QString name;
    qint64 local = 0;
    qint64 marketFit = 0;
    qint64 marketAvailable = 0;
    Status status = Status::Match;
};

// Наши товары по артикулу на Маркете (market_sku, а если его нет - наш артикул); остаток - по складам кабинета.
QHash<QString, LocalItem> loadLocal(const MarketAccount &acc);

// Загружает каталог и остатки с Маркета и сверяет с нашими.
bool compareStocks(const MarketAccount &acc, MarketApi &api, QList<StockRow> *rows, QString *error);

// Что передавать на Маркет: расхождения; с zeroMissing - ещё и обнуление того, чего у нас нет.
// *zeroed - сколько из них обнуления.
QList<QPair<QString, qint64>> itemsToPush(const QList<StockRow> &rows, bool zeroMissing, int *zeroed = nullptr);

} // namespace MarketSync

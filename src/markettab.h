#pragma once

#include "marketapi.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

// Работа с кабинетами Яндекс Маркета: сверка и передача остатков и цен.
// Всё, что уходит на Маркет, сначала показывается в таблице сверки с
// понятным статусом по каждой позиции, и отправляется только после
// подтверждения.
class MarketTab : public QWidget
{
    Q_OBJECT
public:
    explicit MarketTab(bool isAdmin, QWidget *parent = nullptr);

public slots:
    void refresh();

private slots:
    void addAccount();
    void editAccount();
    void deleteAccount();

    void syncStocks();
    void pushStocks();
    void syncPrices();
    void pushPrices();
    void pullPrices();
    void applyFilter();

private:
    enum class Status { Match, Differ, NotInCatalog, NotInCampaign, Archived, NotInApp, NoMarketPrice, NoLocalPrice };

    struct StockRow {
        QString offerId;
        QString name;
        qint64 local = 0;
        qint64 marketFit = 0;
        qint64 marketAvailable = 0;
        Status status = Status::Match;
    };
    struct PriceRow {
        QString offerId;
        QString name;
        QList<int> productIds;
        double local = 0;
        double market = 0;
        double discountBase = 0;
        Status status = Status::Match;
    };
    struct LocalItem {
        QString name;
        QList<int> productIds;
        qint64 stock = 0;
        double price = 0;
    };

    bool currentAccount(MarketAccount *acc);
    QHash<QString, LocalItem> loadLocal(const MarketAccount &acc) const;
    void log(const QString &text);
    void updateAccountInfo();
    void fillStockTable();
    void fillPriceTable();

    bool m_isAdmin;
    QList<MarketAccount> m_accounts;
    int m_stockAccountId = 0; // для какого кабинета сделана текущая сверка
    int m_priceAccountId = 0;
    QList<StockRow> m_stockRows;
    QList<PriceRow> m_priceRows;

    QComboBox *m_accountCombo;
    QLabel *m_accountInfo;
    QCheckBox *m_onlyDiffs;
    QCheckBox *m_zeroMissing;
    QTableWidget *m_stockTable;
    QLabel *m_stockSummary;
    QTableWidget *m_priceTable;
    QLabel *m_priceSummary;
    QPlainTextEdit *m_log;
};

#pragma once

#include "marketapi.h"
#include "marketsync.h"

#include <QWidget>

class QCheckBox;
class QLineEdit;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QListWidget;
class QSpinBox;
class QTabWidget;
class MarketAutoSync;

// Работа с кабинетами Яндекс Маркета: сверка и передача остатков и цен.
// Всё, что уходит на Маркет, сначала показывается в таблице сверки с
// понятным статусом по каждой позиции, и отправляется только после
// подтверждения.
class MarketTab : public QWidget
{
    Q_OBJECT
public:
    explicit MarketTab(bool isAdmin, QWidget *parent = nullptr);

    // Подключает страницу «Автообновление» (расписание отправки остатков).
    void setAutoSync(MarketAutoSync *sync);

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
    using Status = MarketSync::Status;
    using StockRow = MarketSync::StockRow;
    using LocalItem = MarketSync::LocalItem;

    struct PriceRow {
        QString offerId;
        QString name;
        QList<int> productIds;
        double local = 0;
        double market = 0;
        double discountBase = 0;
        Status status = Status::Match;
    };
    bool currentAccount(MarketAccount *acc);
    void log(const QString &text);
    void updateAccountInfo();
    void fillStockTable();
    void fillPriceTable();
    void fillAutoAccounts();
    void saveAutoSettings();
    void updateAutoStatus();

    bool m_isAdmin;
    QList<MarketAccount> m_accounts;
    int m_stockAccountId = 0; // для какого кабинета сделана текущая сверка
    int m_priceAccountId = 0;
    QList<StockRow> m_stockRows;
    QList<PriceRow> m_priceRows;

    QComboBox *m_accountCombo;
    QLabel *m_accountInfo;
    QCheckBox *m_onlyDiffs;
    QLineEdit *m_search;
    QCheckBox *m_zeroMissing;
    QTableWidget *m_stockTable;
    QLabel *m_stockSummary;
    QTableWidget *m_priceTable;
    QLabel *m_priceSummary;
    QPlainTextEdit *m_log;

    QTabWidget *m_pages = nullptr;
    MarketAutoSync *m_auto = nullptr;
    QCheckBox *m_autoEnabled = nullptr;
    QSpinBox *m_autoEvery = nullptr;
    QComboBox *m_autoUnit = nullptr;
    QListWidget *m_autoAccounts = nullptr;
    QCheckBox *m_autoZero = nullptr;
    QLabel *m_autoStatus = nullptr;
    QPushButton *m_autoRunBtn = nullptr;
};

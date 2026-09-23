#pragma once

#include "marketapi.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// Настройка одного кабинета Маркета: API-ключ, магазин (выбирается из
// списка, который отдаёт сам Маркет по ключу), склад Маркета и наши склады,
// остатки которых передаются в этот кабинет.
class MarketAccountDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MarketAccountDialog(const MarketAccount &account, QWidget *parent = nullptr);

    MarketAccount account() const { return m_account; }

private slots:
    void loadCampaigns();
    void loadWarehouses();
    void onCampaignChanged();
    void updateWarehouseControls();
    void validateAndAccept();

private:
    MarketAccount m_account;

    QLineEdit *m_name;
    QLineEdit *m_apiKey;
    QComboBox *m_campaign;
    QCheckBox *m_groups;
    QComboBox *m_warehouse;
    QPushButton *m_loadWarehousesBtn;
    QListWidget *m_localWarehouses;
    QLabel *m_status;
};

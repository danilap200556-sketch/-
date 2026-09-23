#pragma once

#include "marketapi.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QTableWidget;

// Заказы Яндекс Маркета по одному кабинету или по всем сразу: список с
// артикулами и местами хранения на нашем складе, выгрузка в Excel (заказы +
// сборочный лист) и PDF с ярлыками для наклейки на коробки.
class OrdersTab : public QWidget
{
    Q_OBJECT
public:
    explicit OrdersTab(QWidget *parent = nullptr);

public slots:
    void refresh();

private slots:
    void loadOrders();
    void exportExcel();
    void downloadLabels();

private:
    struct LoadedOrder {
        MarketOrder order;
        QString accountName;
        QString apiKey;
        qint64 businessId = 0;
    };

    QList<const LoadedOrder *> selectedOrders() const;
    QHash<QString, QString> loadLocations() const;
    void log(const QString &text);

    QList<MarketAccount> m_accounts;
    QList<LoadedOrder> m_orders;
    QHash<QString, QString> m_locations; // offerId -> где лежит у нас

    QComboBox *m_accountCombo;
    QComboBox *m_statusCombo;
    QComboBox *m_formatCombo;
    QTableWidget *m_table;
    QLabel *m_summary;
    QPlainTextEdit *m_log;
};

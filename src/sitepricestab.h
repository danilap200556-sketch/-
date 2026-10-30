#pragma once

#include "siteprices.h"

#include <QHash>
#include <QList>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QTimer;

// Вкладка «Цены сайта»: свои цены и скидки для интернет-магазина по моделям (артикул без размера). Правка прямо в
// таблице или массово (цена как на складе, наценка к цене склада, скидка в % на срок), обмен через Excel.
// Цена склада здесь не меняется: она нужна учёту и Яндекс Маркету. У модели без цены сайта сайт берёт цену склада.
class SitePricesTab : public QWidget
{
    Q_OBJECT
public:
    explicit SitePricesTab(QWidget *parent = nullptr);

    void refresh(); // перечитать товары и цены сайта

private slots:
    void applyFilter();
    void onItemChanged(QTableWidgetItem *item);
    void onSelectionChanged();
    void priceFromWarehouse();
    void setPriceDialog();
    void setSaleDialog();
    void clearSale();
    void removeSitePrice();
    void exportExcel();
    void importExcel();

private:
    enum Col { ColKey = 0, ColName, ColSizes, ColStock, ColWarehouse, ColPrice, ColPercent, ColSale, ColFrom, ColTo, ColNow, ColCount };

    int rowIndexOf(const QString &key) const;     // индекс в m_rows или -1
    QStringList selectedKeys() const;
    QList<int> selectedRowIndexes() const;        // индексы в m_rows
    void fillTableRow(int tableRow, const SitePrices::ModelRow &m);
    void updateTableRowFor(const QString &key);
    bool saveEntries(const QList<SitePrices::Entry> &entries, const QString &okText);
    void setSummary();
    void setStatus(const QString &text, bool problem = false);
    bool visibleByFilter(const SitePrices::ModelRow &m) const;

    QList<SitePrices::ModelRow> m_rows;
    QHash<QString, int> m_index; // ключ модели -> индекс в m_rows
    bool m_loading = false;

    QLineEdit *m_search;
    QTimer *m_searchTimer;
    QComboBox *m_filter;
    QTableWidget *m_table;
    QLabel *m_summary;
    QLabel *m_status;
    QPushButton *m_fromWarehouseBtn;
    QPushButton *m_priceBtn;
    QPushButton *m_saleBtn;
    QPushButton *m_clearSaleBtn;
    QPushButton *m_removeBtn;
};

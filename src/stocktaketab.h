#pragma once

#include "stocktake.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class ProductSuggest;

// Раздел «Инвентаризация»: выбираете склад, начинаете подсчёт и сканируете штрихкоды - каждый скан добавляет штуку товару.
// В таблице видно, сколько посчитано, сколько числится в учёте и разницу. При завершении остаток каждого посчитанного
// товара становится равным посчитанному, а у товаров склада, которых в подсчёте нет, - нулю (можно выключить). Всё
// проводится одним документом «Инвентаризация», который можно отменить в разделе «Документы». Подсчёт общий: с другого
// компьютера (и с сайта) в него можно считать одновременно.
class StocktakeTab : public QWidget
{
    Q_OBJECT
public:
    explicit StocktakeTab(QWidget *parent = nullptr);

    void refresh(); // перечитать склады и подсчёт

signals:
    void stockChanged();        // подсчёт проведён: остатки изменились
    void documentCreated(int id);

private slots:
    void warehouseChanged();
    void startCount();
    void scanEntered();
    void itemEdited(QTableWidgetItem *item);
    void finishCount();
    void discardCount();
    void exportReport();
    void viewChanged();
    void adjustSelected(int delta);
    void removeSelected();

private:
    enum View { Counted = 0, Differences = 1, Missing = 2 };
    enum Col { ColSku = 0, ColName, ColBook, ColCounted, ColDiff, ColCount };

    int warehouseId() const;
    void loadCount();           // подсчёт выбранного склада (или его отсутствие)
    void reloadTable();
    void updateSummary();
    void updateControls();
    void fillRow(int row, const Stocktake::Row &r);
    void showRow(const Stocktake::Row &r);   // после скана: строка наверх таблицы
    bool countProduct(int productId, int qty);
    void say(const QString &text, bool error);
    bool resolveCode(const QString &key, int *productId);
    int rowOfProduct(int productId) const;

    QComboBox *m_warehouse;
    QPushButton *m_startBtn;
    QPushButton *m_finishBtn;
    QPushButton *m_discardBtn;
    QPushButton *m_exportBtn;
    QLabel *m_banner;
    QLineEdit *m_scan;
    QLabel *m_status;
    QComboBox *m_view;
    QTableWidget *m_table;
    QLabel *m_summary;
    QCheckBox *m_zeroMissing;
    QPushButton *m_plusBtn;
    QPushButton *m_minusBtn;
    QPushButton *m_removeBtn;
    ProductSuggest *m_suggest;
    QTimer *m_summaryTimer;
    QTimer *m_autoTimer;

    Stocktake::Info m_info;
    QList<Stocktake::Row> m_rows;          // что сейчас в таблице
    QHash<QString, int> m_unknown;         // не опознанные коды -> сколько раз отсканированы
    bool m_filling = false;
    qint64 m_lastScan = 0;
};

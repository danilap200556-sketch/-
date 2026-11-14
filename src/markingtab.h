#pragma once

#include "markingcode.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class MarkingModel;
class QTableView;
class QTimer;

// Страница «Честный знак»: коды маркировки (КМ) в общей базе - добавить (вставкой, из файла, сканером или заказом в СУЗ),
// привязать к товарам по GTIN и напечатать на термоэтикетках 58x40. Только в программе, на сайте этой страницы нет.
class MarkingTab : public QWidget
{
    Q_OBJECT
public:
    explicit MarkingTab(QWidget *parent = nullptr);

    void refresh();

signals:
    void codesChanged();

private slots:
    void addCodes();
    void addFromFile();
    void orderCodes();
    void settings();
    void print();
    void introXml();
    void linkSelected();
    void removeSelected();
    void selectNew();
    void scanned();
    void assignTarget();
    void manageOwners();

private:
    QList<int> selectedIds() const;
    // ИП и склад в верхних списках: -1 - все, 0 - без ИП (без склада), больше 0 - этот ИП (склад). ИП и склад друг с другом не связаны.
    int ownerFilter() const;
    int warehouseFilter() const;
    void reloadOwners();
    // Куда записать новые коды (ИП и склад): диалог с выбором, подставлено выбранное в верхних списках. false - отказались. Без заведённых ИП и
    // складов вопросов нет: коды пишутся без ИП и без склада.
    bool targetForNewCodes(const QString &what, int *ownerId, int *warehouseId);
    void reload();
    void showNotice(const QString &text);
    void showAddResult(const MarkingCode::AddResult &result);

    QTableView *m_table;
    MarkingModel *m_model;
    QComboBox *m_status;
    QComboBox *m_owner;
    QComboBox *m_warehouse;
    QLineEdit *m_search;
    QLineEdit *m_scan;
    QLabel *m_counts;
    QLabel *m_notice = nullptr; // сообщение без модального окна: «напечатано N этикеток»
    QLabel *m_hint;
    QTimer *m_searchTimer;
};

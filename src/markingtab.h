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
    void assignOwner();
    void manageOwners();

private:
    QList<int> selectedIds() const;
    // ИП в верхнем списке: -1 - все, 0 - без ИП, больше 0 - этот ИП. Для новых кодов нужен конкретный ИП (или «без ИП», если ИП не заведены).
    int ownerFilter() const;
    void reloadOwners();
    // ИП для новых кодов: выбранный в списке; -1 - надо спросить (ИП есть, а в списке «все») или отказаться.
    int ownerForNewCodes(const QString &what);
    void reload();
    void showNotice(const QString &text);
    void showAddResult(const MarkingCode::AddResult &result);

    QTableView *m_table;
    MarkingModel *m_model;
    QComboBox *m_status;
    QComboBox *m_owner;
    QLineEdit *m_search;
    QLineEdit *m_scan;
    QLabel *m_counts;
    QLabel *m_notice = nullptr; // сообщение без модального окна: «напечатано N этикеток»
    QLabel *m_hint;
    QTimer *m_searchTimer;
};

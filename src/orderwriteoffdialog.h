#pragma once

#include "orderwriteoff.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

// Списание заказов со склада: что будет сделано с каждым заказом (списать, вернуть при отмене, уже сделано, нельзя - и
// почему), отметка нужных и кнопка «Списать отмеченные». Заказы Маркета передаёт вкладка «Заказы Маркета» (те, что
// загружены и выбраны), заказы сайта берутся из очереди в базе. Ничего не списывается без нажатия кнопки.
class OrderWriteoffDialog : public QDialog
{
    Q_OBJECT
public:
    OrderWriteoffDialog(const QList<OrderWriteoff::Order> &marketOrders, QWidget *parent = nullptr);

    // Что-то списано или возвращено: остатки изменились.
    bool changed() const { return m_changed; }

private slots:
    void reload();
    void applyChecked();
    void checkAll(bool on);
    void editSiteOrders();
    void updateButtons();

private:
    void fillTable();
    QList<OrderWriteoff::Planned> checkedRows() const;
    void log(const QString &line);

    QList<OrderWriteoff::Order> m_market;
    QList<OrderWriteoff::Planned> m_plan;
    bool m_changed = false;
    bool m_busy = false;

    QTableWidget *m_table;
    QLabel *m_summary;
    QCheckBox *m_showAll;
    QPlainTextEdit *m_log;
    QPushButton *m_applyBtn;
    QPushButton *m_siteBtn;
    QPushButton *m_closeBtn;
};

// «Заказы сайта»: с какого склада списывать заказы сайта и что сейчас в очереди.
class SiteOrdersDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SiteOrdersDialog(QWidget *parent = nullptr);
};

#pragma once

#include "marketbarcodes.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QTableWidget;
class QTimer;

// «Штрихкоды (GTIN) на Маркет»: сравнивает штрихкоды наших товаров с карточками Маркета, показывает, что изменится, и после
// подтверждения передаёт на Маркет только штрихкоды (см. MarketBarcodes). Ничего не отправляется без кнопки «Отправить отмеченные».
class MarketBarcodesDialog : public QDialog
{
    Q_OBJECT
public:
    // onlyOfferIds - только эти артикулы Маркета (выбранные карточки); пусто - все товары базы, которые есть в кабинете.
    MarketBarcodesDialog(const MarketAccount &account, const QStringList &onlyOfferIds, QWidget *parent = nullptr);

    // Что-то отправлено на Маркет (карточки в базе обновлены).
    bool changed() const { return m_changed; }

private slots:
    void compare();
    void send();
    void checkAll(bool on);
    void rebuild();      // пересчёт по уже загруженному (без запроса к Маркету)
    void fillTable();
    void applyFilter();
    void updateButtons();

private:
    MarketBarcodes::Options options() const;
    int checkedCount() const;   // отмечено среди видимых (скрытые фильтром не отправляются)
    void log(const QString &line);

    MarketAccount m_account;
    QStringList m_only;
    MarketBarcodes::Source m_source;
    MarketBarcodes::Compared m_compared;
    bool m_changed = false;
    bool m_busy = false;
    bool m_loaded = false;

    QRadioButton *m_replace;
    QRadioButton *m_add;
    QLineEdit *m_prefixes;
    QLineEdit *m_filter;
    QTimer *m_rebuildTimer;
    QCheckBox *m_showAll;
    QPushButton *m_compareBtn;
    QPushButton *m_sendBtn;
    QPushButton *m_closeBtn;
    QLabel *m_summary;
    QTableWidget *m_table;
    QPlainTextEdit *m_log;
};

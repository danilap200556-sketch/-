#pragma once

#include "marketcards.h"

#include <QHash>
#include <QList>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QVBoxLayout;
class ParamForm;

// Карточки товаров для ассортимента Яндекс Маркета: список слева, все поля карточки справа, включая характеристики
// выбранной категории (обязательные первыми). Карточки хранятся в базе как черновики; данные подставляются из товаров
// базы, отправка на Маркет - пачками, с понятным результатом по каждой карточке.
class CardsTab : public QWidget
{
    Q_OBJECT
public:
    explicit CardsTab(QWidget *parent = nullptr);

    // Сохраняет открытую карточку, если в ней что-то меняли (при уходе со страницы и закрытии окна).
    bool saveIfDirty() { return commitCurrent(); }

public slots:
    void refresh();

private slots:
    void accountChanged();
    void currentRowChanged();
    void addFromProducts();
    void addBlank();
    void removeSelected();
    void refillFromDatabase();
    void writeToProducts();
    void pullFromMarket();
    void checkOnMarket();
    void sendToMarket();
    void chooseCategory();
    void photoLinksFromDatabase();
    void saveCurrent();
    void applyFilter();

private:
    bool currentAccount(MarketAccount *acc) const;
    int currentCardIndex() const;
    QList<int> selectedCardIndexes() const;
    void reloadCards(int selectId = 0);
    void fillTable();
    void updateRow(int index);
    void showCard(int index);
    void readForm(MarketCard *card) const;
    // Сохраняет открытую карточку, если в ней что-то меняли.
    bool commitCurrent();
    void rebuildParams(const MarketCard &card, bool forceReload = false);
    QString siteUrl();
    void setStatus(const QString &text, bool error = false);
    void markDirty() { if (!m_loading) m_dirty = true; }
    void setFormEnabled(bool on);
    QHash<qint64, QList<MarketCategoryParam>> knownParams() const;

    QList<MarketAccount> m_accounts;
    QList<MarketCard> m_cards;
    int m_shown = -1; // индекс в m_cards карточки, открытой в форме
    bool m_loading = false;
    bool m_dirty = false;
    QJsonObject m_tree;
    QList<MarketCategoryParam> m_params;

    QComboBox *m_accountCombo;
    QLineEdit *m_search;
    QTableWidget *m_table;
    QLabel *m_summary;
    QLabel *m_status;

    QWidget *m_formPanel;
    QLineEdit *m_offerId;
    QLineEdit *m_name;
    QLineEdit *m_vendor;
    QLineEdit *m_vendorCode;
    QPlainTextEdit *m_description;
    QLineEdit *m_barcodes;
    QDoubleSpinBox *m_price;
    QLineEdit *m_countries;
    QLabel *m_categoryLabel;
    QPlainTextEdit *m_pictures;
    QPlainTextEdit *m_videos;
    QDoubleSpinBox *m_length;
    QDoubleSpinBox *m_width;
    QDoubleSpinBox *m_height;
    QDoubleSpinBox *m_weight;
    QLineEdit *m_commodity;
    QLineEdit *m_tags;
    QCheckBox *m_adult;
    QGroupBox *m_paramsBox;
    ParamForm *m_paramForm;
    QLabel *m_paramsHint;
    QLabel *m_cardState;
    QPushButton *m_pullProductBtn;
    QPushButton *m_pullMarketBtn;
};

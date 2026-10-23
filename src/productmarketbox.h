#pragma once

#include "productmarket.h"

#include <QWidget>
#include <functional>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QToolButton;
class ParamForm;

// Блок «Параметры для Яндекс Маркета» в окне товара: бренд, страны, габариты и вес упаковки, ТН ВЭД, теги, 18+,
// категория Маркета и её характеристики. Сворачивается; в свёрнутом виде показывает краткую сводку.
// Категории и характеристики берутся с Маркета (нужен подключённый кабинет) и кэшируются на компьютере.
class ProductMarketBox : public QWidget
{
    Q_OBJECT
public:
    explicit ProductMarketBox(QWidget *parent = nullptr);

    void setInfo(const ProductMarketInfo &info);
    ProductMarketInfo info() const;
    void setReadOnly(bool readOnly);
    // Откуда брать название товара для «бренд по названию».
    void setNameProvider(std::function<QString()> provider) { m_nameProvider = std::move(provider); }

private:
    void setExpanded(bool on);
    void updateSummary();
    void chooseCategory();
    void clearCategory();
    // allowNetwork: если характеристик категории нет в кэше, получить их с Маркета.
    void rebuildParams(bool allowNetwork);
    void guessVendor();

    ProductMarketInfo m_original; // что было в товаре: если характеристики показать нельзя, они остаются как есть
    qint64 m_categoryId = 0;
    QString m_categoryName;
    bool m_paramsShown = false;
    bool m_readOnly = false;
    std::function<QString()> m_nameProvider;

    QToolButton *m_toggle;
    QLabel *m_summary;
    QWidget *m_body;
    QLineEdit *m_vendor;
    QPushButton *m_vendorGuess;
    QLineEdit *m_countries;
    QDoubleSpinBox *m_length;
    QDoubleSpinBox *m_width;
    QDoubleSpinBox *m_height;
    QDoubleSpinBox *m_weight;
    QLineEdit *m_commodity;
    QLineEdit *m_tags;
    QCheckBox *m_adult;
    QLabel *m_categoryLabel;
    QPushButton *m_categoryBtn;
    QPushButton *m_categoryClear;
    QLabel *m_paramsHint;
    QPushButton *m_paramsLoad;
    QScrollArea *m_paramsScroll;
    ParamForm *m_paramForm;
};

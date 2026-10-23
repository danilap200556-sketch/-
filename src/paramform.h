#pragma once

#include "marketcards.h"
#include "marketcategories.h"

#include <QList>
#include <QMap>
#include <QWidget>
#include <functional>

class QFormLayout;
class QLabel;

// Поля характеристик категории Маркета: по полю на характеристику (список значений, несколько значений, число с единицей,
// да/нет или текст), обязательные отмечены *. Используется в карточке Маркета и в окне товара.
class ParamForm : public QWidget
{
    Q_OBJECT
public:
    explicit ParamForm(QWidget *parent = nullptr);

    // Показывает поля для params и подставляет значения из values (характеристика -> значения).
    void setParams(const QList<MarketCategoryParam> &params, const QMap<qint64, QList<MarketParamValue>> &values);
    // Убирает все поля.
    void clear();
    int count() const { return m_editors.size(); }
    // Значения всех полей; пустые характеристики не попадают в результат.
    QMap<qint64, QList<MarketParamValue>> values() const;

signals:
    void changed();

private:
    struct Editor {
        MarketCategoryParam param;
        QWidget *widget = nullptr;
        std::function<QList<MarketParamValue>()> get;
        std::function<void(const QList<MarketParamValue> &)> set;
    };
    Editor makeEditor(const MarketCategoryParam &p);

    QFormLayout *m_form;
    QList<Editor> m_editors;
};

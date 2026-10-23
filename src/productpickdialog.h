#pragma once

#include <QDialog>
#include <QList>
#include <QSet>

class QLabel;
class QLineEdit;
class QTableWidget;

// Выбор товаров из базы, для которых нужно создать карточки Маркета. Те, у кого карточка в кабинете уже есть
// (по артикулу на Маркете), отмечены и недоступны. Поиск - по артикулу, названию или штрихкоду (сканером).
class ProductPickDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProductPickDialog(const QSet<QString> &existingOfferIds, QWidget *parent = nullptr);

    QList<int> selectedProductIds() const;

private:
    void applyFilter();
    void updateSummary();

    QLineEdit *m_search;
    QTableWidget *m_table;
    QLabel *m_summary;
};

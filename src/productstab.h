#pragma once

#include <QWidget>

class QTableView;
class QSqlTableModel;

// Вкладка "Товары": список товаров + добавление/редактирование/удаление
// через ProductDialog, открытие фото во внешнем просмотрщике.
class ProductsTab : public QWidget
{
    Q_OBJECT
public:
    explicit ProductsTab(QWidget *parent = nullptr);

    void refresh();

signals:
    void productsChanged();

private slots:
    void addProduct();
    void editProduct();
    void deleteProduct();
    void openSelectedPhoto();

private:
    int selectedProductId() const;

    QTableView *m_table;
    QSqlTableModel *m_model;
};

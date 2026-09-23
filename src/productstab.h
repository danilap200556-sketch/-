#pragma once

#include <QWidget>

class QTableView;
class QSqlTableModel;
class QLineEdit;
class QLabel;

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
    void applySearch();
    void onSearchEntered();

private:
    int selectedProductId() const;
    void selectProduct(int productId);
    void saveBarcodes(int productId, const QStringList &codes);

    QLineEdit *m_search;
    QLabel *m_searchStatus;

    QTableView *m_table;
    QSqlTableModel *m_model;
};

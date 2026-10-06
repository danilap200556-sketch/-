#pragma once

#include "photostore.h"

#include <QWidget>

class QTableView;
class QSqlTableModel;
class QLineEdit;
class QLabel;

// Вкладка "Товары": список товаров с миниатюрами фото, добавление/редактирование/
// удаление через ProductDialog, массовое редактирование и загрузка фото пачкой
// для нескольких выделенных товаров.
class ProductsTab : public QWidget
{
    Q_OBJECT
public:
    explicit ProductsTab(QWidget *parent = nullptr);

    void refresh();
    // Роль "только просмотр": убирает кнопки изменения, карточка открывается без редактирования.
    void setReadOnly(bool readOnly);

signals:
    void productsChanged();

private slots:
    void addProduct();
    void editProduct();
    void deleteProduct();
    void openSelectedPhoto();
    void bulkEdit();
    void bulkPhotos();
    void prefetchVisible();
    void applySearch();
    void onSearchEntered();

private:
    int selectedProductId() const;
    QList<int> selectedProductIds() const;
    void selectProduct(int productId);
    void saveBarcodes(int productId, const QStringList &codes);
    void savePhotos(int productId, const PhotoStore::Changes &changes);

    QLineEdit *m_search;
    QLabel *m_searchStatus;

    QTableView *m_table;
    QSqlTableModel *m_model;
    QList<QWidget *> m_editWidgets; // скрываются в режиме "только просмотр"
    bool m_readOnly = false;
};

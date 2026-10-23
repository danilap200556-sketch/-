#pragma once

#include <QDialog>
#include <QJsonObject>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Выбор категории Маркета из дерева: поиск по названию, выбрать можно только листовую категорию.
class CategoryPickerDialog : public QDialog
{
    Q_OBJECT
public:
    CategoryPickerDialog(const QJsonObject &root, qint64 current, QWidget *parent = nullptr);

    qint64 categoryId() const { return m_id; }
    QString categoryPath() const { return m_path; }
    // Нажали «Обновить список с Маркета»: вызывающий перечитывает дерево и открывает диалог заново.
    bool refreshRequested() const { return m_refresh; }

private:
    bool filterItem(QTreeWidgetItem *item, const QString &text);
    void selectionChanged();

    QLineEdit *m_filter;
    QTreeWidget *m_tree;
    QLabel *m_info;
    QPushButton *m_ok;
    qint64 m_id = 0;
    QString m_path;
    bool m_refresh = false;
};

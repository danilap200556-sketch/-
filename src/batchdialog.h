#pragma once

#include "batchparser.h"
#include "database.h"

#include <QDialog>
#include <QHash>
#include <QPair>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class ProductSuggest;

// Приход, списание или перемещение со склада на склад сразу многих позиций одним документом: сканируйте штрихкоды
// (каждый скан - ещё одна штука), вставляйте список из Excel или буфера, правьте количество в таблице. Всё проводится
// одной транзакцией: если хоть одной позиции не хватает остатка, не меняется ничего.
class BatchDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Kind { Receipt, WriteOff, Transfer };

    explicit BatchDialog(Kind kind, QWidget *parent = nullptr);

    // Добавляет позиции из текста (формат строк - в batchparser.h). Возвращает число строк с ошибками;
    // сами ошибки показываются под таблицей и доступны через problems().
    int addText(const QString &text);
    QStringList problems() const { return m_problems; }

    void setKind(Kind kind);
    void setWarehouse(int warehouseId);
    void setWarehouseTo(int warehouseId); // для перемещения: куда
    void setComment(const QString &comment);

    int rowCount() const;
    qint64 totalQty() const;
    // Проводит документ (без вопросов). При отказе - false, причины в problems() и lastError().
    bool apply();
    bool applied() const { return m_applied; }
    QString lastError() const { return m_lastError; }

    void reject() override; // непроведённый список спрашивает, закрывать ли

private slots:
    void scanEntered();
    void pasteFromClipboard();
    void removeSelectedRows();
    void clearAll();
    void recompute();
    void commit();

private:
    Kind kind() const;
    bool writeOff() const { return kind() == Kind::WriteOff; }
    // Расходная операция: убирает остаток со склада (списание и перемещение) - там нужна проверка наличия.
    bool takesOut() const { return kind() != Kind::Receipt; }
    int warehouseId() const;
    int warehouseToId() const;
    void addQuantity(int productId, qint64 qty);
    // Добавляет выбранный в подсказках товар (qty штук) и показывает его строку.
    void addChosen(int productId, qint64 qty);
    int rowOf(int productId) const;
    void showProblems();
    QList<Database::BatchItem> itemsFromTable() const;

    QComboBox *m_kind = nullptr;
    QComboBox *m_warehouse = nullptr;
    QComboBox *m_warehouseTo = nullptr;
    QLabel *m_warehouseLabel = nullptr;
    QLabel *m_warehouseToLabel = nullptr;
    QLineEdit *m_comment = nullptr;
    QLineEdit *m_scan = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_problemsLabel = nullptr;
    QPushButton *m_commitButton = nullptr;

    QStringList m_problems;
    ProductSuggest *m_suggest = nullptr;
    QString m_lastError;
    bool m_applied = false;
    QHash<int, QPair<QString, QString>> m_info; // id товара -> (артикул, название), читаются пачкой
    bool m_updating = false;
};

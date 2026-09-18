#pragma once

#include <QDialog>

class QComboBox;
class QSpinBox;
class QLineEdit;
class QLabel;

// Диалог для приход/списание/перемещение/инвентаризация.
// Набор видимых полей зависит от Kind.
class MovementDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Kind { Receipt, WriteOff, Transfer, InventoryAdjust };

    explicit MovementDialog(Kind kind, QWidget *parent = nullptr);

    int productId() const;
    int warehouseId() const;      // склад операции (для Transfer - склад-источник)
    int destWarehouseId() const;  // только для Transfer
    int quantity() const;         // для InventoryAdjust - это посчитанное фактическое количество
    QString comment() const;

private slots:
    void updateStockHint();

private:
    Kind m_kind;
    QComboBox *m_product;
    QComboBox *m_warehouse;
    QComboBox *m_destWarehouse = nullptr;
    QSpinBox *m_quantity;
    QLineEdit *m_comment;
    QLabel *m_stockHint;
};

#include "movementdialog.h"
#include "database.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>
#include <QSqlQuery>
#include <QVBoxLayout>

namespace {

void fillProducts(QComboBox *combo)
{
    QSqlQuery q("SELECT id, sku, name FROM products ORDER BY sku");
    while (q.next())
        combo->addItem(QStringLiteral("%1 — %2").arg(q.value(1).toString(), q.value(2).toString()),
                        q.value(0).toInt());
}

void fillWarehouses(QComboBox *combo)
{
    QSqlQuery q("SELECT id, name FROM warehouses ORDER BY name");
    while (q.next())
        combo->addItem(q.value(1).toString(), q.value(0).toInt());
}

} // namespace

MovementDialog::MovementDialog(Kind kind, QWidget *parent)
    : QDialog(parent), m_kind(kind)
{
    QString title;
    switch (kind) {
    case Kind::Receipt: title = tr("Приход товара"); break;
    case Kind::WriteOff: title = tr("Списание товара"); break;
    case Kind::Transfer: title = tr("Перемещение между складами"); break;
    case Kind::InventoryAdjust: title = tr("Инвентаризация (пересчёт)"); break;
    }
    setWindowTitle(title);
    setMinimumWidth(380);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    layout->addLayout(form);

    m_product = new QComboBox(this);
    fillProducts(m_product);
    form->addRow(tr("Товар"), m_product);

    m_warehouse = new QComboBox(this);
    fillWarehouses(m_warehouse);
    form->addRow(kind == Kind::Transfer ? tr("Склад-источник") : tr("Склад"), m_warehouse);

    if (kind == Kind::Transfer) {
        m_destWarehouse = new QComboBox(this);
        fillWarehouses(m_destWarehouse);
        form->addRow(tr("Склад-получатель"), m_destWarehouse);
    }

    m_quantity = new QSpinBox(this);
    m_quantity->setRange(0, 1'000'000);
    form->addRow(kind == Kind::InventoryAdjust ? tr("Посчитано (факт)") : tr("Количество"), m_quantity);

    m_comment = new QLineEdit(this);
    form->addRow(tr("Комментарий"), m_comment);

    m_stockHint = new QLabel(this);
    m_stockHint->setStyleSheet("color: gray; font-size: 11px;");
    layout->addWidget(m_stockHint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (m_product->count() == 0 || m_warehouse->count() == 0) {
            QMessageBox::warning(this, tr("Нет данных"), tr("Сначала добавьте товары и склады."));
            return;
        }
        if (m_kind == Kind::Transfer && m_warehouse->currentData() == m_destWarehouse->currentData()) {
            QMessageBox::warning(this, tr("Ошибка"), tr("Склад-источник и склад-получатель должны различаться."));
            return;
        }
        if (m_kind != Kind::InventoryAdjust && m_quantity->value() <= 0) {
            QMessageBox::warning(this, tr("Ошибка"), tr("Количество должно быть больше нуля."));
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_product, &QComboBox::currentIndexChanged, this, &MovementDialog::updateStockHint);
    connect(m_warehouse, &QComboBox::currentIndexChanged, this, &MovementDialog::updateStockHint);
    updateStockHint();
}

void MovementDialog::updateStockHint()
{
    if (m_product->count() == 0 || m_warehouse->count() == 0) {
        m_stockHint->setText(tr("Добавьте хотя бы один товар и один склад."));
        return;
    }
    const int stock = Database::currentStock(m_product->currentData().toInt(), m_warehouse->currentData().toInt());
    m_stockHint->setText(tr("Текущий остаток на складе: %1 шт.").arg(stock));
}

int MovementDialog::productId() const { return m_product->currentData().toInt(); }
int MovementDialog::warehouseId() const { return m_warehouse->currentData().toInt(); }
int MovementDialog::destWarehouseId() const { return m_destWarehouse ? m_destWarehouse->currentData().toInt() : -1; }
int MovementDialog::quantity() const { return m_quantity->value(); }
QString MovementDialog::comment() const { return m_comment->text(); }

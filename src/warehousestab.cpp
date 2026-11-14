#include "warehousestab.h"
#include "formdialog.h"
#include "stockbar.h"
#include "markingowner.h"
#include "markingownersdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QSqlTableModel>
#include <QTableView>
#include <QVBoxLayout>

namespace {
// Колонки справа считаются запросом: ИП склада (для кодов «Честного знака»),, а не хранятся: сколько пар лежит на складе и по скольким артикулам (размерам) есть остаток.
enum WCol { WColId = 0, WColName, WColAddress, WColOwner, WColPairs, WColPositions };
enum LCol { LColId = 0, LColWarehouseId, LColCode, LColDescription };

class WarehousesModel : public QSqlTableModel
{
public:
    using QSqlTableModel::QSqlTableModel;

    QVariant data(const QModelIndex &idx, int role) const override
    {
        if (idx.isValid() && idx.column() == WColOwner && role == Qt::DisplayRole) {
            const QString name = QSqlTableModel::data(idx, Qt::DisplayRole).toString();
            return name.isEmpty() ? QStringLiteral("-") : name;
        }
        if (idx.isValid() && (idx.column() == WColPairs || idx.column() == WColPositions)) {
            if (role == Qt::TextAlignmentRole)
                return int(Qt::AlignRight | Qt::AlignVCenter);
            if (role == Qt::DisplayRole)
                return StockBar::groupDigits(QSqlTableModel::data(idx, Qt::DisplayRole).toLongLong());
            if (role == Qt::FontRole && idx.column() == WColPairs) {
                QFont f;
                f.setBold(true);
                return f;
            }
        }
        return QSqlTableModel::data(idx, role);
    }

protected:
    QString selectStatement() const override
    {
        QString s = QSqlTableModel::selectStatement();
        const int from = s.indexOf(QLatin1String(" FROM "));
        if (from > 0)
            s = s.left(from)
                + QLatin1String(", (SELECT o.name FROM marking_warehouse_owner w JOIN marking_owners o ON o.id = w.owner_id WHERE w.warehouse_id = \"warehouses\".\"id\") AS \"owner_name\""
                                ", (SELECT COALESCE(SUM(quantity), 0)::bigint FROM stock WHERE warehouse_id = \"warehouses\".\"id\") AS \"pairs\""
                                ", (SELECT COUNT(*)::bigint FROM stock WHERE warehouse_id = \"warehouses\".\"id\" AND quantity > 0) AS \"positions\"")
                + s.mid(from);
        return s;
    }
};

// Склад: название, адрес и ИП, которому он принадлежит (чьи коды «Честного знака» нужны под его остатки).
class WarehouseDialog : public QDialog
{
public:
    WarehouseDialog(const QString &title, const QString &name, const QString &address, int ownerId, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(title);
        auto *l = new QVBoxLayout(this);
        auto *form = new QFormLayout();
        m_name = new QLineEdit(name, this);
        m_address = new QLineEdit(address, this);
        m_owner = new QComboBox(this);
        m_owner->addItem(tr("не задан"), 0);
        for (const MarkingOwner::Owner &o : MarkingOwner::list())
            m_owner->addItem(o.name, o.id);
        if (const int i = m_owner->findData(ownerId); i >= 0)
            m_owner->setCurrentIndex(i);
        m_owner->setToolTip(tr("ИП, которому принадлежат товары на этом складе. Заказ кодов по остаткам склада и печать к заказам Маркета берут коды этого ИП. "
                               "Список ИП - кнопка «ИП…» на этой вкладке или на вкладке «Честный знак»."));
        form->addRow(tr("Название"), m_name);
        form->addRow(tr("Адрес"), m_address);
        form->addRow(tr("ИП (Честный знак)"), m_owner);
        l->addLayout(form);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        l->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        resize(440, 170);
    }
    QString name() const { return m_name->text(); }
    QString address() const { return m_address->text(); }
    int ownerId() const { return m_owner->currentData().toInt(); }

private:
    QLineEdit *m_name, *m_address;
    QComboBox *m_owner;
};
}

WarehousesTab::WarehousesTab(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *splitter = new QSplitter(this);
    layout->addWidget(splitter);

    // --- Левая панель: склады ---
    auto *warehouseBox = new QGroupBox(tr("Склады"), this);
    auto *warehouseLayout = new QVBoxLayout(warehouseBox);
    auto *warehouseToolbar = new QHBoxLayout();
    auto *addWhBtn = new QPushButton(tr("Добавить"), warehouseBox);
    auto *editWhBtn = new QPushButton(tr("Изменить"), warehouseBox);
    auto *delWhBtn = new QPushButton(tr("Удалить"), warehouseBox);
    warehouseToolbar->addWidget(addWhBtn);
    warehouseToolbar->addWidget(editWhBtn);
    warehouseToolbar->addWidget(delWhBtn);
    auto *ownersBtn = new QPushButton(tr("ИП…"), warehouseBox);
    ownersBtn->setToolTip(tr("Справочник ИП для кодов «Честного знака»: название и ИНН. Какому ИП принадлежит склад - в окне «Изменить»."));
    warehouseToolbar->addWidget(ownersBtn);
    warehouseToolbar->addStretch();
    m_editWidgets << addWhBtn << editWhBtn << delWhBtn << ownersBtn;
    warehouseLayout->addLayout(warehouseToolbar);

    m_warehouseModel = new WarehousesModel(this);
    m_warehouseModel->setEditStrategy(QSqlTableModel::OnManualSubmit);
    m_warehouseModel->setSort(WColName, Qt::AscendingOrder);

    m_warehouseTable = new QTableView(warehouseBox);
    m_warehouseTable->setModel(m_warehouseModel);
    m_warehouseTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_warehouseTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_warehouseTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_warehouseTable->horizontalHeader()->setStretchLastSection(true);
    warehouseLayout->addWidget(m_warehouseTable);
    splitter->addWidget(warehouseBox);

    // --- Правая панель: места хранения выбранного склада ---
    auto *locationBox = new QGroupBox(tr("Места хранения (полки/зоны)"), this);
    auto *locationLayout = new QVBoxLayout(locationBox);
    auto *locationToolbar = new QHBoxLayout();
    auto *addLocBtn = new QPushButton(tr("Добавить"), locationBox);
    auto *editLocBtn = new QPushButton(tr("Изменить"), locationBox);
    auto *delLocBtn = new QPushButton(tr("Удалить"), locationBox);
    locationToolbar->addWidget(addLocBtn);
    locationToolbar->addWidget(editLocBtn);
    locationToolbar->addWidget(delLocBtn);
    locationToolbar->addStretch();
    m_editWidgets << addLocBtn << editLocBtn << delLocBtn;
    locationLayout->addLayout(locationToolbar);

    m_locationModel = new QSqlTableModel(this);
    m_locationModel->setEditStrategy(QSqlTableModel::OnManualSubmit);
    m_locationModel->setSort(LColCode, Qt::AscendingOrder);

    m_locationTable = new QTableView(locationBox);
    m_locationTable->setModel(m_locationModel);
    m_locationTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_locationTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_locationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_locationTable->horizontalHeader()->setStretchLastSection(true);
    locationLayout->addWidget(m_locationTable);
    splitter->addWidget(locationBox);


    connect(ownersBtn, &QPushButton::clicked, this, [this]() {
        MarkingOwnersDialog dlg(this, false);
        dlg.exec();
        refresh();
    });
    connect(addWhBtn, &QPushButton::clicked, this, &WarehousesTab::addWarehouse);
    connect(editWhBtn, &QPushButton::clicked, this, &WarehousesTab::editWarehouse);
    connect(delWhBtn, &QPushButton::clicked, this, &WarehousesTab::deleteWarehouse);
    connect(addLocBtn, &QPushButton::clicked, this, &WarehousesTab::addLocation);
    connect(editLocBtn, &QPushButton::clicked, this, &WarehousesTab::editLocation);
    connect(delLocBtn, &QPushButton::clicked, this, &WarehousesTab::deleteLocation);
    connect(m_warehouseTable->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &WarehousesTab::onWarehouseSelectionChanged);
}

void WarehousesTab::setReadOnly(bool readOnly)
{
    for (QWidget *w : m_editWidgets)
        w->setVisible(!readOnly);
}

void WarehousesTab::refresh()
{
    // setTable() сам читает у базы описание таблицы (два запроса на каждую), поэтому откладываем его до
    // первого показа страницы: пока на неё не зашли, базу не трогаем.
    if (m_warehouseModel->tableName().isEmpty())
        m_warehouseModel->setTable("warehouses");
    if (m_locationModel->tableName().isEmpty())
        m_locationModel->setTable("locations");
    m_warehouseModel->select();
    m_warehouseModel->setHeaderData(WColName, Qt::Horizontal, tr("Название"));
    m_warehouseModel->setHeaderData(WColAddress, Qt::Horizontal, tr("Адрес"));
    m_warehouseModel->setHeaderData(WColOwner, Qt::Horizontal, tr("ИП (Честный знак)"));
    m_warehouseModel->setHeaderData(WColPairs, Qt::Horizontal, tr("Пар на складе"));
    m_warehouseModel->setHeaderData(WColPositions, Qt::Horizontal, tr("Артикулов в наличии"));
    m_warehouseTable->setColumnHidden(WColId, true);
    // Название и адрес делят свободное место, числа - по содержимому (заголовки «Пар на складе» и «Артикулов в наличии» целиком).
    QHeaderView *wh = m_warehouseTable->horizontalHeader();
    wh->setStretchLastSection(false);
    wh->setSectionResizeMode(WColName, QHeaderView::Stretch);
    wh->setSectionResizeMode(WColAddress, QHeaderView::Stretch);
    wh->setSectionResizeMode(WColOwner, QHeaderView::ResizeToContents);
    wh->setSectionResizeMode(WColPairs, QHeaderView::ResizeToContents);
    wh->setSectionResizeMode(WColPositions, QHeaderView::ResizeToContents);
    refreshLocations();
}

int WarehousesTab::selectedWarehouseId() const
{
    const auto sel = m_warehouseTable->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return -1;
    return m_warehouseModel->record(sel.first().row()).value("id").toInt();
}

void WarehousesTab::refreshLocations()
{
    const int whId = selectedWarehouseId();
    if (whId < 0) {
        m_locationModel->setFilter("0 = 1");
    } else {
        m_locationModel->setFilter(QStringLiteral("warehouse_id = %1").arg(whId));
    }
    m_locationModel->select();
    m_locationModel->setHeaderData(LColCode, Qt::Horizontal, tr("Код"));
    m_locationModel->setHeaderData(LColDescription, Qt::Horizontal, tr("Описание"));
    m_locationTable->setColumnHidden(LColId, true);
    m_locationTable->setColumnHidden(LColWarehouseId, true);
}

void WarehousesTab::onWarehouseSelectionChanged()
{
    refreshLocations();
}

void WarehousesTab::addWarehouse()
{
    WarehouseDialog dlg(tr("Новый склад"), QString(), QString(), 0, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString name = dlg.name().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Название склада обязательно."));
        return;
    }

    QSqlRecord rec = m_warehouseModel->record();
    rec.setValue("name", name);
    rec.setValue("address", dlg.address());
    rec.remove(rec.indexOf("id"));
    if (!m_warehouseModel->insertRecord(-1, rec) || !m_warehouseModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_warehouseModel->lastError().text());
        m_warehouseModel->revertAll();
        return;
    }
    if (dlg.ownerId() > 0) {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT id FROM warehouses WHERE name = ?"));
        q.addBindValue(name);
        QString err;
        if (q.exec() && q.next() && !MarkingOwner::setWarehouseOwner(q.value(0).toInt(), dlg.ownerId(), &err))
            QMessageBox::warning(this, tr("ИП склада"), tr("Склад добавлен, но ИП не записан: %1").arg(err));
    }
    refresh();
    emit warehousesChanged();
}

void WarehousesTab::editWarehouse()
{
    const auto sel = m_warehouseTable->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    const int row = sel.first().row();
    const QSqlRecord rec = m_warehouseModel->record(row);
    const int whId = rec.value("id").toInt();

    WarehouseDialog dlg(tr("Изменить склад"), rec.value("name").toString(), rec.value("address").toString(), MarkingOwner::warehouseOwner(whId), this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    m_warehouseModel->setData(m_warehouseModel->index(row, WColName), dlg.name().trimmed());
    m_warehouseModel->setData(m_warehouseModel->index(row, WColAddress), dlg.address());
    if (!m_warehouseModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_warehouseModel->lastError().text());
        m_warehouseModel->revertAll();
        return;
    }
    QString err;
    if (!MarkingOwner::setWarehouseOwner(whId, dlg.ownerId(), &err))
        QMessageBox::warning(this, tr("ИП склада"), tr("ИП не записан: %1").arg(err));
    refresh();
    emit warehousesChanged();
}

void WarehousesTab::deleteWarehouse()
{
    const auto sel = m_warehouseTable->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Удалить склад"),
                               tr("Удалить склад со всеми его местами хранения, остатками и историей движений?"))
        != QMessageBox::Yes)
        return;
    m_warehouseModel->removeRow(sel.first().row());
    if (!m_warehouseModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_warehouseModel->lastError().text());
        m_warehouseModel->revertAll();
        return;
    }
    refresh();
    emit warehousesChanged();
}

void WarehousesTab::addLocation()
{
    const int whId = selectedWarehouseId();
    if (whId < 0) {
        QMessageBox::information(this, tr("Выберите склад"), tr("Сначала выберите склад слева."));
        return;
    }
    FormDialog dlg(tr("Новое место хранения"),
                    {{tr("Код (напр. A1-03)"), "", false}, {tr("Описание"), "", false}}, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const auto v = dlg.values();
    if (v[0].trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Код места хранения обязателен."));
        return;
    }

    QSqlRecord rec = m_locationModel->record();
    rec.setValue("warehouse_id", whId);
    rec.setValue("code", v[0].trimmed());
    rec.setValue("description", v[1]);
    rec.remove(rec.indexOf("id"));
    if (!m_locationModel->insertRecord(-1, rec) || !m_locationModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_locationModel->lastError().text());
        m_locationModel->revertAll();
        return;
    }
    refreshLocations();
}

void WarehousesTab::editLocation()
{
    const auto sel = m_locationTable->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    const int row = sel.first().row();
    const QSqlRecord rec = m_locationModel->record(row);

    FormDialog dlg(tr("Изменить место хранения"),
                    {{tr("Код"), rec.value("code").toString(), false},
                     {tr("Описание"), rec.value("description").toString(), false}},
                    this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const auto v = dlg.values();

    m_locationModel->setData(m_locationModel->index(row, LColCode), v[0].trimmed());
    m_locationModel->setData(m_locationModel->index(row, LColDescription), v[1]);
    if (!m_locationModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_locationModel->lastError().text());
        m_locationModel->revertAll();
        return;
    }
    refreshLocations();
}

void WarehousesTab::deleteLocation()
{
    const auto sel = m_locationTable->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Удалить место хранения"), tr("Удалить выбранное место хранения?"))
        != QMessageBox::Yes)
        return;
    m_locationModel->removeRow(sel.first().row());
    if (!m_locationModel->submitAll()) {
        QMessageBox::warning(this, tr("Ошибка"), m_locationModel->lastError().text());
        m_locationModel->revertAll();
        return;
    }
    refreshLocations();
}

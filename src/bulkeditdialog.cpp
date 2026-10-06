#include "bulkeditdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVBoxLayout>
#include <cmath>

namespace BulkEdit {

double newPrice(double oldPrice, const Spec &spec)
{
    double p = oldPrice;
    switch (spec.priceMode) {
    case Spec::PriceSet: p = spec.priceValue; break;
    case Spec::PricePercent: p = oldPrice * (1.0 + spec.priceValue / 100.0); break;
    case Spec::PriceAdd: p = oldPrice + spec.priceValue; break;
    }
    if (spec.roundPrice)
        p = std::round(p);
    else
        p = std::round(p * 100.0) / 100.0;
    return p < 0 ? 0 : p;
}

QString newName(const QString &oldName, const Spec &spec)
{
    switch (spec.nameMode) {
    case Spec::NamePrefix: return spec.nameA + oldName;
    case Spec::NameSuffix: return oldName + spec.nameA;
    case Spec::NameReplace: return spec.nameA.isEmpty() ? oldName : QString(oldName).replace(spec.nameA, spec.nameB);
    }
    return oldName;
}

QString describe(const Spec &spec)
{
    QStringList parts;
    if (spec.changePrice) {
        const QString v = QString::number(spec.priceValue, 'f', 2);
        switch (spec.priceMode) {
        case Spec::PriceSet: parts << QStringLiteral("цена = %1 ₽").arg(v); break;
        case Spec::PricePercent: parts << QStringLiteral("цена %1%2%").arg(spec.priceValue >= 0 ? "+" : "").arg(v); break;
        case Spec::PriceAdd: parts << QStringLiteral("цена %1%2 ₽").arg(spec.priceValue >= 0 ? "+" : "").arg(v); break;
        }
    }
    if (spec.changeName)
        parts << QStringLiteral("название");
    if (spec.changeDescription)
        parts << QStringLiteral("описание");
    if (spec.changeMarketSku)
        parts << (spec.marketSku.isEmpty() ? QStringLiteral("артикул на Маркете: очистить")
                                           : QStringLiteral("артикул на Маркете = %1").arg(spec.marketSku));
    if (spec.changeLocation)
        parts << QStringLiteral("место хранения");
    return parts.join(QStringLiteral(", "));
}

bool apply(const QList<int> &productIds, const Spec &spec, QString *error)
{
    if (productIds.isEmpty() || !spec.any())
        return true;
    QSqlDatabase db = QSqlDatabase::database();
    if (!db.transaction()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    auto fail = [&](const QSqlQuery &q) {
        db.rollback();
        if (error)
            *error = q.lastError().text();
        return false;
    };

    constexpr int kChunk = 400;
    for (int start = 0; start < productIds.size(); start += kChunk) {
        const QList<int> chunk = productIds.mid(start, kChunk);
        QStringList marks;
        for (int i = 0; i < chunk.size(); ++i)
            marks << QStringLiteral("?");
        const QString in = marks.join(',');
        auto bindIds = [&](QSqlQuery &q) {
            for (int id : chunk)
                q.addBindValue(id);
        };

        if (spec.changePrice) {
            QSqlQuery q;
            const QString rnd = spec.roundPrice ? QStringLiteral("0") : QStringLiteral("2");
            switch (spec.priceMode) {
            case Spec::PriceSet:
                q.prepare(QStringLiteral("UPDATE products SET price = ROUND(GREATEST(0, ?::numeric), %1) WHERE id IN (%2)").arg(rnd, in));
                break;
            case Spec::PricePercent:
                q.prepare(QStringLiteral("UPDATE products SET price = ROUND(GREATEST(0, price::numeric * (1 + ?::numeric / 100)), %1) "
                                         "WHERE id IN (%2)").arg(rnd, in));
                break;
            case Spec::PriceAdd:
                q.prepare(QStringLiteral("UPDATE products SET price = ROUND(GREATEST(0, price::numeric + ?::numeric), %1) "
                                         "WHERE id IN (%2)").arg(rnd, in));
                break;
            }
            q.addBindValue(QString::number(spec.priceValue, 'f', 4));
            bindIds(q);
            if (!q.exec())
                return fail(q);
        }
        if (spec.changeName) {
            QSqlQuery q;
            switch (spec.nameMode) {
            case Spec::NamePrefix:
                q.prepare(QStringLiteral("UPDATE products SET name = ? || name WHERE id IN (%1)").arg(in));
                q.addBindValue(spec.nameA);
                break;
            case Spec::NameSuffix:
                q.prepare(QStringLiteral("UPDATE products SET name = name || ? WHERE id IN (%1)").arg(in));
                q.addBindValue(spec.nameA);
                break;
            case Spec::NameReplace:
                q.prepare(QStringLiteral("UPDATE products SET name = replace(name, ?, ?) WHERE id IN (%1)").arg(in));
                q.addBindValue(spec.nameA);
                q.addBindValue(spec.nameB);
                break;
            }
            bindIds(q);
            if (!q.exec())
                return fail(q);
        }
        if (spec.changeDescription) {
            QSqlQuery q;
            q.prepare(QStringLiteral("UPDATE products SET description = ? WHERE id IN (%1)").arg(in));
            q.addBindValue(spec.description);
            bindIds(q);
            if (!q.exec())
                return fail(q);
        }
        if (spec.changeMarketSku) {
            QSqlQuery q;
            q.prepare(QStringLiteral("UPDATE products SET market_sku = ? WHERE id IN (%1)").arg(in));
            q.addBindValue(spec.marketSku);
            bindIds(q);
            if (!q.exec())
                return fail(q);
        }
        if (spec.changeLocation) {
            QSqlQuery q;
            q.prepare(QStringLiteral("INSERT INTO stock (product_id, warehouse_id, location_id, quantity) "
                                     "SELECT id, ?::int, ?::int, 0 FROM products WHERE id IN (%1) "
                                     "ON CONFLICT (product_id, warehouse_id) DO UPDATE SET location_id = EXCLUDED.location_id")
                          .arg(in));
            q.addBindValue(spec.warehouseId);
            q.addBindValue(spec.locationId > 0 ? QVariant(spec.locationId) : QVariant(QMetaType(QMetaType::Int)));
            bindIds(q);
            if (!q.exec())
                return fail(q);
        }
    }
    if (!db.commit()) {
        db.rollback();
        if (error)
            *error = db.lastError().text();
        return false;
    }
    return true;
}

} // namespace BulkEdit

// ---------------------------------------------------------------------------

BulkEditDialog::BulkEditDialog(const QList<int> &productIds, QWidget *parent)
    : QDialog(parent), m_ids(productIds)
{
    setWindowTitle(tr("Массовое редактирование"));
    setMinimumWidth(560);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Выбрано товаров: <b>%1</b>. Отметьте, что нужно изменить - остальное останется как есть.")
                                     .arg(m_ids.size()), this));

    // Пример для предпросмотра - первый выбранный товар.
    {
        QSqlQuery q;
        q.prepare("SELECT sku, name, price FROM products WHERE id = ?");
        q.addBindValue(m_ids.value(0));
        if (q.exec() && q.next()) {
            m_sampleSku = q.value(0).toString();
            m_sampleName = q.value(1).toString();
            m_samplePrice = q.value(2).toDouble();
        }
    }

    auto addGroup = [&](QCheckBox *&check, const QString &title) {
        auto *box = new QGroupBox(this);
        auto *vl = new QVBoxLayout(box);
        check = new QCheckBox(title, box);
        vl->addWidget(check);
        layout->addWidget(box);
        return vl;
    };

    // --- Цена ---
    {
        auto *vl = addGroup(m_priceOn, tr("Цена"));
        auto *row = new QHBoxLayout();
        m_priceMode = new QComboBox(this);
        m_priceMode->addItem(tr("Задать цену"), BulkEdit::Spec::PriceSet);
        m_priceMode->addItem(tr("Изменить на, % (минус - скидка)"), BulkEdit::Spec::PricePercent);
        m_priceMode->addItem(tr("Прибавить, ₽ (минус - убавить)"), BulkEdit::Spec::PriceAdd);
        m_priceValue = new QDoubleSpinBox(this);
        m_priceValue->setRange(-10'000'000, 10'000'000);
        m_priceValue->setDecimals(2);
        row->addWidget(m_priceMode, 1);
        row->addWidget(m_priceValue);
        vl->addLayout(row);
        m_priceRound = new QCheckBox(tr("Округлять до целых рублей"), this);
        vl->addWidget(m_priceRound);
    }
    // --- Название ---
    {
        auto *vl = addGroup(m_nameOn, tr("Название"));
        m_nameMode = new QComboBox(this);
        m_nameMode->addItem(tr("Добавить в начало"), BulkEdit::Spec::NamePrefix);
        m_nameMode->addItem(tr("Добавить в конец"), BulkEdit::Spec::NameSuffix);
        m_nameMode->addItem(tr("Найти и заменить"), BulkEdit::Spec::NameReplace);
        m_nameA = new QLineEdit(this);
        m_nameB = new QLineEdit(this);
        m_nameB->setPlaceholderText(tr("заменить на (пусто - удалить найденное)"));
        vl->addWidget(m_nameMode);
        vl->addWidget(m_nameA);
        vl->addWidget(m_nameB);
    }
    // --- Описание ---
    {
        auto *vl = addGroup(m_descOn, tr("Описание (заменить у всех)"));
        m_desc = new QLineEdit(this);
        vl->addWidget(m_desc);
    }
    // --- Артикул на Маркете ---
    {
        auto *vl = addGroup(m_marketOn, tr("Артикул на Маркете"));
        m_market = new QLineEdit(this);
        m_market->setPlaceholderText(tr("пусто - очистить (на Маркете будет использоваться наш артикул)"));
        vl->addWidget(m_market);
    }
    // --- Место хранения ---
    {
        auto *vl = addGroup(m_locOn, tr("Место хранения на складе"));
        auto *row = new QHBoxLayout();
        m_warehouse = new QComboBox(this);
        m_location = new QComboBox(this);
        QSqlQuery q("SELECT id, name FROM warehouses ORDER BY name");
        while (q.next())
            m_warehouse->addItem(q.value(1).toString(), q.value(0).toInt());
        row->addWidget(m_warehouse, 1);
        row->addWidget(m_location, 1);
        vl->addLayout(row);
        if (m_warehouse->count() == 0)
            m_locOn->setEnabled(false);
        onWarehouseChanged();
    }

    m_preview = new QLabel(this);
    m_preview->setWordWrap(true);
    m_preview->setStyleSheet("color: gray;");
    layout->addWidget(m_preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Применить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Отмена"));
    connect(buttons, &QDialogButtonBox::accepted, this, &BulkEditDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    for (auto *c : {m_priceOn, m_priceRound, m_nameOn, m_descOn, m_marketOn, m_locOn})
        connect(c, &QCheckBox::toggled, this, &BulkEditDialog::updatePreview);
    for (auto *c : {m_priceMode, m_nameMode, m_warehouse, m_location})
        connect(c, &QComboBox::currentIndexChanged, this, &BulkEditDialog::updatePreview);
    for (auto *e : {m_nameA, m_nameB, m_desc, m_market})
        connect(e, &QLineEdit::textChanged, this, &BulkEditDialog::updatePreview);
    connect(m_priceValue, &QDoubleSpinBox::valueChanged, this, &BulkEditDialog::updatePreview);
    connect(m_warehouse, &QComboBox::currentIndexChanged, this, &BulkEditDialog::onWarehouseChanged);
    updatePreview();
}

void BulkEditDialog::onWarehouseChanged()
{
    m_location->clear();
    m_location->addItem(tr("— не указано —"), 0);
    QSqlQuery q;
    q.prepare("SELECT id, code FROM locations WHERE warehouse_id = ? ORDER BY code");
    q.addBindValue(m_warehouse->currentData().toInt());
    if (q.exec())
        while (q.next())
            m_location->addItem(q.value(1).toString(), q.value(0).toInt());
}

BulkEdit::Spec BulkEditDialog::spec() const
{
    BulkEdit::Spec s;
    s.changePrice = m_priceOn->isChecked();
    s.priceMode = BulkEdit::Spec::PriceMode(m_priceMode->currentData().toInt());
    s.priceValue = m_priceValue->value();
    s.roundPrice = m_priceRound->isChecked();
    s.changeName = m_nameOn->isChecked();
    s.nameMode = BulkEdit::Spec::NameMode(m_nameMode->currentData().toInt());
    s.nameA = m_nameA->text();
    s.nameB = m_nameB->text();
    s.changeDescription = m_descOn->isChecked();
    s.description = m_desc->text();
    s.changeMarketSku = m_marketOn->isChecked();
    s.marketSku = m_market->text().trimmed();
    s.changeLocation = m_locOn->isChecked();
    s.warehouseId = m_warehouse->currentData().toInt();
    s.locationId = m_location->currentData().toInt();
    return s;
}

void BulkEditDialog::updatePreview()
{
    const auto s = spec();
    m_nameB->setEnabled(s.nameMode == BulkEdit::Spec::NameReplace);
    if (!s.any()) {
        m_preview->setText(tr("Ничего не выбрано."));
        return;
    }
    QStringList lines;
    if (s.changePrice)
        lines << tr("цена: %1 → %2").arg(m_samplePrice, 0, 'f', 2).arg(BulkEdit::newPrice(m_samplePrice, s), 0, 'f', 2);
    if (s.changeName)
        lines << tr("название: «%1» → «%2»").arg(m_sampleName, BulkEdit::newName(m_sampleName, s));
    m_preview->setText(tr("Пример на товаре %1:\n%2").arg(m_sampleSku, lines.isEmpty() ? tr("(остальные поля меняются так, как введено)") : lines.join('\n')));
}

void BulkEditDialog::accept()
{
    const auto s = spec();
    if (!s.any()) {
        QMessageBox::information(this, tr("Нечего менять"), tr("Отметьте хотя бы одно поле."));
        return;
    }
    if (s.changeName && s.nameMode == BulkEdit::Spec::NameReplace && s.nameA.isEmpty()) {
        QMessageBox::warning(this, tr("Проверьте поля"), tr("Укажите, что нужно найти в названии."));
        return;
    }
    if (s.changeName && s.nameMode != BulkEdit::Spec::NameReplace && s.nameA.isEmpty()) {
        QMessageBox::warning(this, tr("Проверьте поля"), tr("Введите текст, который нужно добавить к названию."));
        return;
    }
    if (QMessageBox::question(this, tr("Применить изменения"),
                              tr("Изменить %1 товаров?\n\n%2\n\nОтменить это действие будет нельзя.")
                                  .arg(m_ids.size()).arg(BulkEdit::describe(s)))
        != QMessageBox::Yes)
        return;
    QString err;
    if (!BulkEdit::apply(m_ids, s, &err)) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Изменения не применены:\n%1").arg(err));
        return;
    }
    QDialog::accept();
}

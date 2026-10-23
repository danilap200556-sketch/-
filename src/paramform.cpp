#include "paramform.h"

#include <QComboBox>
#include <QCompleter>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QVBoxLayout>

ParamForm::ParamForm(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_form = new QFormLayout();
    layout->addLayout(m_form);
}

void ParamForm::clear()
{
    while (m_form->rowCount() > 0)
        m_form->removeRow(0);
    m_editors.clear();
}

void ParamForm::setParams(const QList<MarketCategoryParam> &params, const QMap<qint64, QList<MarketParamValue>> &values)
{
    clear();
    for (const MarketCategoryParam &p : params) {
        Editor e = makeEditor(p);
        QString title = p.name + (p.required ? QStringLiteral(" *") : QString());
        auto *label = new QLabel(title, this);
        if (p.required) {
            QFont f = label->font();
            f.setBold(true);
            label->setFont(f);
        }
        QStringList tip;
        if (!p.description.isEmpty())
            tip << p.description;
        if (p.distinctive)
            tip << tr("Особенность варианта (по ней различаются товары одной модели).");
        if (p.hasMin || p.hasMax)
            tip << tr("Допустимо: от %1 до %2").arg(p.hasMin ? QString::number(p.minValue) : QStringLiteral("…"), p.hasMax ? QString::number(p.maxValue) : QStringLiteral("…"));
        if (p.maxLength > 0)
            tip << tr("Не длиннее %1 символов").arg(p.maxLength);
        label->setToolTip(tip.join(QLatin1Char('\n')));
        e.widget->setToolTip(label->toolTip());
        m_form->addRow(label, e.widget);
        e.set(values.value(p.id));
        m_editors.append(e);
    }
}

QMap<qint64, QList<MarketParamValue>> ParamForm::values() const
{
    QMap<qint64, QList<MarketParamValue>> result;
    for (const Editor &e : m_editors) {
        const QList<MarketParamValue> v = e.get();
        if (!v.isEmpty())
            result.insert(e.param.id, v);
    }
    return result;
}

ParamForm::Editor ParamForm::makeEditor(const MarketCategoryParam &p)
{
    Editor e;
    e.param = p;
    auto dirty = [this]() { emit changed(); };

    if (p.type == QLatin1String("ENUM") && p.multivalue) {
        auto *w = new QWidget(this);
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        auto *list = new QListWidget(w);
        list->setFixedHeight(qBound(48, 12 + int(p.values.size()) * 24, 150));
        for (const auto &o : p.values) {
            auto *it = new QListWidgetItem(o.value, list);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(Qt::Unchecked);
            it->setData(Qt::UserRole, o.id);
        }
        l->addWidget(list);
        QLineEdit *custom = nullptr;
        if (p.allowCustomValues) {
            custom = new QLineEdit(w);
            custom->setPlaceholderText(tr("свои значения через «;»"));
            l->addWidget(custom);
            connect(custom, &QLineEdit::textChanged, this, dirty);
        }
        connect(list, &QListWidget::itemChanged, this, dirty);
        e.widget = w;
        e.get = [list, custom]() {
            QList<MarketParamValue> v;
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->checkState() == Qt::Checked)
                    v.append({list->item(i)->text(), list->item(i)->data(Qt::UserRole).toLongLong(), 0});
            if (custom)
                for (const QString &part : MarketCards::splitList(custom->text()))
                    v.append({part, 0, 0});
            return v;
        };
        e.set = [list, custom](const QList<MarketParamValue> &vals) {
            const QSignalBlocker b(list);
            QStringList extra;
            for (int i = 0; i < list->count(); ++i)
                list->item(i)->setCheckState(Qt::Unchecked);
            for (const MarketParamValue &v : vals) {
                bool found = false;
                for (int i = 0; i < list->count() && !found; ++i) {
                    if ((v.valueId > 0 && list->item(i)->data(Qt::UserRole).toLongLong() == v.valueId) ||
                        (v.valueId <= 0 && list->item(i)->text() == v.value)) {
                        list->item(i)->setCheckState(Qt::Checked);
                        found = true;
                    }
                }
                if (!found)
                    extra << v.value;
            }
            if (custom)
                custom->setText(extra.join(QStringLiteral("; ")));
        };
    } else if (p.type == QLatin1String("ENUM")) {
        auto *combo = new QComboBox(this);
        combo->setEditable(p.allowCustomValues);
        combo->addItem(QString(), 0);
        for (const auto &o : p.values)
            combo->addItem(o.value, o.id);
        if (p.allowCustomValues) {
            combo->setInsertPolicy(QComboBox::NoInsert);
            if (combo->completer()) {
                combo->completer()->setFilterMode(Qt::MatchContains);
                combo->completer()->setCaseSensitivity(Qt::CaseInsensitive);
            }
        }
        combo->setCurrentIndex(0);
        connect(combo, &QComboBox::currentTextChanged, this, dirty);
        e.widget = combo;
        e.get = [combo]() {
            QList<MarketParamValue> v;
            const QString text = combo->currentText().trimmed();
            if (text.isEmpty())
                return v;
            const int idx = combo->findText(text, Qt::MatchFixedString);
            if (idx > 0)
                v.append({combo->itemText(idx), combo->itemData(idx).toLongLong(), 0});
            else if (combo->isEditable())
                v.append({text, 0, 0});
            return v;
        };
        e.set = [combo](const QList<MarketParamValue> &vals) {
            const QSignalBlocker b(combo);
            combo->setCurrentIndex(0);
            if (combo->isEditable())
                combo->setEditText(QString());
            if (vals.isEmpty())
                return;
            int idx = vals[0].valueId > 0 ? combo->findData(vals[0].valueId) : -1;
            if (idx < 0)
                idx = combo->findText(vals[0].value, Qt::MatchFixedString);
            if (idx > 0)
                combo->setCurrentIndex(idx);
            else if (combo->isEditable())
                combo->setEditText(vals[0].value);
        };
    } else if (p.type == QLatin1String("NUMERIC")) {
        auto *w = new QWidget(this);
        auto *l = new QHBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        auto *edit = new QLineEdit(w);
        edit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("-?[0-9]*[.,]?[0-9]*")), edit));
        l->addWidget(edit, 1);
        QComboBox *units = nullptr;
        if (!p.units.isEmpty()) {
            units = new QComboBox(w);
            for (const auto &u : p.units)
                units->addItem(u.name, u.id);
            const int d = units->findData(p.defaultUnitId);
            units->setCurrentIndex(d >= 0 ? d : 0);
            l->addWidget(units);
            connect(units, &QComboBox::currentIndexChanged, this, dirty);
        }
        connect(edit, &QLineEdit::textChanged, this, dirty);
        e.widget = w;
        const qint64 defaultUnit = p.defaultUnitId;
        e.get = [edit, units, defaultUnit]() {
            QList<MarketParamValue> v;
            QString t = edit->text().trimmed();
            t.replace(QLatin1Char(','), QLatin1Char('.'));
            if (t.isEmpty())
                return v;
            const qint64 unit = units ? units->currentData().toLongLong() : 0;
            v.append({t, 0, unit == defaultUnit ? 0 : unit}); // единица по умолчанию подставится сама
            return v;
        };
        e.set = [edit, units, defaultUnit](const QList<MarketParamValue> &vals) {
            const QSignalBlocker b1(edit);
            edit->setText(vals.isEmpty() ? QString() : vals[0].value);
            if (units) {
                const QSignalBlocker b2(units);
                const qint64 unit = vals.isEmpty() || vals[0].unitId <= 0 ? defaultUnit : vals[0].unitId;
                const int idx = units->findData(unit);
                units->setCurrentIndex(idx >= 0 ? idx : 0);
            }
        };
    } else if (p.type == QLatin1String("BOOLEAN")) {
        auto *combo = new QComboBox(this);
        combo->addItem(QString(), QString());
        combo->addItem(tr("Да"), QStringLiteral("true"));
        combo->addItem(tr("Нет"), QStringLiteral("false"));
        connect(combo, &QComboBox::currentIndexChanged, this, dirty);
        e.widget = combo;
        e.get = [combo]() {
            QList<MarketParamValue> v;
            const QString val = combo->currentData().toString();
            if (!val.isEmpty())
                v.append({val, 0, 0});
            return v;
        };
        e.set = [combo](const QList<MarketParamValue> &vals) {
            const QSignalBlocker b(combo);
            const QString val = vals.isEmpty() ? QString() : vals[0].value.toLower();
            const int idx = combo->findData(val == QLatin1String("1") ? QStringLiteral("true") : val == QLatin1String("0") ? QStringLiteral("false") : val);
            combo->setCurrentIndex(idx >= 0 ? idx : 0);
        };
    } else { // TEXT
        auto *edit = new QLineEdit(this);
        if (p.maxLength > 0)
            edit->setMaxLength(p.maxLength);
        connect(edit, &QLineEdit::textChanged, this, dirty);
        e.widget = edit;
        e.get = [edit]() {
            QList<MarketParamValue> v;
            const QString t = edit->text().trimmed();
            if (!t.isEmpty())
                v.append({t, 0, 0});
            return v;
        };
        e.set = [edit](const QList<MarketParamValue> &vals) {
            const QSignalBlocker b(edit);
            edit->setText(vals.isEmpty() ? QString() : vals[0].value);
        };
    }
    return e;
}

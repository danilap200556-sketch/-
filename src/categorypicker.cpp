#include "categorypicker.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace {
constexpr int kIdRole = Qt::UserRole;
constexpr int kLeafRole = Qt::UserRole + 1;
constexpr int kPathRole = Qt::UserRole + 2;

QTreeWidgetItem *addNode(QTreeWidgetItem *parent, QTreeWidget *tree, const QJsonObject &node, const QString &trail,
                         qint64 current, QTreeWidgetItem **currentItem)
{
    const QString name = node.value("name").toString();
    auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setText(0, name);
    const QJsonArray kids = node.value("children").toArray();
    const QString path = trail.isEmpty() ? name : trail + QStringLiteral(" › ") + name;
    item->setData(0, kIdRole, node.value("id").toInteger());
    item->setData(0, kLeafRole, kids.isEmpty());
    item->setData(0, kPathRole, path);
    if (!kids.isEmpty()) {
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
    }
    if (kids.isEmpty() && node.value("id").toInteger() == current)
        *currentItem = item;
    for (const QJsonValue &k : kids)
        addNode(item, tree, k.toObject(), path, current, currentItem);
    return item;
}
} // namespace

CategoryPickerDialog::CategoryPickerDialog(const QJsonObject &root, qint64 current, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Категория на Маркете"));
    resize(640, 560);
    auto *layout = new QVBoxLayout(this);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Найти категорию: например, «кроссовки»"));
    m_filter->setClearButtonEnabled(true);
    layout->addWidget(m_filter);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    layout->addWidget(m_tree, 1);

    QTreeWidgetItem *currentItem = nullptr;
    // Корень («Все товары») не показываем: начинаем с его детей.
    for (const QJsonValue &k : root.value("children").toArray())
        addNode(nullptr, m_tree, k.toObject(), QString(), current, &currentItem);

    m_info = new QLabel(tr("Выберите самую нижнюю категорию (не жирную): в карточке можно указать только такую."), this);
    m_info->setWordWrap(true);
    m_info->setObjectName("muted");
    layout->addWidget(m_info);

    auto *buttons = new QHBoxLayout();
    auto *refresh = new QPushButton(tr("Обновить список с Маркета"), this);
    buttons->addWidget(refresh);
    buttons->addStretch(1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = box->button(QDialogButtonBox::Ok);
    m_ok->setEnabled(false);
    buttons->addWidget(box);
    layout->addLayout(buttons);

    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &t) {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            filterItem(m_tree->topLevelItem(i), t.trimmed());
    });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &CategoryPickerDialog::selectionChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, kLeafRole).toBool())
            accept();
    });
    connect(refresh, &QPushButton::clicked, this, [this]() {
        m_refresh = true;
        reject();
    });
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (currentItem) {
        for (QTreeWidgetItem *p = currentItem->parent(); p; p = p->parent())
            p->setExpanded(true);
        m_tree->setCurrentItem(currentItem);
        m_tree->scrollToItem(currentItem);
    }
    m_filter->setFocus();
}

bool CategoryPickerDialog::filterItem(QTreeWidgetItem *item, const QString &text)
{
    bool visible = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive);
    bool childMatch = false;
    for (int i = 0; i < item->childCount(); ++i)
        childMatch = filterItem(item->child(i), text) || childMatch;
    // Название подошло - показываем всю ветку под ним.
    if (visible && !text.isEmpty() && item->childCount() > 0 && !childMatch) {
        std::function<void(QTreeWidgetItem *)> showAll = [&](QTreeWidgetItem *n) {
            n->setHidden(false);
            for (int i = 0; i < n->childCount(); ++i)
                showAll(n->child(i));
        };
        showAll(item);
    }
    visible = visible || childMatch;
    item->setHidden(!visible);
    if (!text.isEmpty() && childMatch)
        item->setExpanded(true);
    return visible;
}

void CategoryPickerDialog::selectionChanged()
{
    const auto items = m_tree->selectedItems();
    if (items.isEmpty() || !items.first()->data(0, kLeafRole).toBool()) {
        m_ok->setEnabled(false);
        return;
    }
    m_id = items.first()->data(0, kIdRole).toLongLong();
    m_path = items.first()->data(0, kPathRole).toString();
    m_info->setText(tr("Выбрано: %1").arg(m_path));
    m_ok->setEnabled(true);
}

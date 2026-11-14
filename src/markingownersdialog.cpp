#include "markingownersdialog.h"

#include "chestnyznak.h"
#include "markingowner.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// Название и ИНН одного ИП.
class OwnerEditDialog : public QDialog
{
public:
    OwnerEditDialog(const QString &title, const QString &name, const QString &inn, QWidget *parent)
        : QDialog(parent)
    {
        setWindowTitle(title);
        auto *l = new QVBoxLayout(this);
        auto *form = new QFormLayout();
        m_name = new QLineEdit(name, this);
        m_name->setPlaceholderText(tr("например, ИП Иванов И. И."));
        m_inn = new QLineEdit(inn, this);
        m_inn->setPlaceholderText(tr("10 или 12 цифр (нужен для файла «Ввод в оборот»)"));
        m_inn->setMaxLength(12);
        form->addRow(tr("Название:"), m_name);
        form->addRow(tr("ИНН:"), m_inn);
        l->addLayout(form);
        auto *buttons = new QDialogButtonBox(this);
        buttons->addButton(tr("Сохранить"), QDialogButtonBox::AcceptRole);
        buttons->addButton(tr("Отмена"), QDialogButtonBox::RejectRole);
        l->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        resize(460, 140);
    }
    QString name() const { return m_name->text(); }
    QString inn() const { return m_inn->text(); }

private:
    QLineEdit *m_name, *m_inn;
};

} // namespace

MarkingOwnersDialog::MarkingOwnersDialog(QWidget *parent, bool suzButtons)
    : QDialog(parent)
{
    setWindowTitle(tr("ИП и настройки СУЗ"));
    resize(640, 460);
    auto *l = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Коды «Честного знака» хранятся по ИП: у каждого свой кабинет, свои коды и своя электронная подпись. Добавьте ИП здесь. Склады к ИП не "
                               "привязаны: код хранится и с ИП, и (отдельно) со складом, под остаток которого он заказан. ИП для печати к заказам Маркета задаётся в "
                               "настройках кабинета Маркета. "
                               "Настройки СУЗ (идентификатор СУЗ, соединение, токен, сертификат) у каждого ИП свои - кнопка «Настройки СУЗ…»."),
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    l->addWidget(hint);
    m_list = new QListWidget(this);
    l->addWidget(m_list, 1);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    l->addWidget(m_info);

    auto *row = new QHBoxLayout();
    auto *addBtn = new QPushButton(tr("Добавить…"), this);
    auto *editBtn = new QPushButton(tr("Изменить…"), this);
    auto *suzBtn = new QPushButton(tr("Настройки СУЗ…"), this);
    auto *copyBtn = new QPushButton(tr("Взять общие настройки СУЗ"), this);
    copyBtn->setToolTip(tr("Скопировать этому ИП идентификатор СУЗ, соединение и токен, которые были введены до появления ИП (общие настройки)"));
    auto *delBtn = new QPushButton(tr("Удалить"), this);
    auto *close = new QPushButton(tr("Закрыть"), this);
    for (QPushButton *b : {addBtn, editBtn, suzBtn, copyBtn, delBtn})
        row->addWidget(b);
    suzBtn->setVisible(suzButtons);
    copyBtn->setVisible(suzButtons);
    row->addStretch(1);
    row->addWidget(close);
    for (QPushButton *b : {addBtn, editBtn, suzBtn, copyBtn, delBtn, close})
        b->setAutoDefault(false);
    l->addLayout(row);

    connect(addBtn, &QPushButton::clicked, this, [this]() { addOwner(); });
    connect(editBtn, &QPushButton::clicked, this, [this]() { editOwner(); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this]() { editOwner(); });
    connect(delBtn, &QPushButton::clicked, this, [this]() { removeOwner(); });
    connect(suzBtn, &QPushButton::clicked, this, [this]() {
        if (currentId() > 0)
            emit suzSettingsRequested(currentId());
    });
    connect(copyBtn, &QPushButton::clicked, this, [this]() {
        const int id = currentId();
        if (id <= 0)
            return;
        const ChestnyZnak::Settings general = ChestnyZnak::loadSettings(0);
        if (general.omsId.isEmpty() && general.clientToken.isEmpty()) {
            QMessageBox::information(this, windowTitle(), tr("Общих настроек СУЗ нет - заполните настройки этого ИП кнопкой «Настройки СУЗ…»."));
            return;
        }
        ChestnyZnak::Settings s = ChestnyZnak::loadSettings(id);
        if (!s.omsId.isEmpty() || !s.clientToken.isEmpty()) {
            if (QMessageBox::question(this, windowTitle(), tr("У этого ИП уже есть свои настройки СУЗ. Заменить их общими?")) != QMessageBox::Yes)
                return;
        }
        s.omsId = general.omsId;
        s.omsConnection = general.omsConnection;
        s.clientToken = general.clientToken;
        s.tokenIssuedAt = general.tokenIssuedAt;
        s.certThumbprint = general.certThumbprint;
        ChestnyZnak::saveSettings(s);
        reload(id);
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_list, &QListWidget::currentRowChanged, this, [this]() {
        const MarkingOwner::Owner o = MarkingOwner::get(currentId());
        if (o.id <= 0) {
            m_info->clear();
            return;
        }
        const ChestnyZnak::Settings s = ChestnyZnak::loadSettings(o.id);
        m_info->setText(tr("СУЗ этого ИП: идентификатор %1, токен %2.").arg(s.omsId.isEmpty() ? tr("не задан") : s.omsId, s.clientToken.isEmpty() ? tr("не получен") : tr("есть")));
    });
    reload();
}

int MarkingOwnersDialog::currentId() const
{
    const QListWidgetItem *it = m_list->currentItem();
    return it ? it->data(Qt::UserRole).toInt() : 0;
}

void MarkingOwnersDialog::reload(int selectId)
{
    const int keep = selectId > 0 ? selectId : currentId();
    m_list->clear();
    const QHash<int, int> counts = MarkingOwner::codeCounts();
    for (const MarkingOwner::Owner &o : MarkingOwner::list()) {
        auto *item = new QListWidgetItem(tr("%1 - ИНН %2, кодов: %3").arg(o.name, o.inn.isEmpty() ? tr("не указан") : o.inn).arg(counts.value(o.id)), m_list);
        item->setData(Qt::UserRole, o.id);
        if (o.id == keep)
            m_list->setCurrentItem(item);
    }
    if (!m_list->currentItem() && m_list->count() > 0)
        m_list->setCurrentRow(0);
    const int noOwner = counts.value(0);
    if (m_list->count() == 0)
        m_info->setText(tr("ИП пока нет. Кодов без ИП: %1.").arg(noOwner));
}

void MarkingOwnersDialog::addOwner()
{
    OwnerEditDialog dlg(tr("Новый ИП"), QString(), QString(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    const int id = MarkingOwner::add(dlg.name(), dlg.inn(), &err);
    if (id <= 0) {
        QMessageBox::warning(this, windowTitle(), tr("Не добавлено: %1.").arg(err));
        return;
    }
    reload(id);
}

void MarkingOwnersDialog::editOwner()
{
    const MarkingOwner::Owner o = MarkingOwner::get(currentId());
    if (o.id <= 0)
        return;
    OwnerEditDialog dlg(tr("Изменить ИП"), o.name, o.inn, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QString err;
    if (!MarkingOwner::update(o.id, dlg.name(), dlg.inn(), &err)) {
        QMessageBox::warning(this, windowTitle(), tr("Не сохранено: %1.").arg(err));
        return;
    }
    reload(o.id);
}

void MarkingOwnersDialog::removeOwner()
{
    const MarkingOwner::Owner o = MarkingOwner::get(currentId());
    if (o.id <= 0)
        return;
    const int codes = MarkingOwner::codeCounts().value(o.id);
    if (QMessageBox::question(this, windowTitle(), tr("Удалить «%1»? Его коды (%2) останутся, но будут без ИП; кабинеты Маркета этого ИП останутся без ИП. Настройки СУЗ этого ИП пропадут.").arg(o.name).arg(codes))
        != QMessageBox::Yes)
        return;
    QString err;
    if (!MarkingOwner::remove(o.id, &err)) {
        QMessageBox::warning(this, windowTitle(), tr("Не удалено: %1.").arg(err));
        return;
    }
    reload();
}

#include "marketaccountdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSqlQuery>
#include <QVBoxLayout>

namespace {
constexpr int RoleCampaignId = Qt::UserRole;
constexpr int RoleBusinessId = Qt::UserRole + 1;
}

MarketAccountDialog::MarketAccountDialog(const MarketAccount &account, QWidget *parent)
    : QDialog(parent), m_account(account)
{
    setWindowTitle(account.id ? tr("Кабинет Маркета — %1").arg(account.name) : tr("Новый кабинет Маркета"));
    setMinimumWidth(520);
    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(
        tr("API-ключ создаётся в кабинете продавца: «Настройки» → «API и модули» → «Авторизационные "
           "токены». Ключу нужны доступы к управлению товарами (остатки и цены)."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");
    layout->addWidget(hint);

    auto *form = new QFormLayout();
    layout->addLayout(form);

    m_name = new QLineEdit(account.name, this);
    m_name->setPlaceholderText(tr("подпись, чтобы не путать кабинеты, напр. «Маркет — магазин 1»"));
    form->addRow(tr("Название"), m_name);

    auto *keyRow = new QHBoxLayout();
    m_apiKey = new QLineEdit(account.apiKey, this);
    m_apiKey->setEchoMode(QLineEdit::Password);
    auto *showKey = new QCheckBox(tr("показать"), this);
    connect(showKey, &QCheckBox::toggled, this, [this](bool on) {
        m_apiKey->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    keyRow->addWidget(m_apiKey);
    keyRow->addWidget(showKey);
    form->addRow(tr("API-ключ"), keyRow);

    auto *campaignRow = new QHBoxLayout();
    m_campaign = new QComboBox(this);
    m_campaign->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto *loadCampaignsBtn = new QPushButton(tr("Загрузить магазины"), this);
    campaignRow->addWidget(m_campaign, 1);
    campaignRow->addWidget(loadCampaignsBtn);
    form->addRow(tr("Магазин"), campaignRow);
    if (account.campaignId) {
        m_campaign->addItem(tr("Магазин ID %1").arg(account.campaignId));
        m_campaign->setItemData(0, account.campaignId, RoleCampaignId);
        m_campaign->setItemData(0, account.businessId, RoleBusinessId);
    }

    m_groups = new QCheckBox(tr("В кабинете настроены группы складов"), this);
    m_groups->setChecked(account.warehouseGroups);
    m_groups->setToolTip(tr("Если склады объединены в группы, Маркет принимает остатки по магазину, "
                            "а не по отдельному складу. Не уверены - оставьте выключенным."));
    form->addRow(QString(), m_groups);

    auto *warehouseRow = new QHBoxLayout();
    m_warehouse = new QComboBox(this);
    m_loadWarehousesBtn = new QPushButton(tr("Загрузить склады"), this);
    warehouseRow->addWidget(m_warehouse, 1);
    warehouseRow->addWidget(m_loadWarehousesBtn);
    form->addRow(tr("Склад Маркета"), warehouseRow);
    if (account.marketWarehouseId) {
        m_warehouse->addItem(tr("Склад ID %1").arg(account.marketWarehouseId), account.marketWarehouseId);
    }

    m_localWarehouses = new QListWidget(this);
    m_localWarehouses->setMaximumHeight(130);
    QSqlQuery q("SELECT id, name FROM warehouses ORDER BY name");
    while (q.next()) {
        auto *item = new QListWidgetItem(q.value(1).toString(), m_localWarehouses);
        item->setData(Qt::UserRole, q.value(0).toInt());
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(account.localWarehouseIds.contains(q.value(0).toInt()) ? Qt::Checked : Qt::Unchecked);
    }
    form->addRow(tr("Наши склады"), m_localWarehouses);
    auto *localHint = new QLabel(tr("Остатки отмеченных складов суммируются и передаются в этот кабинет. "
                                    "Ничего не отмечено - берутся все склады."),
                                 this);
    localHint->setWordWrap(true);
    localHint->setStyleSheet("color: gray;");
    form->addRow(QString(), localHint);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &MarketAccountDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(loadCampaignsBtn, &QPushButton::clicked, this, &MarketAccountDialog::loadCampaigns);
    connect(m_loadWarehousesBtn, &QPushButton::clicked, this, &MarketAccountDialog::loadWarehouses);
    connect(m_campaign, &QComboBox::currentIndexChanged, this, &MarketAccountDialog::onCampaignChanged);
    connect(m_groups, &QCheckBox::toggled, this, &MarketAccountDialog::updateWarehouseControls);
    updateWarehouseControls();
}

void MarketAccountDialog::loadCampaigns()
{
    if (m_apiKey->text().trimmed().isEmpty()) {
        m_status->setStyleSheet("color: #c0392b;");
        m_status->setText(tr("Сначала вставьте API-ключ."));
        return;
    }
    m_status->setStyleSheet("color: gray;");
    m_status->setText(tr("Запрашиваю список магазинов у Маркета..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QList<MarketCampaign> campaigns;
    QString err;
    const bool ok = MarketApi(m_apiKey->text()).campaigns(&campaigns, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        m_status->setStyleSheet("color: #c0392b;");
        m_status->setText(tr("Не удалось получить магазины: %1").arg(err));
        return;
    }

    const qint64 previous = m_campaign->currentData(RoleCampaignId).toLongLong();
    m_campaign->blockSignals(true);
    m_campaign->clear();
    for (const auto &c : campaigns) {
        m_campaign->addItem(tr("%1 — %2, ID %3 (кабинет «%4»)")
                                .arg(c.domain, c.placementType)
                                .arg(c.id)
                                .arg(c.businessName));
        const int i = m_campaign->count() - 1;
        m_campaign->setItemData(i, c.id, RoleCampaignId);
        m_campaign->setItemData(i, c.businessId, RoleBusinessId);
        if (c.id == previous)
            m_campaign->setCurrentIndex(i);
    }
    m_campaign->blockSignals(false);
    onCampaignChanged();

    m_status->setStyleSheet("color: #27ae60;");
    m_status->setText(campaigns.isEmpty() ? tr("Ключ рабочий, но магазинов в кабинете нет.")
                                          : tr("Ключ рабочий, магазинов: %1. Выберите нужный.").arg(campaigns.size()));
    if (m_name->text().trimmed().isEmpty() && !campaigns.isEmpty())
        m_name->setText(m_campaign->currentText().section(" — ", 0, 0));
}

void MarketAccountDialog::onCampaignChanged()
{
    // Склады принадлежат кабинету (business); при смене магазина из другого
    // кабинета прежний выбор склада становится недействительным.
    const qint64 businessId = m_campaign->currentData(RoleBusinessId).toLongLong();
    if (businessId != m_account.businessId) {
        m_warehouse->clear();
        m_account.businessId = businessId;
    }
}

void MarketAccountDialog::updateWarehouseControls()
{
    const bool needWarehouse = !m_groups->isChecked();
    m_warehouse->setEnabled(needWarehouse);
    m_loadWarehousesBtn->setEnabled(needWarehouse);
}

void MarketAccountDialog::loadWarehouses()
{
    const qint64 businessId = m_campaign->currentData(RoleBusinessId).toLongLong();
    if (!businessId) {
        m_status->setStyleSheet("color: #c0392b;");
        m_status->setText(tr("Сначала загрузите и выберите магазин."));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QList<MarketWarehouse> warehouses;
    QString err;
    const bool ok = MarketApi(m_apiKey->text()).partnerWarehouses(businessId, &warehouses, &err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        m_status->setStyleSheet("color: #c0392b;");
        m_status->setText(tr("Не удалось получить склады: %1").arg(err));
        return;
    }
    const qint64 previous = m_warehouse->currentData().toLongLong();
    m_warehouse->clear();
    for (const auto &w : warehouses) {
        m_warehouse->addItem(tr("%1 (ID %2, %3)").arg(w.name).arg(w.id).arg(w.models.join(", ")), w.id);
        if (w.id == previous)
            m_warehouse->setCurrentIndex(m_warehouse->count() - 1);
    }
    if (warehouses.isEmpty()) {
        m_status->setStyleSheet("color: #c0392b;");
        m_status->setText(tr("Маркет не вернул ни одного отдельного склада. Если склады объединены в группы - "
                             "включите галку «группы складов»."));
    } else {
        m_status->setStyleSheet("color: #27ae60;");
        m_status->setText(tr("Складов: %1. Выберите тот, куда передавать остатки.").arg(warehouses.size()));
    }
}

void MarketAccountDialog::validateAndAccept()
{
    QString err;
    if (m_name->text().trimmed().isEmpty())
        err = tr("Укажите название кабинета.");
    else if (m_apiKey->text().trimmed().isEmpty())
        err = tr("Укажите API-ключ.");
    else if (!m_campaign->currentData(RoleCampaignId).toLongLong())
        err = tr("Загрузите и выберите магазин.");
    else if (!m_groups->isChecked() && !m_warehouse->currentData().toLongLong())
        err = tr("Загрузите и выберите склад Маркета (или отметьте, что в кабинете группы складов).");
    if (!err.isEmpty()) {
        m_status->setStyleSheet("color: #c0392b;");
        m_status->setText(err);
        return;
    }

    m_account.name = m_name->text().trimmed();
    m_account.apiKey = m_apiKey->text().trimmed();
    m_account.campaignId = m_campaign->currentData(RoleCampaignId).toLongLong();
    m_account.businessId = m_campaign->currentData(RoleBusinessId).toLongLong();
    m_account.warehouseGroups = m_groups->isChecked();
    m_account.marketWarehouseId = m_account.warehouseGroups ? 0 : m_warehouse->currentData().toLongLong();
    m_account.localWarehouseIds.clear();
    for (int i = 0; i < m_localWarehouses->count(); ++i) {
        const auto *item = m_localWarehouses->item(i);
        if (item->checkState() == Qt::Checked)
            m_account.localWarehouseIds.append(item->data(Qt::UserRole).toInt());
    }
    accept();
}

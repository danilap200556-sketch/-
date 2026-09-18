#include "mainwindow.h"
#include "productstab.h"
#include "warehousestab.h"
#include "stocktab.h"
#include "labelstab.h"

#include <QTabWidget>

MainWindow::MainWindow(const QString &username, QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Управление складом — %1").arg(username));
    resize(1200, 800);

    auto *tabs = new QTabWidget(this);
    setCentralWidget(tabs);

    m_productsTab = new ProductsTab(this);
    m_warehousesTab = new WarehousesTab(this);
    m_stockTab = new StockTab(this);
    m_labelsTab = new LabelsTab(this);

    tabs->addTab(m_productsTab, tr("Товары"));
    tabs->addTab(m_warehousesTab, tr("Склады"));
    tabs->addTab(m_stockTab, tr("Остатки и движения"));
    tabs->addTab(m_labelsTab, tr("Этикетки"));

    // Изменения товаров/складов должны сразу отражаться там, где на них
    // ссылаются другие вкладки (комбобоксы, сводные таблицы).
    connect(m_productsTab, &ProductsTab::productsChanged, m_stockTab, &StockTab::refresh);
    connect(m_productsTab, &ProductsTab::productsChanged, m_labelsTab, &LabelsTab::refresh);
    connect(m_warehousesTab, &WarehousesTab::warehousesChanged, m_stockTab, &StockTab::refresh);
    connect(m_warehousesTab, &WarehousesTab::warehousesChanged, m_labelsTab, &LabelsTab::refresh);

    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 2)
            m_stockTab->refresh();
        else if (index == 3)
            m_labelsTab->refresh();
    });
}

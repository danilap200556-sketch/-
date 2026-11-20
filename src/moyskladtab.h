#pragma once

#include "moysklad.h"

#include <QWidget>

class MoySkladAutoSync;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;

// Страница «МойСклад»: остатки из МойСклад приходят в программу. МойСклад здесь главный: остатки сопоставленных складов в программе
// приводятся к его остаткам, расхождения пишутся в журнал одним документом (в разделе «Документы» его можно отменить целиком).
// Три вкладки: «Подключение» (токен, какой склад МойСклад - какой склад программы), «Остатки» (показать расхождения и загрузить),
// «Автозагрузка» (по расписанию, пока запущена программа). Только чтение из МойСклад: в МойСклад программа ничего не записывает.
class MoySkladTab : public QWidget
{
    Q_OBJECT
public:
    explicit MoySkladTab(MoySkladAutoSync *autoSync, QWidget *parent = nullptr);

    void refresh();

    // Действия кнопок (для проверок). checkConnection: токен рабочий, склады получены. preview: расхождения без записи.
    // loadNow: расхождения и запись; ask - спросить перед записью. Возвращает, сколько записей журнала изменено (-1 - не удалось или отказались).
    bool checkConnection();
    bool preview();
    int loadNow(bool ask = true);
    MoySklad::Plan plan() const { return m_plan; }

signals:
    void stockChanged();
    void documentCreated(int documentId);
    void warehousesChanged();

private slots:
    void saveConnection();
    void matchByName();
    void createMissingWarehouses();
    void saveSchedule();
    void updateAutoStatus();
    void log(const QString &text);

private:
    MoySklad::Settings settingsFromUi() const;
    QHash<QString, int> mapFromTable() const;
    void rebuildStores(const QHash<QString, int> &map);
    QList<QPair<int, QString>> warehouses() const;
    void showPlan(const MoySklad::Plan &plan);
    void setConnectionStatus(const QString &text, bool ok);

    MoySkladAutoSync *m_auto;
    QTabWidget *m_pages = nullptr;
    QLineEdit *m_token = nullptr;
    QLabel *m_connStatus = nullptr;
    QTableWidget *m_stores = nullptr;
    QComboBox *m_stockType = nullptr;
    QCheckBox *m_zeroUnknown = nullptr;
    QTableWidget *m_changes = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_previewBtn = nullptr;
    QPushButton *m_loadBtn = nullptr;
    QCheckBox *m_autoEnabled = nullptr;
    QSpinBox *m_autoEvery = nullptr;
    QComboBox *m_autoUnit = nullptr;
    QLabel *m_autoStatus = nullptr;
    QPushButton *m_autoRunBtn = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QList<MoySklad::Store> m_storeList;
    MoySklad::Plan m_plan;
    bool m_busy = false;
};

#pragma once

#include <QWidget>
#include <QStringList>
#include <QVector>

class QLabel;
class QSpinBox;
class QTableWidget;
class QComboBox;
class QRadioButton;
class QCheckBox;
class QPlainTextEdit;
class QVBoxLayout;

// Вкладка "Импорт": загрузка CSV-выгрузки остатков (например, отчёт
// "Остатки" из МойСклад, сохранённый как CSV) с ручным сопоставлением
// колонок - разные выгрузки называют и располагают колонки по-разному
// (например, у части пользователей реальный артикул лежит в колонке
// "Наименование", а не "Артикул"), поэтому колонка-источник и её роль
// выбираются вручную, а не жёстко зашиты.
class ImportTab : public QWidget
{
    Q_OBJECT
public:
    enum Role { RoleIgnore = 0, RoleKey, RoleName, RoleStock, RoleWarehouse, RoleBarcode };

    explicit ImportTab(QWidget *parent = nullptr);

    void refresh(); // перечитать список складов

signals:
    void dataImported();

private slots:
    void pickFile();
    void exportStock();
    void onHeaderRowChanged(int row);
    void runImport();

private:
    void showRawPreview();
    void rebuildColumnMapping();
    void guessHeaderRow();

    QVector<QStringList> m_rows;
    QString m_fileName;

    QLabel *m_fileLabel;
    QSpinBox *m_headerRow;
    QTableWidget *m_rawPreview;

    QWidget *m_mappingBox;
    QVBoxLayout *m_mappingLayout;
    QVector<QComboBox *> m_roleCombos;

    QComboBox *m_warehouseCombo;
    QRadioButton *m_modeInventory;
    QRadioButton *m_modeReceipt;
    QRadioButton *m_modeWriteOff;
    QCheckBox *m_createMissing;

    QPlainTextEdit *m_log;
};

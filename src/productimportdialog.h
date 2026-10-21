#pragma once

#include "productsync.h"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;
class QVBoxLayout;

// Обновление данных товаров из Excel/CSV: название, описание, цена, артикул на Маркете, свой код, штрихкоды.
// Остатки не меняются. Выбираете файл, указываете, что означает каждая колонка (для выгрузки из программы
// это определяется само), смотрите «что изменится» и применяете. Прежние значения можно сохранить в файл «как было».
class ProductImportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ProductImportDialog(QWidget *parent = nullptr);

    // Загружает таблицу (без выбора файла - для кнопки «Выбрать файл» и для тестов).
    bool loadRows(const QVector<QStringList> &rows, const QString &fileName);
    bool loadFile(const QString &path);
    void setRole(int column, int role);
    void setOptions(bool createMissing, bool clearEmpty);
    void setHeaderRow(int row); // с единицы
    const ProductSync::Plan &plan() const { return m_plan; }
    // Записывает (без вопросов). При отказе - false, причина в lastError().
    bool apply();
    bool applied() const { return m_applied; }
    QString lastError() const { return m_error; }
    const ProductSync::Result &result() const { return m_result; }

private slots:
    void chooseFile();
    void refreshPlan();
    void rebuildMapping();
    void applyClicked();
    void saveBackup();

private:
    void schedule();

    QVector<QStringList> m_rows;
    QString m_fileName;
    QLabel *m_fileLabel = nullptr;
    QSpinBox *m_headerRow = nullptr;
    QTableWidget *m_raw = nullptr;
    QVBoxLayout *m_mappingLayout = nullptr;
    QList<QComboBox *> m_combos;
    QCheckBox *m_createMissing = nullptr;
    QCheckBox *m_clearEmpty = nullptr;
    QLabel *m_summary = nullptr;
    QPlainTextEdit *m_problems = nullptr;
    QTableWidget *m_diff = nullptr;
    QPushButton *m_applyButton = nullptr;
    QTimer *m_timer = nullptr;

    ProductSync::Plan m_plan;
    ProductSync::Result m_result;
    QString m_error;
    bool m_applied = false;
    bool m_building = false;
};

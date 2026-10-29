#pragma once

#include "photoexcel.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

// «Фото из Excel»: выбираете файл со ссылками на фото (артикул + «Фото 1», «Фото 2»…), программа скачивает картинки, приводит к
// JPEG и сохраняет в карточки - с заменой прежних фото или добавлением. Скачивание идёт пачками, можно остановить: что успело
// сохраниться, остаётся.
class PhotoExcelDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PhotoExcelDialog(QWidget *parent = nullptr);

    bool changed() const { return m_changed; }
    // Разобрать файл без диалога выбора (для проверок).
    bool loadFile(const QString &path);

    void reject() override;

private slots:
    void pickFile();
    void start();

private:
    void setBusy(bool busy);
    void log(const QString &line);

    PhotoExcel::Parsed m_parsed;
    bool m_changed = false;
    bool m_running = false;
    bool m_stop = false;

    QLabel *m_fileLabel;
    QLabel *m_summary;
    QCheckBox *m_replace;
    QSpinBox *m_perProduct;
    QProgressBar *m_bar;
    QLabel *m_status;
    QPlainTextEdit *m_log;
    QPushButton *m_pickBtn;
    QPushButton *m_startBtn;
    QPushButton *m_stopBtn;
    QPushButton *m_closeBtn;
};

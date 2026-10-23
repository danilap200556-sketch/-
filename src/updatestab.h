#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class Updater;

// Вкладка «Обновления»: какая версия установлена, есть ли новая, что в ней изменилось, кнопки «Проверить», «Скачать и
// установить» и «Установить и перезапустить». Саму работу (сеть, файлы) делает Updater.
class UpdatesTab : public QWidget
{
    Q_OBJECT
public:
    explicit UpdatesTab(Updater *updater, QWidget *parent = nullptr);

    // «Проверять при запуске» (запоминается на этом компьютере; по умолчанию включено).
    static bool autoCheckEnabled();

public slots:
    void checkNow();
    // Скачать новую версию и (после согласия) предложить установить.
    void startDownload();
    void refresh();

signals:
    // Скачано, пользователь согласен закрыть программу и установить.
    void installRequested();

private:
    void offerInstall();

    Updater *m_updater;
    QLabel *m_current;
    QLabel *m_status;
    QProgressBar *m_progress;
    QPushButton *m_check;
    QPushButton *m_download;
    QPushButton *m_install;
    QPushButton *m_cancel;
    QCheckBox *m_auto;
    QTextBrowser *m_notes;
    bool m_checkedOnce = false;
    QString m_downloadError;
};

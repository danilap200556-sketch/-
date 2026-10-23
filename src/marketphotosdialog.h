#pragma once

#include <QDialog>
#include <QList>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;

// «Фото с Маркета»: скачивает фото товаров, которые уже есть на Яндекс Маркете (или в карточках Маркета в программе),
// и сохраняет их у товаров в базе. Логика - в MarketPhotos::run, здесь только выбор и отчёт.
class MarketPhotosDialog : public QDialog
{
    Q_OBJECT
public:
    MarketPhotosDialog(const QList<int> &selectedProductIds, QWidget *parent = nullptr);

    bool changed() const { return m_changed; }

    void reject() override;

private slots:
    void start();

private:
    void setBusy(bool busy);
    void log(const QString &line);

    QList<int> m_selected;
    bool m_changed = false;
    bool m_running = false;
    bool m_stop = false;

    QRadioButton *m_toSelected;
    QRadioButton *m_toAll;
    QCheckBox *m_onlyEmpty;
    QCheckBox *m_fromMarket;
    QCheckBox *m_fromCards;
    QProgressBar *m_bar;
    QLabel *m_status;
    QPlainTextEdit *m_log;
    QPushButton *m_startBtn;
    QPushButton *m_stopBtn;
    QPushButton *m_closeBtn;
};

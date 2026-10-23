#pragma once

#include <QDialog>
#include <QList>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;

// «Найти фото»: подбирает фото товарам, у которых их нет, и сохраняет в базе. Источники - Яндекс Маркет, карточки Маркета
// в программе и поиск картинок в интернете по названию (см. PhotoFetch::run). Здесь только выбор и отчёт.
class PhotoFetchDialog : public QDialog
{
    Q_OBJECT
public:
    PhotoFetchDialog(const QList<int> &selectedProductIds, QWidget *parent = nullptr);

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
    QSpinBox *m_perProduct;
    QCheckBox *m_fromMarket;
    QCheckBox *m_fromCards;
    QCheckBox *m_fromWeb;
    QProgressBar *m_bar;
    QLabel *m_status;
    QPlainTextEdit *m_log;
    QPushButton *m_startBtn;
    QPushButton *m_stopBtn;
    QPushButton *m_closeBtn;
};

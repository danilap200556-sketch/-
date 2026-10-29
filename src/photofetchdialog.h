#pragma once

#include "photofetch.h"

#include <QDialog>
#include <QList>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;

// «Найти фото»: подбирает фото товарам и сохраняет в базе. Источники - Яндекс Маркет, карточки Маркета в программе и
// интернет по артикулу (магазины, New Balance, Google по ключу, см. PhotoFetch). По умолчанию найденное сначала показывается
// (PhotoReviewDialog) и сохраняется только выбранное; можно сохранять сразу, тогда берутся лишь проверенные снимки.
class PhotoFetchDialog : public QDialog
{
    Q_OBJECT
public:
    PhotoFetchDialog(const QList<int> &selectedProductIds, QWidget *parent = nullptr);

    bool changed() const { return m_changed; }

    void reject() override;

private slots:
    void start();
    void editGoogle();
    void diagnose();

private:
    void setBusy(bool busy);
    void log(const QString &line);
    PhotoFetch::Options options() const;
    void reportResult(const PhotoFetch::Result &r, const PhotoFetch::Options &opt, bool saved);
    void updateGoogleLabel();

    QList<int> m_selected;
    bool m_changed = false;
    bool m_running = false;
    bool m_stop = false;

    QRadioButton *m_toSelected;
    QRadioButton *m_toAll;
    QCheckBox *m_onlyEmpty;
    QSpinBox *m_perProduct;
    QCheckBox *m_replace;
    QCheckBox *m_fromMarket;
    QCheckBox *m_fromCards;
    QCheckBox *m_fromShops;
    QCheckBox *m_fromNb;
    QCheckBox *m_fromGoogle;
    bool m_hasGoogle = false;
    QLabel *m_googleState;
    QPushButton *m_googleBtn;
    QRadioButton *m_modePreview;
    QRadioButton *m_modeDirect;
    QCheckBox *m_acceptUnverified;
    QProgressBar *m_bar;
    QLabel *m_status;
    QPlainTextEdit *m_log;
    QPushButton *m_startBtn;
    QPushButton *m_diagBtn;
    QPushButton *m_stopBtn;
    QPushButton *m_closeBtn;
};

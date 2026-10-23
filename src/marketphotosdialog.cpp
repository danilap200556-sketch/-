#include "marketphotosdialog.h"

#include "marketapi.h"
#include "marketphotos.h"
#include "photostore.h"

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSqlQuery>
#include <QVBoxLayout>

MarketPhotosDialog::MarketPhotosDialog(const QList<int> &selectedProductIds, QWidget *parent)
    : QDialog(parent), m_selected(selectedProductIds)
{
    setWindowTitle(tr("Фото с Маркета"));
    resize(720, 560);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Программа найдёт фото ваших товаров на Яндекс Маркете (по артикулу на Маркете, а если его нет - по артикулу), "
                                "скачает их и сохранит в карточки товаров: их увидят все компьютеры и сайт. "
                                "Такие же фото, которые у товара уже есть, повторно не добавляются."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_toSelected = new QRadioButton(tr("Выделенные товары (%1 шт.)").arg(m_selected.size()), this);
    m_toAll = new QRadioButton(tr("Все товары"), this);
    if (m_selected.isEmpty()) {
        m_toSelected->setEnabled(false);
        m_toSelected->setToolTip(tr("Чтобы выбрать часть товаров, выделите их в списке (Ctrl или Shift + клик)"));
        m_toAll->setChecked(true);
    } else {
        m_toSelected->setChecked(true);
    }
    layout->addWidget(m_toSelected);
    layout->addWidget(m_toAll);

    m_onlyEmpty = new QCheckBox(tr("Только товарам, у которых ещё нет фото"), this);
    m_onlyEmpty->setChecked(true);
    m_onlyEmpty->setToolTip(tr("Если снять галочку, к уже имеющимся фото добавятся недостающие (не больше %1 на товар).")
                                .arg(PhotoStore::kMaxPhotosPerProduct));
    layout->addWidget(m_onlyEmpty);

    const int accounts = MarketAccount::loadAll().size();
    m_fromMarket = new QCheckBox(tr("Спросить у Маркета (кабинетов: %1)").arg(accounts), this);
    m_fromMarket->setChecked(accounts > 0);
    m_fromMarket->setEnabled(accounts > 0);
    if (accounts == 0)
        m_fromMarket->setToolTip(tr("Кабинет Маркета не подключён: меню «Маркет» → «Кабинеты»."));
    m_fromCards = new QCheckBox(tr("Взять ссылки на фото из карточек Маркета в программе (вкладка «Карточки»)"), this);
    m_fromCards->setChecked(true);
    layout->addWidget(m_fromMarket);
    layout->addWidget(m_fromCards);

    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 1);
    m_bar->setValue(0);
    layout->addWidget(m_bar);
    m_status = new QLabel(this);
    layout->addWidget(m_status);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    layout->addWidget(m_log, 1);

    auto *bottom = new QHBoxLayout();
    m_startBtn = new QPushButton(tr("Скачать фото"), this);
    m_stopBtn = new QPushButton(tr("Остановить"), this);
    m_stopBtn->setEnabled(false);
    m_closeBtn = new QPushButton(tr("Закрыть"), this);
    bottom->addStretch();
    bottom->addWidget(m_startBtn);
    bottom->addWidget(m_stopBtn);
    bottom->addWidget(m_closeBtn);
    layout->addLayout(bottom);

    connect(m_startBtn, &QPushButton::clicked, this, &MarketPhotosDialog::start);
    connect(m_stopBtn, &QPushButton::clicked, this, [this]() { m_stop = true; m_stopBtn->setEnabled(false); });
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void MarketPhotosDialog::reject()
{
    if (m_running) { // закрыли окно во время работы: сначала останавливаемся
        m_stop = true;
        m_stopBtn->setEnabled(false);
        return;
    }
    QDialog::reject();
}

void MarketPhotosDialog::setBusy(bool busy)
{
    m_running = busy;
    for (QWidget *w : {static_cast<QWidget *>(m_toAll), static_cast<QWidget *>(m_onlyEmpty), static_cast<QWidget *>(m_startBtn),
                       static_cast<QWidget *>(m_closeBtn)})
        w->setEnabled(!busy);
    m_toSelected->setEnabled(!busy && !m_selected.isEmpty());
    m_fromMarket->setEnabled(!busy && MarketAccount::loadAll().size() > 0);
    m_fromCards->setEnabled(!busy);
    m_stopBtn->setEnabled(busy);
}

void MarketPhotosDialog::log(const QString &line)
{
    m_log->appendPlainText(line);
}

void MarketPhotosDialog::start()
{
    if (!m_fromMarket->isChecked() && !m_fromCards->isChecked()) {
        m_status->setText(tr("Отметьте, откуда брать ссылки на фото."));
        return;
    }
    MarketPhotos::Options opt;
    if (m_toSelected->isChecked())
        opt.productIds = m_selected;
    opt.onlyWithoutPhotos = m_onlyEmpty->isChecked();
    opt.fromMarket = m_fromMarket->isChecked();
    opt.fromCards = m_fromCards->isChecked();

    m_log->clear();
    m_stop = false;
    m_bar->setRange(0, 0); // пока ссылки собираются - бегунок без процентов
    m_status->setText(tr("Готовлю список товаров…"));
    setBusy(true);
    QApplication::processEvents();

    const MarketPhotos::Result r = MarketPhotos::run(opt, [this](int done, int total, const QString &text) {
        if (total > 0) {
            m_bar->setRange(0, total);
            m_bar->setValue(done);
        }
        if (!text.isEmpty())
            m_status->setText(text);
        QApplication::processEvents(); // перерисовка и кнопка «Остановить»
        return !m_stop;
    });
    setBusy(false);
    m_bar->setRange(0, 1);
    m_bar->setValue(r.cancelled ? 0 : 1);
    if (r.added > 0)
        m_changed = true;

    if (r.considered == 0) {
        m_status->setText(tr("Нет подходящих товаров."));
        log(opt.onlyWithoutPhotos ? tr("Среди выбранных нет товаров без фото. Снимите галочку «Только товарам, у которых ещё нет фото», "
                                       "чтобы добавить недостающие фото и тем, у кого они есть.")
                                  : tr("Нет товаров, которым можно добавить фото (у всех уже %1).").arg(PhotoStore::kMaxPhotosPerProduct));
        return;
    }
    m_status->setText(r.cancelled ? tr("Остановлено. Добавлено фото: %1.").arg(r.added) : tr("Готово. Добавлено фото: %1.").arg(r.added));

    log(tr("Товаров проверено: %1").arg(r.considered));
    log(tr("Нашлись ссылки на фото: у %1 товаров").arg(r.withLinks));
    log(tr("Добавлено фото: %1 (у %2 товаров)").arg(r.added).arg(r.productsFilled));
    if (r.duplicates > 0)
        log(tr("Пропущено, такие фото уже есть: %1").arg(r.duplicates));
    if (r.failed > 0)
        log(tr("Не удалось скачать или сохранить: %1").arg(r.failed));
    if (!r.noLinks.isEmpty()) {
        QStringList shown = r.noLinks.mid(0, 15);
        log(tr("Без фото на Маркете (%1): %2%3").arg(r.noLinks.size()).arg(shown.join(QStringLiteral(", ")),
                                                                              r.noLinks.size() > shown.size() ? QStringLiteral(" …") : QString()));
        log(tr("Для них фото можно добавить вручную: «Фото…» или «Фото пачкой…»."));
    }
    for (const QString &e : r.marketErrors)
        log(tr("Маркет не ответил - %1").arg(e));
    if (!r.problems.isEmpty()) {
        log(QString());
        log(tr("Подробности ошибок:"));
        for (const QString &p : r.problems)
            log(QStringLiteral("  ") + p);
    }
}

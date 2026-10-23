#include "photofetchdialog.h"

#include "marketapi.h"
#include "photofetch.h"
#include "photostore.h"

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

PhotoFetchDialog::PhotoFetchDialog(const QList<int> &selectedProductIds, QWidget *parent)
    : QDialog(parent), m_selected(selectedProductIds)
{
    setWindowTitle(tr("Найти фото"));
    resize(760, 620);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Программа подберёт фото товарам, у которых их ещё нет, и сохранит в карточки: их увидят все компьютеры и сайт. "
                                "Сначала берётся то, что уже есть на Яндекс Маркете (по артикулу на Маркете или артикулу) и в карточках Маркета; "
                                "для остальных товаров фото ищется в интернете по названию (размер в конце названия отбрасывается; "
                                "товары с одним названием ищутся один раз). Лучше подходят снимки на белом фоне."),
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
    m_onlyEmpty->setToolTip(tr("Если снять галочку, к уже имеющимся фото добавятся недостающие (не больше %1 на товар; такие же фото повторно не кладутся).")
                                .arg(PhotoStore::kMaxPhotosPerProduct));
    layout->addWidget(m_onlyEmpty);

    auto *countRow = new QHBoxLayout();
    countRow->addWidget(new QLabel(tr("Сколько фото добавлять каждому товару:"), this));
    m_perProduct = new QSpinBox(this);
    m_perProduct->setRange(1, PhotoStore::kMaxPhotosPerProduct);
    m_perProduct->setValue(1);
    countRow->addWidget(m_perProduct);
    countRow->addStretch();
    layout->addLayout(countRow);

    const int accounts = MarketAccount::loadAll().size();
    m_fromMarket = new QCheckBox(tr("Яндекс Маркет: спросить у Маркета (кабинетов: %1)").arg(accounts), this);
    m_fromMarket->setChecked(accounts > 0);
    m_fromMarket->setEnabled(accounts > 0);
    if (accounts == 0)
        m_fromMarket->setToolTip(tr("Кабинет Маркета не подключён: меню «Маркет» → «Кабинеты»."));
    m_fromCards = new QCheckBox(tr("Карточки Маркета в программе (вкладка «Карточки»): взять ссылки на фото"), this);
    m_fromCards->setChecked(true);
    m_fromWeb = new QCheckBox(tr("Интернет: искать картинки по названию товара (для тех, у кого фото нет на Маркете)"), this);
    m_fromWeb->setChecked(true);
    m_fromWeb->setToolTip(tr("Поиск идёт в Bing, при его отказе - в DuckDuckGo. Не больше одного запроса в полсекунды; "
                             "если поисковики несколько раз подряд не отвечают, поиск прекращается - повторите позже."));
    layout->addWidget(m_fromMarket);
    layout->addWidget(m_fromCards);
    layout->addWidget(m_fromWeb);

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
    m_startBtn = new QPushButton(tr("Найти и сохранить фото"), this);
    m_stopBtn = new QPushButton(tr("Остановить"), this);
    m_stopBtn->setEnabled(false);
    m_closeBtn = new QPushButton(tr("Закрыть"), this);
    bottom->addStretch();
    bottom->addWidget(m_startBtn);
    bottom->addWidget(m_stopBtn);
    bottom->addWidget(m_closeBtn);
    layout->addLayout(bottom);

    connect(m_startBtn, &QPushButton::clicked, this, &PhotoFetchDialog::start);
    connect(m_stopBtn, &QPushButton::clicked, this, [this]() { m_stop = true; m_stopBtn->setEnabled(false); });
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void PhotoFetchDialog::reject()
{
    if (m_running) { // закрыли окно во время работы: сначала останавливаемся
        m_stop = true;
        m_stopBtn->setEnabled(false);
        return;
    }
    QDialog::reject();
}

void PhotoFetchDialog::setBusy(bool busy)
{
    m_running = busy;
    for (QWidget *w : {static_cast<QWidget *>(m_toAll), static_cast<QWidget *>(m_onlyEmpty), static_cast<QWidget *>(m_perProduct),
                       static_cast<QWidget *>(m_fromCards), static_cast<QWidget *>(m_fromWeb), static_cast<QWidget *>(m_startBtn),
                       static_cast<QWidget *>(m_closeBtn)})
        w->setEnabled(!busy);
    m_toSelected->setEnabled(!busy && !m_selected.isEmpty());
    m_fromMarket->setEnabled(!busy && MarketAccount::loadAll().size() > 0);
    m_stopBtn->setEnabled(busy);
}

void PhotoFetchDialog::log(const QString &line)
{
    m_log->appendPlainText(line);
}

void PhotoFetchDialog::start()
{
    if (!m_fromMarket->isChecked() && !m_fromCards->isChecked() && !m_fromWeb->isChecked()) {
        m_status->setText(tr("Отметьте, откуда брать фото."));
        return;
    }
    PhotoFetch::Options opt;
    if (m_toSelected->isChecked())
        opt.productIds = m_selected;
    opt.onlyWithoutPhotos = m_onlyEmpty->isChecked();
    opt.fromMarket = m_fromMarket->isChecked();
    opt.fromCards = m_fromCards->isChecked();
    opt.fromWeb = m_fromWeb->isChecked();
    opt.perProduct = m_perProduct->value();

    m_log->clear();
    m_stop = false;
    m_bar->setRange(0, 0); // пока список собирается - бегунок без процентов
    m_status->setText(tr("Готовлю список товаров…"));
    setBusy(true);
    QApplication::processEvents();

    const PhotoFetch::Result r = PhotoFetch::run(opt, [this](int done, int total, const QString &text) {
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
    if (opt.fromMarket || opt.fromCards)
        log(tr("Фото нашлись на Маркете или в карточках: у %1 товаров").arg(r.withLinks));
    if (opt.fromWeb)
        log(tr("Фото нашлись в интернете: у %1 товаров").arg(r.fromWeb));
    log(tr("Добавлено фото: %1 (у %2 товаров)").arg(r.added).arg(r.productsFilled));
    if (r.duplicates > 0)
        log(tr("Пропущено, такие фото уже есть: %1").arg(r.duplicates));
    if (r.failed > 0)
        log(tr("Не удалось скачать или сохранить: %1").arg(r.failed));
    if (r.webBlocked)
        log(tr("Поиск в интернете остановлен: поисковики несколько раз подряд не ответили. Повторите позже - уже найденное останется, "
               "остальным товарам фото подберётся при следующем запуске."));
    for (const QString &e : r.webErrors)
        log(tr("Поисковик не ответил - %1").arg(e));
    if (!r.noLinks.isEmpty()) {
        const QStringList shown = r.noLinks.mid(0, 20);
        log(tr("Фото так и не нашлось (%1): %2%3").arg(r.noLinks.size()).arg(shown.join(QStringLiteral(", ")),
                                                                             r.noLinks.size() > shown.size() ? QStringLiteral(" …") : QString()));
        log(tr("Для них фото можно добавить вручную: «Фото…» или «Фото пачкой…»."));
    }
    for (const QString &e : r.marketErrors)
        log(tr("Маркет не ответил - %1").arg(e));
    if (!r.sources.isEmpty()) {
        log(QString());
        log(tr("Откуда взяты фото из интернета (проверьте, что это нужный товар; неподходящее фото можно заменить в карточке товара):"));
        for (const QString &s : r.sources)
            log(QStringLiteral("  ") + s);
    }
    if (!r.problems.isEmpty()) {
        log(QString());
        log(tr("Подробности ошибок:"));
        for (const QString &p : r.problems)
            log(QStringLiteral("  ") + p);
    }
}

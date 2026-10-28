#include "photofetchdialog.h"

#include "appsettings.h"
#include "marketapi.h"
#include "photoreviewdialog.h"
#include "photostore.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QSqlQuery>
#include <QVBoxLayout>

namespace {

const char kGoogleKey[] = "google_cse_key";
const char kGoogleCx[] = "google_cse_cx";

ImageSearch::Settings googleSettings()
{
    ImageSearch::Settings s;
    s.googleKey = AppSettings::get(QString::fromLatin1(kGoogleKey));
    s.googleCx = AppSettings::get(QString::fromLatin1(kGoogleCx));
    return s;
}

} // namespace

PhotoFetchDialog::PhotoFetchDialog(const QList<int> &selectedProductIds, QWidget *parent)
    : QDialog(parent), m_selected(selectedProductIds)
{
    setWindowTitle(tr("Найти фото"));
    resize(860, 760);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Программа подберёт фото товарам и сохранит в карточки: их увидят все компьютеры и сайт. Сначала берётся то, что уже есть "
                                "на Яндекс Маркете (по артикулу на Маркете или артикулу) и в карточках Маркета; для остальных товаров фото ищется в "
                                "интернете ПО АРТИКУЛУ: Poizon, Google, Wildberries. Снимок считается «проверенным», если артикул нашёлся в "
                                "названии, подписи или адресе страницы - остальное лучше смотреть глазами."),
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
    auto *targets = new QHBoxLayout();
    targets->addWidget(m_toSelected);
    targets->addWidget(m_toAll);
    targets->addStretch();
    layout->addLayout(targets);

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

    m_replace = new QCheckBox(tr("Заменять прежние фото: если для товара нашлись новые, его старые фото удаляются"), this);
    m_replace->setToolTip(tr("Удаляются только фото тех товаров, для которых найдено и сохранено новое. Если ничего не нашлось - старые остаются.\n"
                             "Включённая замена обрабатывает и товары, у которых фото уже есть."));
    layout->addWidget(m_replace);

    auto *sources = new QGroupBox(tr("Откуда брать фото"), this);
    auto *sl = new QVBoxLayout(sources);
    const int accounts = MarketAccount::loadAll().size();
    m_fromMarket = new QCheckBox(tr("Яндекс Маркет: спросить у Маркета (кабинетов: %1)").arg(accounts), sources);
    m_fromMarket->setChecked(accounts > 0);
    m_fromMarket->setEnabled(accounts > 0);
    if (accounts == 0)
        m_fromMarket->setToolTip(tr("Кабинет Маркета не подключён: меню «Маркет» → «Кабинеты»."));
    m_fromCards = new QCheckBox(tr("Карточки Маркета в программе (вкладка «Карточки»): взять ссылки на фото"), sources);
    m_fromCards->setChecked(true);
    m_fromPoizon = new QCheckBox(tr("Poizon: по артикулу (в первую очередь)"), sources);
    m_fromPoizon->setChecked(true);
    m_fromPoizon->setToolTip(tr("У Poizon нет открытого API: ищется на странице поиска сайта, а если она пустая - картинки сайта poizon.com через Google."));
    m_fromGoogle = new QCheckBox(tr("Google: картинки по артикулу"), sources);
    m_fromGoogle->setChecked(true);
    m_fromGoogle->setToolTip(tr("С ключом Google API поиск идёт официально (100 запросов в день бесплатно); без ключа разбирается обычная страница Google, "
                                "которая может потребовать подтверждение «я не робот»."));
    m_fromWb = new QCheckBox(tr("Wildberries: по артикулу (запасной источник)"), sources);
    m_fromWb->setChecked(true);
    sl->addWidget(m_fromMarket);
    sl->addWidget(m_fromCards);
    sl->addWidget(m_fromPoizon);
    sl->addWidget(m_fromGoogle);
    sl->addWidget(m_fromWb);
    auto *googleRow = new QHBoxLayout();
    m_googleState = new QLabel(sources);
    m_googleBtn = new QPushButton(tr("Ключ Google API..."), sources);
    googleRow->addWidget(m_googleState, 1);
    googleRow->addWidget(m_googleBtn);
    sl->addLayout(googleRow);
    layout->addWidget(sources);
    updateGoogleLabel();

    auto *modes = new QGroupBox(tr("Что делать с найденным"), this);
    auto *ml = new QVBoxLayout(modes);
    m_modePreview = new QRadioButton(tr("Сначала показать найденное и выбрать самому (рекомендуется; за раз до %1 товаров)").arg(PhotoFetch::kMaxProposals), modes);
    m_modePreview->setChecked(true);
    m_modeDirect = new QRadioButton(tr("Сохранять сразу - только проверенные снимки (для больших партий)"), modes);
    m_acceptUnverified = new QCheckBox(tr("и непроверенные тоже (артикул нигде не упомянут - возможны неподходящие фото)"), modes);
    m_acceptUnverified->setEnabled(false);
    ml->addWidget(m_modePreview);
    ml->addWidget(m_modeDirect);
    ml->addWidget(m_acceptUnverified);
    layout->addWidget(modes);
    connect(m_modeDirect, &QRadioButton::toggled, m_acceptUnverified, [this](bool on) {
        m_acceptUnverified->setEnabled(on);
        if (!on)
            m_acceptUnverified->setChecked(false);
    });
    connect(m_replace, &QCheckBox::toggled, this, [this](bool on) { m_onlyEmpty->setEnabled(!on); });

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
    m_diagBtn = new QPushButton(tr("Проверить источники..."), this);
    m_diagBtn->setToolTip(tr("Спросить все включённые источники интернета по одному артикулу и показать, что каждый ответил - "
                             "если ничего не находится, этот отчёт покажет почему."));
    m_startBtn = new QPushButton(tr("Найти фото"), this);
    m_stopBtn = new QPushButton(tr("Остановить"), this);
    m_stopBtn->setEnabled(false);
    m_closeBtn = new QPushButton(tr("Закрыть"), this);
    bottom->addWidget(m_diagBtn);
    bottom->addStretch();
    bottom->addWidget(m_startBtn);
    bottom->addWidget(m_stopBtn);
    bottom->addWidget(m_closeBtn);
    layout->addLayout(bottom);

    for (QPushButton *b : {m_googleBtn, m_diagBtn, m_stopBtn, m_closeBtn})
        b->setAutoDefault(false); // Enter не должен нажимать вспомогательные кнопки
    m_startBtn->setAutoDefault(false);
    connect(m_startBtn, &QPushButton::clicked, this, &PhotoFetchDialog::start);
    connect(m_googleBtn, &QPushButton::clicked, this, &PhotoFetchDialog::editGoogle);
    connect(m_diagBtn, &QPushButton::clicked, this, &PhotoFetchDialog::diagnose);
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

void PhotoFetchDialog::updateGoogleLabel()
{
    const ImageSearch::Settings g = googleSettings();
    m_googleState->setText(g.hasGoogleApi() ? tr("Google API: ключ задан (поиск через официальный API)")
                                            : tr("Google API: ключ не задан (разбирается обычная страница Google - может не сработать)"));
}

void PhotoFetchDialog::setBusy(bool busy)
{
    m_running = busy;
    for (QWidget *w : {static_cast<QWidget *>(m_toAll), static_cast<QWidget *>(m_onlyEmpty), static_cast<QWidget *>(m_perProduct),
                       static_cast<QWidget *>(m_replace), static_cast<QWidget *>(m_fromCards), static_cast<QWidget *>(m_fromPoizon),
                       static_cast<QWidget *>(m_fromGoogle), static_cast<QWidget *>(m_fromWb), static_cast<QWidget *>(m_googleBtn),
                       static_cast<QWidget *>(m_modePreview), static_cast<QWidget *>(m_modeDirect), static_cast<QWidget *>(m_startBtn),
                       static_cast<QWidget *>(m_diagBtn), static_cast<QWidget *>(m_closeBtn)})
        w->setEnabled(!busy);
    m_onlyEmpty->setEnabled(!busy && !m_replace->isChecked());
    m_acceptUnverified->setEnabled(!busy && m_modeDirect->isChecked());
    m_toSelected->setEnabled(!busy && !m_selected.isEmpty());
    m_fromMarket->setEnabled(!busy && MarketAccount::loadAll().size() > 0);
    m_stopBtn->setEnabled(busy);
}

void PhotoFetchDialog::log(const QString &line)
{
    m_log->appendPlainText(line);
}

void PhotoFetchDialog::editGoogle()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Ключ Google API"));
    auto *l = new QVBoxLayout(&dlg);
    auto *hint = new QLabel(tr("Официальный поиск картинок Google (Custom Search JSON API): 100 запросов в день бесплатно.\n"
                               "1. В Google Cloud Console создайте проект и включите «Custom Search API», создайте ключ API.\n"
                               "2. На programmablesearchengine.google.com создайте поисковую систему «искать во всём интернете» с поиском "
                               "картинок и скопируйте её идентификатор (cx).\n"
                               "Ключ хранится в общей базе - вводить его на каждом компьютере не нужно."),
                            &dlg);
    hint->setWordWrap(true);
    l->addWidget(hint);
    auto *form = new QFormLayout();
    const ImageSearch::Settings g = googleSettings();
    auto *key = new QLineEdit(g.googleKey, &dlg);
    key->setEchoMode(QLineEdit::Password);
    auto *cx = new QLineEdit(g.googleCx, &dlg);
    form->addRow(tr("Ключ API:"), key);
    form->addRow(tr("Идентификатор поисковой системы (cx):"), cx);
    l->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    l->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    AppSettings::set(QString::fromLatin1(kGoogleKey), key->text().trimmed());
    AppSettings::set(QString::fromLatin1(kGoogleCx), cx->text().trimmed());
    updateGoogleLabel();
}

PhotoFetch::Options PhotoFetchDialog::options() const
{
    PhotoFetch::Options opt;
    if (m_toSelected->isChecked())
        opt.productIds = m_selected;
    opt.onlyWithoutPhotos = m_onlyEmpty->isChecked();
    opt.fromMarket = m_fromMarket->isChecked();
    opt.fromCards = m_fromCards->isChecked();
    opt.webSources = (m_fromPoizon->isChecked() ? ImageSearch::Poizon : 0) | (m_fromGoogle->isChecked() ? ImageSearch::Google : 0)
                     | (m_fromWb->isChecked() ? ImageSearch::Wildberries : 0);
    opt.fromWeb = opt.webSources != 0;
    opt.google = googleSettings();
    opt.perProduct = m_perProduct->value();
    opt.replaceExisting = m_replace->isChecked();
    opt.acceptUnverified = m_modeDirect->isChecked() && m_acceptUnverified->isChecked();
    return opt;
}

void PhotoFetchDialog::diagnose()
{
    QString suggestion;
    if (!m_selected.isEmpty()) {
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT COALESCE(NULLIF(TRIM(market_sku), ''), sku) FROM products WHERE id = ?"));
        q.addBindValue(m_selected.first());
        if (q.exec() && q.next())
            suggestion = q.value(0).toString();
    }
    bool ok = false;
    const QString article = QInputDialog::getText(this, tr("Проверить источники"), tr("Артикул товара (например DD1391-100):"), QLineEdit::Normal, suggestion, &ok).trimmed();
    if (!ok || article.isEmpty())
        return;
    const PhotoFetch::Options opt = options();
    if (opt.webSources == 0) {
        m_status->setText(tr("Отметьте хотя бы один источник в интернете."));
        return;
    }
    m_log->clear();
    m_stop = false;
    setBusy(true);
    log(tr("Артикул: %1; варианты поиска: %2").arg(article, ImageSearch::articleVariants(article).join(QStringLiteral(" | "))));
    for (const ImageSearch::Source src : {ImageSearch::Poizon, ImageSearch::Google, ImageSearch::Wildberries}) {
        if (!(opt.webSources & src) || m_stop)
            continue;
        m_status->setText(tr("Спрашиваю: %1…").arg(ImageSearch::sourceName(src)));
        QApplication::processEvents();
        log(QString());
        log(QStringLiteral("=== %1").arg(ImageSearch::sourceName(src)));
        const ImageSearch::Reply r = ImageSearch::search(src, article, opt.google, [this]() {
            QApplication::processEvents();
            return m_stop;
        });
        for (const QString &t : r.trace)
            log(QStringLiteral("  ") + t);
        if (!r.error.isEmpty())
            log(tr("  ОШИБКА: %1").arg(r.error));
        int verified = 0;
        for (const ImageSearch::Hit &h : r.hits)
            verified += h.verified;
        log(tr("  Итого: картинок %1, проверенных %2").arg(r.hits.size()).arg(verified));
        for (int i = 0; i < qMin(5, int(r.hits.size())); ++i)
            log(QStringLiteral("    %1 %2").arg(r.hits[i].verified ? QStringLiteral("[проверено]") : QStringLiteral("[не проверено]"), r.hits[i].url));
    }
    setBusy(false);
    m_status->setText(m_stop ? tr("Остановлено.") : tr("Проверка закончена."));
}

void PhotoFetchDialog::reportResult(const PhotoFetch::Result &r, const PhotoFetch::Options &opt, bool saved)
{
    log(tr("Товаров проверено: %1").arg(r.considered));
    if (r.skippedForPreview > 0)
        log(tr("В просмотр взяты первые %1 товаров, ещё %2 - при следующем запуске (или сохраняйте сразу).").arg(PhotoFetch::kMaxProposals).arg(r.skippedForPreview));
    if (opt.fromMarket || opt.fromCards)
        log(tr("Фото нашлись на Маркете или в карточках: у %1 товаров").arg(r.withLinks));
    if (opt.fromWeb && saved)
        log(tr("Фото нашлись в интернете: у %1 товаров").arg(r.fromWeb));
    if (saved) {
        log(tr("Добавлено фото: %1 (у %2 товаров)").arg(r.added).arg(r.productsFilled));
        if (r.replaced > 0)
            log(tr("Удалено прежних фото при замене: %1").arg(r.replaced));
    }
    if (r.duplicates > 0)
        log(tr("Пропущено, такие фото уже есть: %1").arg(r.duplicates));
    if (r.failed > 0)
        log(tr("Не удалось скачать или сохранить: %1").arg(r.failed));
    if (r.webBlocked)
        log(tr("Поиск в интернете остановлен: источники несколько раз подряд не ответили. Повторите позже - уже найденное останется, "
               "остальным товарам фото подберётся при следующем запуске. Кнопка «Проверить источники...» покажет, что именно ответил каждый."));
    for (const QString &e : r.webErrors)
        log(tr("Источник не ответил - %1").arg(e));
    if (!r.unverified.isEmpty()) {
        const QStringList shown = r.unverified.mid(0, 20);
        log(tr("Нашлись только непроверенные снимки (артикул нигде не упомянут), не сохранены (%1): %2%3").arg(r.unverified.size()).arg(shown.join(QStringLiteral(", ")),
                                                                                                         r.unverified.size() > shown.size() ? QStringLiteral(" …") : QString()));
        log(tr("Чтобы выбрать из них самому, запустите поиск в режиме просмотра."));
    }
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

void PhotoFetchDialog::start()
{
    if (!m_fromMarket->isChecked() && !m_fromCards->isChecked() && !m_fromPoizon->isChecked() && !m_fromGoogle->isChecked() && !m_fromWb->isChecked()) {
        m_status->setText(tr("Отметьте, откуда брать фото."));
        return;
    }
    const PhotoFetch::Options opt = options();
    const bool preview = m_modePreview->isChecked();

    m_log->clear();
    m_stop = false;
    m_bar->setRange(0, 0); // пока список собирается - бегунок без процентов
    m_status->setText(tr("Готовлю список товаров…"));
    setBusy(true);
    QApplication::processEvents();

    const PhotoFetch::Progress progress = [this](int done, int total, const QString &text) {
        if (total > 0) {
            m_bar->setRange(0, total);
            m_bar->setValue(done);
        }
        if (!text.isEmpty())
            m_status->setText(text);
        QApplication::processEvents(); // перерисовка и кнопка «Остановить»
        return !m_stop;
    };

    PhotoFetch::Result r;
    QList<PhotoFetch::Proposal> proposals;
    if (preview)
        r = PhotoFetch::collect(opt, &proposals, progress);
    else
        r = PhotoFetch::run(opt, progress);
    setBusy(false);
    m_bar->setRange(0, 1);
    m_bar->setValue(r.cancelled ? 0 : 1);

    if (r.considered == 0) {
        m_status->setText(tr("Нет подходящих товаров."));
        log(opt.onlyWithoutPhotos && !opt.replaceExisting
                ? tr("Среди выбранных нет товаров без фото. Снимите галочку «Только товарам, у которых ещё нет фото», "
                     "чтобы добавить недостающие фото и тем, у кого они есть.")
                : tr("Нет товаров, которым можно добавить фото (у всех уже %1).").arg(PhotoStore::kMaxPhotosPerProduct));
        return;
    }

    if (!preview) {
        if (r.added > 0)
            m_changed = true;
        m_status->setText(r.cancelled ? tr("Остановлено. Добавлено фото: %1.").arg(r.added) : tr("Готово. Добавлено фото: %1.").arg(r.added));
        reportResult(r, opt, true);
        return;
    }

    // Режим просмотра: показываем найденное, сохраняем выбранное.
    if (proposals.isEmpty()) {
        m_status->setText(r.cancelled ? tr("Остановлено.") : tr("Ничего не найдено."));
        reportResult(r, opt, false);
        return;
    }
    m_status->setText(tr("Найдено для %1 товаров - выберите фото.").arg(proposals.size()));
    PhotoReviewDialog review(proposals, opt.perProduct, opt.replaceExisting, this);
    if (review.exec() != QDialog::Accepted || review.choices().isEmpty()) {
        m_status->setText(tr("Ничего не сохранено."));
        reportResult(r, opt, false);
        log(tr("Найденное не сохранено."));
        return;
    }
    const PhotoFetch::Result saved = PhotoFetch::save(review.choices(), review.replaceExisting());
    PhotoFetch::Result merged = r;
    merged.added = saved.added;
    merged.replaced = saved.replaced;
    merged.productsFilled = saved.productsFilled;
    merged.duplicates += saved.duplicates;
    merged.failed += saved.failed;
    merged.fromWeb = 0;
    for (const QString &p : saved.problems)
        if (merged.problems.size() < PhotoFetch::kMaxProblems)
            merged.problems << p;
    if (saved.added > 0)
        m_changed = true;
    m_status->setText(tr("Готово. Добавлено фото: %1.").arg(saved.added));
    reportResult(merged, opt, true);
}

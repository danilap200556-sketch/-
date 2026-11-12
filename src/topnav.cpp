#include "topnav.h"

#include "theme.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QVariantAnimation>
#include <QVBoxLayout>

namespace {

constexpr int kTabPadding = 14;   // как padding у QPushButton#navTab: линия - под текстом вкладки, а не на всю её ширину
constexpr int kLineMs = 240;

// Линия под выбранной вкладкой. Цвет берётся из темы при каждой отрисовке, поэтому смена темы ничего не требует.
class TabLine : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Theme::colors().accent);
    }
};

QString groupCaption(const QString &name)
{
    QString s = name.trimmed().toLower();
    if (!s.isEmpty())
        s[0] = s[0].toUpper();
    return s;
}

} // namespace

// Полоса вкладок: кнопки в ряд и линия под выбранной. Линия - отдельный виджет поверх кнопок (иначе подсветка
// вкладки под курсором закрывала бы её) и переезжает к новой вкладке за ~0,25 с.
class TabStrip : public QWidget
{
public:
    explicit TabStrip(QWidget *parent)
        : QWidget(parent)
    {
        m_layout = new QHBoxLayout(this);
        m_layout->setContentsMargins(10, 0, 10, 0);
        m_layout->setSpacing(2);
        m_layout->addStretch(1);
        m_line = new TabLine(this);
        m_line->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_line->hide();
        m_anim = new QVariantAnimation(this);
        m_anim->setStartValue(0.0);
        m_anim->setEndValue(1.0);
        m_anim->setDuration(kLineMs);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &) { place(); });
        connect(m_anim, &QVariantAnimation::finished, this, [this]() { place(); });
    }

    void addTab(QPushButton *tab)
    {
        m_layout->insertWidget(m_layout->count() - 1, tab);
        tab->installEventFilter(this);
    }

    // grow - линия «вырастает» слева у новой вкладки (сменилась группа), иначе переезжает со старой.
    void setCurrent(QPushButton *tab, bool grow)
    {
        const QRectF before = m_line->isVisible() ? QRectF(m_line->geometry()) : QRectF();
        m_tab = tab;
        m_layout->activate();
        const QRectF to = target();
        m_anim->stop();
        if (!Theme::animations() || !isVisible() || to.width() <= 0) {
            place();
            return;
        }
        m_from = (grow || before.width() <= 0) ? QRectF(to.left(), to.top(), 0, to.height()) : before;
        m_anim->start();
    }

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (o == m_tab && (e->type() == QEvent::Resize || e->type() == QEvent::Move || e->type() == QEvent::Show) &&
            m_anim->state() != QAbstractAnimation::Running)
            place();
        return false;
    }

    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        if (m_anim->state() != QAbstractAnimation::Running)
            place();
    }

private:
    QRectF target() const
    {
        if (!m_tab || !m_tab->isVisibleTo(this))
            return {};
        const QRect g = m_tab->geometry();
        return QRectF(g.left() + kTabPadding, height() - 2, qMax(0, g.width() - 2 * kTabPadding), 2);
    }

    void place()
    {
        const QRectF to = target();
        if (to.width() <= 0) {
            m_line->hide();
            return;
        }
        QRectF r = to;
        if (m_anim->state() == QAbstractAnimation::Running) {
            const qreal t = m_anim->currentValue().toReal();
            r = QRectF(m_from.left() + (to.left() - m_from.left()) * t, to.top(), m_from.width() + (to.width() - m_from.width()) * t, to.height());
        }
        m_line->setGeometry(r.toAlignedRect());
        m_line->show();
        m_line->raise();
    }

    QHBoxLayout *m_layout = nullptr;
    TabLine *m_line = nullptr;
    QVariantAnimation *m_anim = nullptr;
    QPushButton *m_tab = nullptr;
    QRectF m_from;
};

TopNav::TopNav(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_header = new QFrame(this);
    m_header->setObjectName(QStringLiteral("appHeader"));
    auto *h = new QHBoxLayout(m_header);
    h->setContentsMargins(22, 10, 22, 10);
    h->setSpacing(6);

    auto *logo = new QLabel(m_header);
    logo->setPixmap(Theme::logo(36));
    logo->setFixedSize(36, 36);
    h->addWidget(logo);
    h->addSpacing(6);
    auto *brand = new QVBoxLayout();
    brand->setSpacing(0);
    auto *brandTitle = new QLabel(tr("Склад"), m_header);
    QFont bf = brandTitle->font();
    bf.setPointSizeF(12.5);
    bf.setBold(true);
    brandTitle->setFont(bf);
    auto *brandSub = new QLabel(tr("учёт и Маркет"), m_header);
    brandSub->setObjectName(QStringLiteral("headerMuted"));
    brand->addWidget(brandTitle);
    brand->addWidget(brandSub);
    h->addLayout(brand);
    h->addSpacing(22);

    m_groupsLayout = new QHBoxLayout();
    m_groupsLayout->setSpacing(2);
    h->addLayout(m_groupsLayout);
    h->addStretch(1);
    m_trailing = new QHBoxLayout();
    m_trailing->setSpacing(10);
    h->addLayout(m_trailing);
    root->addWidget(m_header);

    auto *stripFrame = new QFrame(this);
    stripFrame->setObjectName(QStringLiteral("tabStripFrame"));
    auto *sl = new QVBoxLayout(stripFrame);
    sl->setContentsMargins(0, 0, 0, 0);
    m_strip = new TabStrip(stripFrame);
    sl->addWidget(m_strip);
    root->addWidget(stripFrame);
}

int TopNav::groupIndex(const QString &name)
{
    const QString caption = groupCaption(name);
    for (int i = 0; i < m_groups.size(); ++i) {
        if (m_groups[i].name == caption)
            return i;
    }
    Group g;
    g.name = caption;
    g.button = new QPushButton(caption, m_header);
    g.button->setObjectName(QStringLiteral("navGroup"));
    g.button->setProperty("primary", false); // роль кнопки по подписи (ButtonRoles) здесь не нужна
    g.button->setCheckable(true);
    g.button->setCursor(Qt::PointingHandCursor);
    const int index = m_groups.size();
    connect(g.button, &QPushButton::clicked, this, [this, index]() {
        const Group &grp = m_groups[index];
        int page = grp.lastPage;
        if (page < 0) {
            for (int i = 0; i < m_pages.size() && page < 0; ++i) {
                if (m_pages[i].group == index)
                    page = i;
            }
        }
        if (page < 0 || page == m_current) {
            grp.button->setChecked(index == (m_current >= 0 ? m_pages[m_current].group : -1));
            return;
        }
        setCurrentIndex(page);
    });
    m_groupsLayout->addWidget(g.button);
    m_groups << g;
    return index;
}

int TopNav::addPage(const QString &title, const QString &group)
{
    int g;
    if (!group.isEmpty())
        g = groupIndex(group);
    else if (!m_pages.isEmpty())
        g = m_pages.last().group;
    else
        g = groupIndex(tr("Разделы"));

    Page p;
    p.title = title;
    p.group = g;
    p.tab = new QPushButton(title, m_strip);
    p.tab->setObjectName(QStringLiteral("navTab"));
    p.tab->setProperty("primary", false);
    p.tab->setCheckable(true);
    p.tab->setCursor(Qt::PointingHandCursor);
    const int index = m_pages.size();
    connect(p.tab, &QPushButton::clicked, this, [this, index]() {
        if (index == m_current) {
            m_pages[index].tab->setChecked(true);
            return;
        }
        setCurrentIndex(index);
    });
    m_strip->addTab(p.tab);
    p.tab->setVisible(m_shownGroup < 0 || m_shownGroup == g);
    if (m_shownGroup < 0)
        m_shownGroup = g;
    m_pages << p;
    return index;
}

void TopNav::showGroup(int group)
{
    m_shownGroup = group;
    for (const Page &p : std::as_const(m_pages))
        p.tab->setVisible(p.group == group);
}

void TopNav::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_pages.size())
        return;
    const int prevGroup = m_current >= 0 ? m_pages[m_current].group : -1;
    m_current = index;
    const int g = m_pages[index].group;
    m_groups[g].lastPage = index;
    for (int i = 0; i < m_groups.size(); ++i)
        m_groups[i].button->setChecked(i == g);
    if (g != m_shownGroup)
        showGroup(g);
    for (int i = 0; i < m_pages.size(); ++i)
        m_pages[i].tab->setChecked(i == index);
    m_strip->setCurrent(m_pages[index].tab, g != prevGroup);
    emit currentChanged(index);
}

QString TopNav::title(int index) const
{
    return index >= 0 && index < m_pages.size() ? m_pages[index].title : QString();
}

void TopNav::setBadge(int index, bool on)
{
    if (index < 0 || index >= m_pages.size() || m_pages[index].badge == on)
        return;
    m_pages[index].badge = on;
    updateBadges();
}

void TopNav::updateBadges()
{
    static const QString dot = QStringLiteral("  ●");
    for (Page &p : m_pages)
        p.tab->setText(p.badge ? p.title + dot : p.title);
    for (int g = 0; g < m_groups.size(); ++g) {
        bool any = false;
        for (const Page &p : std::as_const(m_pages))
            any = any || (p.group == g && p.badge);
        m_groups[g].button->setText(any ? m_groups[g].name + dot : m_groups[g].name);
    }
}

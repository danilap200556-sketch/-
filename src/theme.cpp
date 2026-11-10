#include "theme.h"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QStyle>
#include <QRegularExpression>
#include <QPushButton>
#include <QEvent>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QtMath>
#include <QPair>

namespace Theme {

namespace {

Mode g_mode = Mode::Light;

// Галочка, точка и стрелка списка рисуются кодом и кладутся в кэш-папку: в таблице стилей картинку можно указать только путём к файлу.
enum class Indicator { Check, Dot, Chevron };
QString indicatorImage(Indicator kind, const QColor &color = Qt::white)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/theme");
    QDir().mkpath(dir);
    const QString name = kind == Indicator::Dot ? QStringLiteral("dot") : (kind == Indicator::Check ? QStringLiteral("check") : QStringLiteral("chevron"));
    const QString path = QStringLiteral("%1/%2-%3.png").arg(dir, name, color.name().mid(1));
    if (!QFile::exists(path)) {
        QPixmap pm(36, 36);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        if (kind == Indicator::Dot) {
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawEllipse(QPointF(18, 18), 5.5, 5.5);
        } else if (kind == Indicator::Check) {
            p.setPen(QPen(color, 3.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(QPolygonF({QPointF(9.5, 18.5), QPointF(15.5, 24.5), QPointF(26.5, 11.5)}));
        } else {
            p.setPen(QPen(color, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(QPolygonF({QPointF(10, 14), QPointF(18, 22), QPointF(26, 14)}));
        }
        p.end();
        pm.save(path, "PNG");
    }
    return path;
}
QString rgba(const QColor &c, int alpha)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}
Colors g_colors;

Colors lightColors()
{
    Colors c;
    c.bg = "#f3f4f8"; c.surface = "#ffffff"; c.surface2 = "#f7f8fb"; c.border = "#e2e5ed"; c.borderStrong = "#c8cedb";
    c.text = "#0f1626"; c.muted = "#5c6882"; c.accent = "#2457d6"; c.accentHover = "#1b45b4"; c.accentText = "#ffffff";
    c.danger = "#c4313c"; c.ok = "#1e8559"; c.selection = "#e4ebfb"; c.scroll = "#c3cada"; c.sidebar = "#0f172a";
    c.warn = "#9c6200"; c.accentSoft = "#e8eefc"; c.okSoft = "#e6f4ed"; c.dangerSoft = "#fceaec"; c.warnSoft = "#fbf0dc";
    return c;
}

Colors darkColors()
{
    Colors c;
    c.bg = "#0e121a"; c.surface = "#151b26"; c.surface2 = "#1a2230"; c.border = "#252e3e"; c.borderStrong = "#364156";
    c.text = "#e6eaf2"; c.muted = "#8b97ad"; c.accent = "#5b8cff"; c.accentHover = "#7aa2ff"; c.accentText = "#0b1020";
    c.danger = "#ff7782"; c.ok = "#4cc790"; c.selection = "#22305a"; c.scroll = "#364156"; c.sidebar = "#0a0d14";
    c.warn = "#f0b04a"; c.accentSoft = "#1d2a4d"; c.okSoft = "#15342a"; c.dangerSoft = "#3a2127"; c.warnSoft = "#3a2f18";
    return c;
}

QString css(const Colors &c)
{
    QString s = QStringLiteral(R"(
* { font-family: "Segoe UI Variable Text", "Segoe UI", "Inter", "Noto Sans", sans-serif; }
QWidget { color: @text@; font-size: 10pt; }
QMainWindow, QDialog { background: @bg@; }
QWidget#content, QWidget#page { background: @bg@; }
QLabel { background: transparent; }
QLabel#pageTitle { font-size: 18pt; font-weight: 600; }
QLabel#muted { color: @muted@; }
QToolTip { background: @surface@; color: @text@; border: 1px solid @borderStrong@; border-radius: 5px; padding: 5px 8px; }

QMenuBar { background: @surface@; border-bottom: 1px solid @border@; padding: 2px 8px; }
QMenuBar::item { background: transparent; padding: 6px 12px; border-radius: 5px; }
QMenuBar::item:selected { background: @surface2@; }
QMenu { background: @surface@; border: 1px solid @borderStrong@; border-radius: 8px; padding: 5px; }
QMenu::item { padding: 7px 26px 7px 12px; border-radius: 5px; }
QMenu::item:selected { background: @selection@; color: @text@; }
QMenu::separator { height: 1px; background: @border@; margin: 5px 8px; }

QPushButton { background: @surface@; border: 1px solid @borderStrong@; border-radius: 6px; padding: 5px 14px; min-height: 20px; }
QPushButton:hover { background: @surface2@; border-color: @muted@; }
QPushButton:pressed { background: @border@; }
QPushButton:disabled { color: @muted@; background: @surface2@; border-color: @border@; }
QPushButton:default, QPushButton[primary="true"] { background: @accent@; border-color: @accent@; color: @accentText@; font-weight: 600; }
QPushButton:default:hover, QPushButton[primary="true"]:hover { background: @accentHover@; border-color: @accentHover@; }
QPushButton:default:disabled, QPushButton[primary="true"]:disabled { background: @surface2@; color: @muted@; border-color: @border@; }
QPushButton[danger="true"] { background: @surface@; color: @danger@; border-color: @dangerLine@; }
QPushButton[danger="true"]:hover { background: @dangerSoft@; border-color: @danger@; }
QPushButton[danger="true"]:disabled { color: @muted@; background: @surface2@; border-color: @border@; }
QToolButton { background: transparent; border: none; border-radius: 6px; padding: 5px; }
QToolButton:hover { background: @surface2@; }

QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QDateEdit, QComboBox {
  background: @surface@; border: 1px solid @borderStrong@; border-radius: 6px; padding: 5px 9px; min-height: 20px;
  selection-background-color: @accent@; selection-color: @accentText@; }
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QDateEdit:hover, QComboBox:hover { border-color: @muted@; }
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QDateEdit:focus, QComboBox:focus, QComboBox:on { border: 1px solid @accent@; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color: @muted@; background: @surface2@; border-color: @border@; }
QLineEdit[readOnly="true"] { background: @surface2@; }
QComboBox { padding-right: 28px; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 26px; border: none; }
QComboBox::down-arrow { image: url(@chevron@); width: 12px; height: 12px; }
QComboBox QAbstractItemView { background: @surface@; border: 1px solid @borderStrong@; border-radius: 6px; padding: 3px;
  selection-background-color: @selection@; selection-color: @text@; outline: 0; }
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 18px; border: none; background: transparent; }

QCheckBox, QRadioButton { spacing: 8px; background: transparent; }
QCheckBox::indicator, QRadioButton::indicator { width: 17px; height: 17px; border: 1.5px solid @borderStrong@; background: @surface@; }
QCheckBox::indicator { border-radius: 4px; }
QRadioButton::indicator { border-radius: 9px; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: @accent@; }
QCheckBox::indicator:checked { background: @accent@; border-color: @accent@; image: url(@check@); }
QRadioButton::indicator:checked { background: @accent@; border-color: @accent@; image: url(@dot@); }
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { background: @surface2@; border-color: @border@; }
QTableView::indicator { width: 17px; height: 17px; border: 1.5px solid @borderStrong@; border-radius: 4px; background: @surface@; }
QTableView::indicator:checked { background: @accent@; border-color: @accent@; image: url(@check@); }

QTableView, QTableWidget, QListWidget, QTreeWidget, QTreeView {
  background: @surface@; alternate-background-color: @surface2@; gridline-color: transparent; border: 1px solid @border@; border-radius: 8px;
  selection-background-color: @selection@; selection-color: @text@; outline: 0; }
QTableView::item, QTableWidget::item { padding: 3px 8px; border: none; }
QTableView::item:hover, QTableWidget::item:hover { background: @accentSoft@; }
QTableView::item:selected, QTableWidget::item:selected { background: @selection@; color: @text@; }
QListWidget::item, QTreeWidget::item { padding: 4px 6px; }
QHeaderView { background: transparent; }
QHeaderView::section { background: @surface@; color: @muted@; border: none; border-bottom: 1px solid @borderStrong@; padding: 9px 10px; font-weight: 600; font-size: 9pt; }
QTableCornerButton::section { background: @surface@; border: none; border-bottom: 1px solid @borderStrong@; }

QScrollBar:vertical { width: 12px; background: transparent; margin: 3px; }
QScrollBar::handle:vertical { background: @scroll@; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @muted@; }
QScrollBar:horizontal { height: 12px; background: transparent; margin: 3px; }
QScrollBar::handle:horizontal { background: @scroll@; border-radius: 4px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: @muted@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QGroupBox { background: @surface@; border: 1px solid @border@; border-radius: 8px; margin-top: 16px; padding: 14px 12px 10px 12px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 12px; padding: 0 6px; color: @muted@; font-weight: 600; background: @bg@; border-radius: 4px; }

QTabWidget::pane { border: none; top: -1px; }
QTabBar::tab { background: transparent; color: @muted@; padding: 9px 16px; border: none; border-bottom: 2px solid transparent; margin-right: 4px; font-weight: 600; }
QTabBar::tab:hover { color: @text@; }
QTabBar::tab:selected { color: @accent@; border-bottom: 2px solid @accent@; }

QSplitter::handle { background: transparent; }
QProgressBar { background: @surface2@; border: 1px solid @border@; border-radius: 5px; text-align: center; height: 10px; }
QProgressBar::chunk { background: @accent@; border-radius: 4px; }
QDialogButtonBox QPushButton { min-width: 84px; }
QMessageBox { background: @bg@; }

/* Плашки-сообщения: успех / ошибка / предупреждение / сведения. Свойство banner = ok | error | warn | info. */
QLabel[banner="ok"], QLabel#noticeBanner, QLabel#printStatus { background: @okSoft@; color: @ok@; border: 1px solid @okLine@; border-radius: 6px; padding: 8px 12px; font-weight: 600; }
QLabel[banner="error"], QLabel#printStatus[error="true"] { background: @dangerSoft@; color: @danger@; border: 1px solid @dangerLine@; border-radius: 6px; padding: 8px 12px; font-weight: 600; }
QLabel[banner="warn"] { background: @warnSoft@; color: @warn@; border: 1px solid @warnLine@; border-radius: 6px; padding: 8px 12px; font-weight: 600; }
QLabel[banner="info"] { background: @accentSoft@; color: @accent@; border: 1px solid @accentLine@; border-radius: 6px; padding: 8px 12px; font-weight: 600; }

/* Полоса с количеством пар на складах */
QFrame#statCard { background: @surface@; border: 1px solid @border@; border-radius: 8px; }
QFrame#statCard[total="true"] { background: @accent@; border-color: @accent@; }
QLabel#statCaption { color: @muted@; font-size: 8pt; font-weight: 600; }
QLabel#statValue { font-size: 14pt; font-weight: 700; }
QFrame#statCard[total="true"] QLabel#statCaption { color: @accentText@; }
QFrame#statCard[total="true"] QLabel#statValue { color: @accentText@; }

QFrame#sidebarFrame { background: @sidebar@; border: none; }
QFrame#sidebarFrame QLabel { color: #e9edf7; }
QFrame#sidebarFrame QLabel#sideMuted { color: #8591ab; font-size: 9pt; }
QFrame#userCard { background: rgba(255,255,255,0.05); border: 1px solid rgba(255,255,255,0.06); border-radius: 8px; }
QListWidget#sidebar { background: transparent; border: none; outline: 0; padding: 4px 8px; }
QListWidget#sidebar::item { color: #b4bfd3; padding: 9px 12px 9px 14px; margin: 1px 0; border-radius: 6px; }
QListWidget#sidebar::item:hover { background: rgba(255,255,255,0.06); color: #ffffff; }
QListWidget#sidebar::item:selected { background: rgba(255,255,255,0.10); color: #ffffff; }
QFrame#sidebarFrame QToolButton { color: #b4bfd3; }
QFrame#sidebarFrame QToolButton:hover { background: rgba(255,255,255,0.10); }
)");
    const QPair<QString, QColor> tokens[] = {
        {"@bg@", c.bg}, {"@surface2@", c.surface2}, {"@surface@", c.surface}, {"@borderStrong@", c.borderStrong}, {"@border@", c.border},
        {"@text@", c.text}, {"@muted@", c.muted}, {"@accentHover@", c.accentHover}, {"@accentText@", c.accentText},
        {"@accentSoft@", c.accentSoft}, {"@accent@", c.accent}, {"@selection@", c.selection}, {"@scroll@", c.scroll}, {"@sidebar@", c.sidebar},
        {"@okSoft@", c.okSoft}, {"@ok@", c.ok}, {"@dangerSoft@", c.dangerSoft}, {"@danger@", c.danger}, {"@warnSoft@", c.warnSoft}, {"@warn@", c.warn}};
    // Сначала «длинные» метки с цветом-приставкой (@okLine@ и т. п.), иначе @ok@ съест их начало.
    s.replace(QStringLiteral("@okLine@"), rgba(c.ok, 90));
    s.replace(QStringLiteral("@dangerLine@"), rgba(c.danger, 100));
    s.replace(QStringLiteral("@warnLine@"), rgba(c.warn, 100));
    s.replace(QStringLiteral("@accentLine@"), rgba(c.accent, 90));
    for (const auto &t : tokens)
        s.replace(t.first, t.second.name());
    s.replace(QStringLiteral("@check@"), indicatorImage(Indicator::Check));
    s.replace(QStringLiteral("@dot@"), indicatorImage(Indicator::Dot));
    s.replace(QStringLiteral("@chevron@"), indicatorImage(Indicator::Chevron, c.muted));
    return s;
}


// Роли кнопок по смыслу подписи: главное действие страницы - заливка акцентом, удаление - красная обводка. Свойства primary / danger,
// заданные кнопке вручную, не перебиваются.
class ButtonRoles : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Polish) {
            if (auto *b = qobject_cast<QPushButton *>(o))
                assign(b);
        }
        return false;
    }

private:
    static void assign(QPushButton *b)
    {
        if (b->property("roleChecked").toBool())
            return;
        b->setProperty("roleChecked", true);
        if (b->property("primary").isValid() || b->property("danger").isValid())
            return;
        const QString t = b->text().remove(QLatin1Char('&')).trimmed();
        static const QRegularExpression danger(QStringLiteral("^(Удалить|Отменить документ|Очистить)"));
        static const QRegularExpression primary(QStringLiteral("^(Добавить|Создать|Войти|Применить|Провести|Принять|Сохранить(?! в PDF)|Печать$)"));
        const char *role = danger.match(t).hasMatch() ? "danger" : (primary.match(t).hasMatch() ? "primary" : nullptr);
        if (!role)
            return;
        b->setProperty(role, true);
        b->style()->unpolish(b);
        b->style()->polish(b);
    }
};

// Подсказки (всплывающие окошки) не показываются первые полсекунды после закрытия диалога: курсор остаётся там, где была кнопка
// «Закрыть», окно под ним оживает, и под курсором тут же вылезала подсказка какого-нибудь элемента.
class PopupGuard : public QObject
{
public:
    using QObject::QObject;
    static constexpr int kQuietMs = 600;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::Hide:
        case QEvent::Close:
            if (auto *d = qobject_cast<QDialog *>(o); d && d->isWindow()) {
                m_closed.restart();
                m_seen = true;
            }
            break;
        case QEvent::ToolTip:
            if (m_seen && !m_closed.hasExpired(kQuietMs))
                return true;
            break;
        default:
            break;
        }
        return false;
    }

private:
    QElapsedTimer m_closed;
    bool m_seen = false;
};

} // namespace

Mode saved()
{
    return QSettings().value(QStringLiteral("theme"), QStringLiteral("light")).toString() == QLatin1String("dark") ? Mode::Dark : Mode::Light;
}

Mode current() { return g_mode; }

const Colors &colors() { return g_colors; }

QString stylesheet(Mode mode) { return css(mode == Mode::Dark ? darkColors() : lightColors()); }

void apply(Mode mode)
{
    static ButtonRoles *roles = nullptr;
    if (!roles) {
        roles = new ButtonRoles(qApp);
        qApp->installEventFilter(roles);
        qApp->installEventFilter(new PopupGuard(qApp));
    }
    g_mode = mode;
    g_colors = mode == Mode::Dark ? darkColors() : lightColors();
    QSettings().setValue(QStringLiteral("theme"), mode == Mode::Dark ? QStringLiteral("dark") : QStringLiteral("light"));

    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"), Qt::CaseInsensitive))
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    const Colors &c = g_colors;
    p.setColor(QPalette::Window, c.bg);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.surface);
    p.setColor(QPalette::AlternateBase, c.surface2);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.surface);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::ToolTipBase, c.surface);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, c.accentText);
    p.setColor(QPalette::PlaceholderText, c.muted);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::Disabled, QPalette::Text, c.muted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.muted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.muted);
    QApplication::setPalette(p);

    QFont f = QApplication::font();
    f.setPointSizeF(10);
    QApplication::setFont(f);
    qApp->setStyleSheet(css(c));
}

QPixmap logo(int size)
{
    const qreal dpr = 2.0;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(0, 0, size, size);
    QLinearGradient g(r.topLeft(), r.bottomRight());
    g.setColorAt(0, QColor("#3d6df2"));
    g.setColorAt(1, QColor("#1c3fa6"));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(r, size * 0.28, size * 0.28);
    // коробка
    const qreal u = size / 24.0;
    QPen pen(QColor(255, 255, 255, 240), 1.7 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    QPainterPath box;
    box.moveTo(12 * u, 5.2 * u); box.lineTo(18.6 * u, 8.8 * u); box.lineTo(18.6 * u, 15.4 * u);
    box.lineTo(12 * u, 19 * u); box.lineTo(5.4 * u, 15.4 * u); box.lineTo(5.4 * u, 8.8 * u); box.closeSubpath();
    p.drawPath(box);
    p.drawLine(QPointF(5.4 * u, 8.8 * u), QPointF(12 * u, 12.4 * u));
    p.drawLine(QPointF(18.6 * u, 8.8 * u), QPointF(12 * u, 12.4 * u));
    p.drawLine(QPointF(12 * u, 12.4 * u), QPointF(12 * u, 19 * u));
    return pm;
}

QIcon icon(Icon which, const QColor &color, int size)
{
    const qreal dpr = 2.0;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal u = size / 24.0;
    p.setPen(QPen(color, 1.7 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    auto P = [u](qreal x, qreal y) { return QPointF(x * u, y * u); };
    auto poly = [&](std::initializer_list<QPointF> pts) {
        QPolygonF pg;
        for (const QPointF &q : pts) pg << q;
        p.drawPolygon(pg);
    };
    switch (which) {
    case Icon::Products: {
        poly({P(12, 3.5), P(19.5, 7.7), P(19.5, 16.3), P(12, 20.5), P(4.5, 16.3), P(4.5, 7.7)});
        p.drawLine(P(4.5, 7.7), P(12, 12)); p.drawLine(P(19.5, 7.7), P(12, 12)); p.drawLine(P(12, 12), P(12, 20.5));
        break;
    }
    case Icon::Warehouses: {
        poly({P(3.5, 10), P(12, 4), P(20.5, 10), P(20.5, 20), P(3.5, 20)});
        p.drawRect(QRectF(9 * u, 13 * u, 6 * u, 7 * u));
        break;
    }
    case Icon::Stock: {
        p.drawLine(P(4, 20), P(20, 20));
        p.drawRoundedRect(QRectF(5 * u, 12 * u, 3.6 * u, 8 * u), u, u);
        p.drawRoundedRect(QRectF(10.2 * u, 7 * u, 3.6 * u, 13 * u), u, u);
        p.drawRoundedRect(QRectF(15.4 * u, 3.5 * u, 3.6 * u, 16.5 * u), u, u);
        break;
    }
    case Icon::Labels: {
        poly({P(3.5, 4.5), P(12.2, 4.5), P(20.5, 12.8), P(12.8, 20.5), P(3.5, 11.8)});
        p.drawEllipse(P(8, 9), 1.3 * u, 1.3 * u);
        break;
    }
    case Icon::Import: {
        p.drawLine(P(12, 3.5), P(12, 14.5)); p.drawLine(P(7.8, 10.5), P(12, 14.7)); p.drawLine(P(16.2, 10.5), P(12, 14.7));
        QPainterPath tray; tray.moveTo(4 * u, 14.5 * u); tray.lineTo(4 * u, 20 * u); tray.lineTo(20 * u, 20 * u); tray.lineTo(20 * u, 14.5 * u);
        p.drawPath(tray);
        break;
    }
    case Icon::Market: {
        poly({P(5, 8), P(19, 8), P(20, 20.5), P(4, 20.5)});
        QPainterPath h; h.moveTo(8.5 * u, 10.5 * u); h.lineTo(8.5 * u, 7.5 * u); h.arcTo(QRectF(8.5 * u, 3.5 * u, 7 * u, 8 * u), 180, -180); h.lineTo(15.5 * u, 10.5 * u);
        p.drawPath(h);
        break;
    }
    case Icon::Cards: {
        p.drawRoundedRect(QRectF(4 * u, 4 * u, 7 * u, 7 * u), 1.6 * u, 1.6 * u);
        p.drawRoundedRect(QRectF(13 * u, 4 * u, 7 * u, 7 * u), 1.6 * u, 1.6 * u);
        p.drawRoundedRect(QRectF(4 * u, 13 * u, 7 * u, 7 * u), 1.6 * u, 1.6 * u);
        p.drawRoundedRect(QRectF(13 * u, 13 * u, 7 * u, 7 * u), 1.6 * u, 1.6 * u);
        break;
    }
    case Icon::Documents: {
        poly({P(6, 3.5), P(14, 3.5), P(19, 8.5), P(19, 20.5), P(6, 20.5)});
        p.drawLine(P(14, 3.5), P(14, 8.5)); p.drawLine(P(14, 8.5), P(19, 8.5));
        p.drawLine(P(9, 13), P(16, 13)); p.drawLine(P(9, 16.5), P(14, 16.5));
        break;
    }
    case Icon::Stocktake: {
        // планшет с галочкой: инвентаризация
        p.drawRoundedRect(QRectF(5.5 * u, 4.5 * u, 13 * u, 16 * u), 2 * u, 2 * u);
        p.drawRect(QRectF(9 * u, 3 * u, 6 * u, 3.4 * u));
        p.drawLine(P(8.5, 13), P(10.8, 15.4)); p.drawLine(P(10.8, 15.4), P(15.5, 10.6));
        break;
    }
    case Icon::Tag: {
        // ценник: пятиугольник с отверстием для верёвочки
        poly({P(3.5, 12.5), P(12.5, 3.5), P(20.5, 3.5), P(20.5, 11.5), P(11.5, 20.5)});
        p.drawEllipse(P(16.5, 7.5), 1.4 * u, 1.4 * u);
        break;
    }
    case Icon::Marking: {
        // квадрат Data Matrix: сплошные левая и нижняя стороны, пунктир сверху и справа, несколько модулей внутри
        p.drawRect(QRectF(4 * u, 4 * u, 16 * u, 16 * u));
        p.drawLine(P(4, 4), P(4, 20)); p.drawLine(P(4, 20), P(20, 20));
        p.setBrush(color);
        for (const QPointF &c : {P(8, 8), P(12, 8), P(16, 8), P(8, 12), P(14, 12), P(10, 16), P(16, 16)})
            p.drawRect(QRectF(c.x() - 1.2 * u, c.y() - 1.2 * u, 2.4 * u, 2.4 * u));
        break;
    }
    case Icon::Update: {
        // круговая стрелка «обновить»
        p.drawArc(QRectF(5 * u, 5 * u, 14 * u, 14 * u), 40 * 16, 270 * 16);
        p.drawLine(P(17.7, 5.2), P(18.6, 9.4)); p.drawLine(P(18.6, 9.4), P(14.4, 9.8));
        break;
    }
    case Icon::Orders: {
        p.drawRoundedRect(QRectF(5 * u, 4.5 * u, 14 * u, 16 * u), 2 * u, 2 * u);
        p.drawRect(QRectF(9 * u, 3 * u, 6 * u, 3.4 * u));
        p.drawLine(P(8.5, 11), P(15.5, 11)); p.drawLine(P(8.5, 14.5), P(15.5, 14.5)); p.drawLine(P(8.5, 18), P(12.5, 18));
        break;
    }
    case Icon::Users: {
        p.drawEllipse(P(12, 8.2), 3.6 * u, 3.6 * u);
        QPainterPath b; b.moveTo(4.8 * u, 20 * u); b.cubicTo(5.5 * u, 14.5 * u, 18.5 * u, 14.5 * u, 19.2 * u, 20 * u);
        p.drawPath(b);
        break;
    }
    case Icon::Search: {
        p.drawEllipse(P(10.5, 10.5), 6 * u, 6 * u); p.drawLine(P(15, 15), P(20, 20));
        break;
    }
    case Icon::Eye: case Icon::EyeOff: {
        QPainterPath e; e.moveTo(2.8 * u, 12 * u); e.cubicTo(6 * u, 5.8 * u, 18 * u, 5.8 * u, 21.2 * u, 12 * u); e.cubicTo(18 * u, 18.2 * u, 6 * u, 18.2 * u, 2.8 * u, 12 * u);
        p.drawPath(e); p.drawEllipse(P(12, 12), 3 * u, 3 * u);
        if (which == Icon::EyeOff) p.drawLine(P(4.5, 19.5), P(19.5, 4.5));
        break;
    }
    case Icon::Close: {
        p.drawLine(P(6, 6), P(18, 18)); p.drawLine(P(18, 6), P(6, 18));
        break;
    }
    case Icon::Sun: {
        p.drawEllipse(P(12, 12), 4 * u, 4 * u);
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4;
            p.drawLine(P(12 + 7 * std::cos(a), 12 + 7 * std::sin(a)), P(12 + 9.4 * std::cos(a), 12 + 9.4 * std::sin(a)));
        }
        break;
    }
    case Icon::Moon: {
        QPainterPath m; m.moveTo(19 * u, 14.5 * u); m.cubicTo(11 * u, 19 * u, 4 * u, 12 * u, 9.3 * u, 4.2 * u); m.cubicTo(9 * u, 11.5 * u, 13 * u, 15.5 * u, 19 * u, 14.5 * u);
        p.drawPath(m);
        break;
    }
    case Icon::Logout: {
        QPainterPath d; d.moveTo(10 * u, 4.5 * u); d.lineTo(5 * u, 4.5 * u); d.lineTo(5 * u, 19.5 * u); d.lineTo(10 * u, 19.5 * u); p.drawPath(d);
        p.drawLine(P(10, 12), P(20, 12)); p.drawLine(P(16.5, 8.5), P(20, 12)); p.drawLine(P(16.5, 15.5), P(20, 12));
        break;
    }
    }
    return QIcon(pm);
}

} // namespace Theme

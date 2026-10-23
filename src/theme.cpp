#include "theme.h"

#include <QApplication>
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

// Галочка и точка для флажков/переключателей рисуются кодом и кладутся в кэш-папку: в таблице
// стилей картинку можно указать только путём к файлу.
QString indicatorImage(bool dot)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/theme");
    QDir().mkpath(dir);
    const QString path = dir + (dot ? QStringLiteral("/dot.png") : QStringLiteral("/check.png"));
    if (!QFile::exists(path)) {
        QPixmap pm(36, 36);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        if (dot) {
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::white);
            p.drawEllipse(QPointF(18, 18), 5.5, 5.5);
        } else {
            p.setPen(QPen(Qt::white, 3.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(QPolygonF({QPointF(9.5, 18.5), QPointF(15.5, 24.5), QPointF(26.5, 11.5)}));
        }
        p.end();
        pm.save(path, "PNG");
    }
    return path;
}
Colors g_colors;

Colors lightColors()
{
    Colors c;
    c.bg = "#f3f5fa"; c.surface = "#ffffff"; c.surface2 = "#eef1f8"; c.border = "#e0e5ef"; c.borderStrong = "#c7cfdf";
    c.text = "#1a2133"; c.muted = "#6b7690"; c.accent = "#4f6bf5"; c.accentHover = "#3d57db"; c.accentText = "#ffffff";
    c.danger = "#e5484d"; c.ok = "#2e9e6b"; c.selection = "#e4eaff"; c.scroll = "#c5cde0"; c.sidebar = "#111827";
    return c;
}

Colors darkColors()
{
    Colors c;
    c.bg = "#12151d"; c.surface = "#1b2030"; c.surface2 = "#232a3d"; c.border = "#2b3347"; c.borderStrong = "#3a4560";
    c.text = "#e8ecf6"; c.muted = "#8f9bb7"; c.accent = "#6b86ff"; c.accentHover = "#8299ff"; c.accentText = "#0e1220";
    c.danger = "#ff6b70"; c.ok = "#4cc790"; c.selection = "#2b3860"; c.scroll = "#3a4560"; c.sidebar = "#0c0f17";
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
QLabel#pageTitle { font-size: 17pt; font-weight: 700; }
QLabel#muted { color: @muted@; }
QToolTip { background: @surface@; color: @text@; border: 1px solid @border@; border-radius: 6px; padding: 6px 8px; }

QMenuBar { background: @surface@; border-bottom: 1px solid @border@; padding: 3px 8px; }
QMenuBar::item { background: transparent; padding: 6px 12px; border-radius: 7px; }
QMenuBar::item:selected { background: @surface2@; }
QMenu { background: @surface@; border: 1px solid @border@; border-radius: 10px; padding: 6px; }
QMenu::item { padding: 8px 28px 8px 14px; border-radius: 7px; }
QMenu::item:selected { background: @selection@; color: @text@; }
QMenu::separator { height: 1px; background: @border@; margin: 6px 8px; }

QPushButton { background: @surface@; border: 1px solid @border@; border-radius: 9px; padding: 7px 15px; }
QPushButton:hover { background: @surface2@; border-color: @borderStrong@; }
QPushButton:pressed { background: @border@; }
QPushButton:disabled { color: @muted@; background: @surface2@; border-color: @border@; }
QPushButton:default, QPushButton[primary="true"] { background: @accent@; border-color: @accent@; color: @accentText@; font-weight: 600; }
QPushButton:default:hover, QPushButton[primary="true"]:hover { background: @accentHover@; border-color: @accentHover@; }
QPushButton:default:disabled, QPushButton[primary="true"]:disabled { background: @surface2@; color: @muted@; border-color: @border@; }
QToolButton { background: transparent; border: none; border-radius: 8px; padding: 5px; }
QToolButton:hover { background: @surface2@; }

QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QDateEdit, QComboBox {
  background: @surface@; border: 1px solid @border@; border-radius: 9px; padding: 6px 10px;
  selection-background-color: @accent@; selection-color: @accentText@; }
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QDateEdit:focus, QComboBox:focus, QComboBox:on { border: 1px solid @accent@; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color: @muted@; background: @surface2@; }
QLineEdit[readOnly="true"] { background: @surface2@; }
QComboBox { padding-right: 24px; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox QAbstractItemView { background: @surface@; border: 1px solid @border@; border-radius: 8px; padding: 4px;
  selection-background-color: @selection@; selection-color: @text@; outline: 0; }
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 18px; border: none; background: transparent; }

QCheckBox, QRadioButton { spacing: 8px; background: transparent; }
QCheckBox::indicator, QRadioButton::indicator { width: 17px; height: 17px; border: 1.5px solid @borderStrong@; background: @surface@; }
QCheckBox::indicator { border-radius: 5px; }
QRadioButton::indicator { border-radius: 9px; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: @accent@; }
QCheckBox::indicator:checked { background: @accent@; border-color: @accent@; image: url(@check@); }
QRadioButton::indicator:checked { background: @accent@; border-color: @accent@; image: url(@dot@); }
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { background: @surface2@; border-color: @border@; }
QTableView::indicator { width: 17px; height: 17px; border: 1.5px solid @borderStrong@; border-radius: 5px; background: @surface@; }
QTableView::indicator:checked { background: @accent@; border-color: @accent@; image: url(@check@); }

QTableView, QTableWidget, QListWidget, QTreeWidget, QTreeView {
  background: @surface@; alternate-background-color: @surface2@; gridline-color: transparent; border: 1px solid @border@; border-radius: 10px;
  selection-background-color: @selection@; selection-color: @text@; outline: 0; }
QTableView::item, QTableWidget::item { padding: 3px 6px; border: none; }
QListWidget::item, QTreeWidget::item { padding: 4px 6px; }
QHeaderView { background: transparent; }
QHeaderView::section { background: @surface2@; color: @muted@; border: none; border-bottom: 1px solid @border@; padding: 8px 10px; font-weight: 600; }
QTableCornerButton::section { background: @surface2@; border: none; }

QScrollBar:vertical { width: 12px; background: transparent; margin: 3px; }
QScrollBar::handle:vertical { background: @scroll@; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @muted@; }
QScrollBar:horizontal { height: 12px; background: transparent; margin: 3px; }
QScrollBar::handle:horizontal { background: @scroll@; border-radius: 4px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: @muted@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QGroupBox { background: @surface@; border: 1px solid @border@; border-radius: 12px; margin-top: 16px; padding: 14px 12px 10px 12px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 14px; padding: 0 6px; color: @muted@; font-weight: 600; background: @bg@; border-radius: 4px; }

QTabWidget::pane { border: none; top: -1px; }
QTabBar::tab { background: transparent; color: @muted@; padding: 9px 16px; border: none; border-bottom: 2px solid transparent; margin-right: 4px; font-weight: 600; }
QTabBar::tab:hover { color: @text@; }
QTabBar::tab:selected { color: @accent@; border-bottom: 2px solid @accent@; }

QSplitter::handle { background: transparent; }
QProgressBar { background: @surface2@; border: none; border-radius: 6px; text-align: center; height: 12px; }
QProgressBar::chunk { background: @accent@; border-radius: 6px; }
QDialogButtonBox QPushButton { min-width: 84px; }
QMessageBox { background: @bg@; }

QFrame#sidebarFrame { background: @sidebar@; border: none; }
QFrame#sidebarFrame QLabel { color: #e9edf7; }
QFrame#sidebarFrame QLabel#sideMuted { color: #8e9ab5; font-size: 9pt; }
QFrame#userCard { background: rgba(255,255,255,0.06); border-radius: 12px; }
QListWidget#sidebar { background: transparent; border: none; outline: 0; padding: 4px 8px; }
QListWidget#sidebar::item { color: #c5cde0; padding: 10px 12px; margin: 2px 0; border-radius: 10px; }
QListWidget#sidebar::item:hover { background: rgba(255,255,255,0.07); color: #ffffff; }
QListWidget#sidebar::item:selected { background: @accent@; color: @accentText@; }
QFrame#sidebarFrame QToolButton { color: #c5cde0; }
QFrame#sidebarFrame QToolButton:hover { background: rgba(255,255,255,0.10); }
)");
    const QPair<QString, QColor> tokens[] = {
        {"@bg@", c.bg}, {"@surface@", c.surface}, {"@surface2@", c.surface2}, {"@border@", c.border}, {"@borderStrong@", c.borderStrong},
        {"@text@", c.text}, {"@muted@", c.muted}, {"@accentHover@", c.accentHover}, {"@accentText@", c.accentText}, {"@accent@", c.accent},
        {"@selection@", c.selection}, {"@scroll@", c.scroll}, {"@sidebar@", c.sidebar}};
    for (const auto &t : tokens)
        s.replace(t.first, t.second.name());
    s.replace(QStringLiteral("@check@"), indicatorImage(false));
    s.replace(QStringLiteral("@dot@"), indicatorImage(true));
    return s;
}

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
    g.setColorAt(0, QColor("#6d7bff"));
    g.setColorAt(1, QColor("#9b5cf6"));
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

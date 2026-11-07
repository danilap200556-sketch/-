#include "labelfonts.h"

#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QMutex>
#include <QMutexLocker>

namespace LabelFonts {

namespace {

QMutex g_mutex;
bool g_loaded = false;
QStringList g_userFamilies;     // шрифты из папки fonts
QStringList g_embeddedFamilies; // имена, под которыми зарегистрирован встроенный Montserrat (их может быть несколько: «Montserrat», «Montserrat SemiBold»...)

// Насыщенность по названию начертания или семейства («SemiBold» -> 600); 0 - в названии её нет.
int weightFromName(const QString &name)
{
    const QString n = name.toLower();
    struct Item { const char *token; int weight; };
    static const Item kItems[] = {{"extrabold", 800}, {"extra bold", 800}, {"ultrabold", 800}, {"semibold", 600}, {"semi bold", 600}, {"demibold", 600},
                                  {"black", 900}, {"heavy", 900}, {"bold", 700}, {"medium", 500}, {"extralight", 200}, {"light", 300}, {"thin", 100}};
    for (const Item &it : kItems)
        if (n.contains(QLatin1String(it.token)))
            return it.weight;
    return 0;
}

struct Face {
    QString family;
    QString style;
    int weight = 400;
    bool italic = false;
};

QList<Face> facesOf(const QString &family)
{
    QList<Face> out;
    for (const QString &style : QFontDatabase::styles(family)) {
        Face f;
        f.family = family;
        f.style = style;
        f.italic = QFontDatabase::italic(family, style);
        f.weight = weightFromName(style);
        if (f.weight == 0)
            f.weight = weightFromName(family);
        if (f.weight == 0)
            f.weight = QFontDatabase::weight(family, style);
        if (f.weight == 0)
            f.weight = 400;
        out << f;
    }
    return out;
}

} // namespace

void ensureLoaded()
{
    QMutexLocker lock(&g_mutex);
    if (g_loaded)
        return;
    g_loaded = true;
    for (const char *file : {"Montserrat-Regular", "Montserrat-SemiBold", "Montserrat-Bold", "Montserrat-ExtraBold"}) {
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/%1.ttf").arg(QLatin1String(file)));
        if (id >= 0)
            for (const QString &f : QFontDatabase::applicationFontFamilies(id))
                if (!g_embeddedFamilies.contains(f))
                    g_embeddedFamilies << f;
    }
    // Шрифты пользователя: папка fonts рядом с программой (и в текущей папке - для запуска из каталога сборки).
    QStringList dirs{QCoreApplication::applicationDirPath() + QStringLiteral("/fonts")};
    if (QDir::currentPath() != QCoreApplication::applicationDirPath())
        dirs << QDir::currentPath() + QStringLiteral("/fonts");
    for (const QString &dirPath : std::as_const(dirs)) {
        const QDir dir(dirPath);
        for (const QFileInfo &fi : dir.entryInfoList({QStringLiteral("*.ttf"), QStringLiteral("*.otf"), QStringLiteral("*.TTF"), QStringLiteral("*.OTF")}, QDir::Files)) {
            const int id = QFontDatabase::addApplicationFont(fi.absoluteFilePath());
            if (id >= 0)
                for (const QString &f : QFontDatabase::applicationFontFamilies(id))
                    if (!g_userFamilies.contains(f) && !g_embeddedFamilies.contains(f))
                        g_userFamilies << f;
        }
    }
}

QString defaultFamily()
{
    ensureLoaded();
    return QFontDatabase::families().contains(QStringLiteral("Montserrat")) ? QStringLiteral("Montserrat") : QStringLiteral("Arial");
}

QStringList families()
{
    ensureLoaded();
    QStringList out;
    if (QFontDatabase::families().contains(QStringLiteral("Montserrat")))
        out << QStringLiteral("Montserrat");
    out << g_userFamilies << QStringLiteral("Arial");
    out.removeDuplicates();
    return out;
}

QFont font(const QString &familyIn, int weight, int pixelSize)
{
    ensureLoaded();
    QString family = familyIn.isEmpty() ? defaultFamily() : familyIn;
    if (!QFontDatabase::families().contains(family))
        family = QStringLiteral("Arial");
    // Все начертания семейства (у Montserrat на некоторых системах они зарегистрированы под разными именами - «Montserrat SemiBold»...).
    QList<Face> faces = facesOf(family);
    if (family == QLatin1String("Montserrat"))
        for (const QString &alt : std::as_const(g_embeddedFamilies))
            if (alt != family)
                faces += facesOf(alt);
    const Face *best = nullptr;
    for (const Face &f : std::as_const(faces)) {
        if (f.italic)
            continue;
        if (!best || qAbs(f.weight - weight) < qAbs(best->weight - weight))
            best = &f;
    }
    QFont out;
    if (best) {
        out = QFontDatabase::font(best->family, best->style, 12);
    } else {
        out = QFont(family);
        out.setBold(weight >= 600);
    }
    out.setPixelSize(qMax(6, pixelSize));
    out.setStyleStrategy(QFont::PreferAntialias);
    return out;
}

} // namespace LabelFonts

#include "siteprices.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace SitePrices {

namespace {

// Хвост артикула после последнего «_» - размер, если он НАЧИНАЕТСЯ с размера и тот не продолжается буквой или цифрой
// (поэтому «ABC_SALE» - не размер «S»). Символы перечислены явно, без юникод-свойств: так же устроен регулярка сайта.
const QRegularExpression &sizeRe()
{
    static const QRegularExpression re(
        QStringLiteral("^(\\d{2}(?:[.,]\\d)?|XXXS|XXS|XS|S|M|L|XL|XXL|XXXL|OS|ONE\\s*SIZE|UNI)"
                       "(?![A-Za-z0-9\\x{0410}-\\x{044F}\\x{0401}\\x{0451}])"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

} // namespace

QString modelKey(const QString &sku)
{
    const QString s = sku.trimmed();
    const int i = s.lastIndexOf(QLatin1Char('_'));
    if (i <= 0 || i == s.size() - 1)
        return s;
    const QString tail = s.mid(i + 1).trimmed();
    return sizeRe().match(tail).hasMatch() ? s.left(i) : s;
}

SaleState saleState(const Entry &e, const QDate &today)
{
    if (!e.salePrice || *e.salePrice <= 0 || *e.salePrice >= e.price)
        return SaleState::None;
    if (e.saleFrom.isValid() && today < e.saleFrom)
        return SaleState::Scheduled;
    if (e.saleTo.isValid() && today > e.saleTo)
        return SaleState::Ended;
    return SaleState::Active;
}

QString saleStateText(SaleState state)
{
    switch (state) {
    case SaleState::None: return QString();
    case SaleState::Active: return QStringLiteral("скидка действует");
    case SaleState::Scheduled: return QStringLiteral("скидка скоро начнётся");
    case SaleState::Ended: return QStringLiteral("скидка закончилась");
    }
    return QString();
}

int discountPercent(double price, double salePrice)
{
    if (price <= 0 || salePrice <= 0 || salePrice >= price)
        return 0;
    return std::clamp(static_cast<int>(std::lround((1.0 - salePrice / price) * 100.0)), 0, 99);
}

double priceByPercent(double price, double percent, int step)
{
    if (price <= 0)
        return 0;
    const double raw = price * (1.0 - std::clamp(percent, 0.0, 99.0) / 100.0);
    const double s = step > 1 ? step : 1;
    double v = std::round(raw / s) * s;
    if (v < 1)
        v = 1;
    return std::min(v, price);
}

double effectivePrice(const Entry &e, const QDate &today)
{
    return saleState(e, today) == SaleState::Active ? *e.salePrice : e.price;
}

} // namespace SitePrices

// Печатает ключ модели (SitePrices::modelKey) для каждого артикула со стандартного ввода - по строке на артикул.
// Нужно, чтобы сверить правило с сайтом (web/src/model.js, splitSku) на всех артикулах из базы:
//   g++ -std=c++17 -fPIC -Isrc tools/check_model_key.cpp src/siteprices.cpp $(pkg-config --cflags --libs Qt6Core) -o check_model_key
//   ./check_model_key < артикулы.txt > cpp.txt ; (на сайте) node scripts/model-keys.js < артикулы.txt > js.txt ; diff cpp.txt js.txt
#include "siteprices.h"

#include <QTextStream>

int main()
{
    QTextStream in(stdin);
    QTextStream out(stdout);
    in.setEncoding(QStringConverter::Utf8);
    out.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd())
        out << SitePrices::modelKey(in.readLine()) << '\n';
    return 0;
}

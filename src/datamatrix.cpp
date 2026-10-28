#include "datamatrix.h"

#include <QPainter>

#include <algorithm>
#include <array>
#include <vector>

namespace DataMatrix {

namespace {

struct SizeInfo {
    int size;        // сторона символа
    int region;      // сторона одной области данных
    int regions;     // областей по каждой стороне
    int dataWords;
    int eccWords;    // всего
    int blocks;
};

const SizeInfo kSizes[] = {
    {10, 8, 1, 3, 5, 1},      {12, 10, 1, 5, 7, 1},      {14, 12, 1, 8, 10, 1},     {16, 14, 1, 12, 12, 1},
    {18, 16, 1, 18, 14, 1},   {20, 18, 1, 22, 18, 1},    {22, 20, 1, 30, 20, 1},    {24, 22, 1, 36, 24, 1},
    {26, 24, 1, 44, 28, 1},   {32, 14, 2, 62, 36, 1},    {36, 16, 2, 86, 42, 1},    {40, 18, 2, 114, 48, 1},
    {44, 20, 2, 144, 56, 1},  {48, 22, 2, 174, 68, 1},   {52, 24, 2, 204, 84, 2},   {64, 14, 4, 280, 112, 2},
    {72, 16, 4, 368, 144, 4}, {80, 18, 4, 456, 192, 4},  {88, 20, 4, 576, 224, 4},  {96, 22, 4, 696, 272, 4},
    {104, 24, 4, 816, 336, 6}, {120, 18, 6, 1050, 408, 6}, {132, 20, 6, 1304, 496, 8}, {144, 22, 6, 1558, 620, 10},
};

constexpr int kFnc1 = 232;

// ASCII-кодирование: пары цифр - одно слово, остальные символы - код+1, FNC1 в начале и вместо 0x1D.
std::vector<int> asciiWords(const QByteArray &text, bool gs1)
{
    std::vector<int> w;
    if (gs1)
        w.push_back(kFnc1);
    for (int i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (gs1 && c == 0x1D) {
            w.push_back(kFnc1);
            continue;
        }
        if (c >= '0' && c <= '9' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') {
            w.push_back(130 + (c - '0') * 10 + (text[i + 1] - '0'));
            ++i;
            continue;
        }
        if (c < 128) {
            w.push_back(c + 1);
        } else { // верхняя половина ASCII: Upper Shift (235) и код-127
            w.push_back(235);
            w.push_back(c - 128 + 1);
        }
    }
    return w;
}

// --- Рид-Соломон над GF(256), порождающий многочлен x^8 + x^5 + x^3 + x^2 + 1 (0x12D), корни a^1 ... a^n
struct Gf {
    std::array<int, 256> exp{};
    std::array<int, 256> log{};
    Gf()
    {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = x;
            log[x] = i;
            x <<= 1;
            if (x & 0x100)
                x ^= 0x12D;
        }
        exp[255] = exp[0];
    }
    int mul(int a, int b) const { return (a && b) ? exp[(log[a] + log[b]) % 255] : 0; }
};

std::vector<int> eccFor(const std::vector<int> &data, int eccCount)
{
    static const Gf gf;
    std::vector<int> gen(eccCount + 1, 0);
    gen[0] = 1;
    for (int i = 1; i <= eccCount; ++i) { // gen *= (x - a^i)
        for (int j = i; j >= 1; --j)
            gen[j] = gen[j - 1] ^ gf.mul(gen[j], gf.exp[i]);
        gen[0] = gf.mul(gen[0], gf.exp[i]);
    }
    std::vector<int> ecc(eccCount, 0);
    for (int d : data) {
        const int k = d ^ ecc[eccCount - 1];
        for (int j = eccCount - 1; j >= 1; --j)
            ecc[j] = ecc[j - 1] ^ gf.mul(k, gen[j]);
        ecc[0] = gf.mul(k, gen[0]);
    }
    std::reverse(ecc.begin(), ecc.end());
    return ecc;
}

// --- Размещение слов в области данных (алгоритм «utah» из ISO 16022, приложение F)
class Placer
{
public:
    Placer(int nrow, int ncol) : m_nrow(nrow), m_ncol(ncol), m_cell(nrow * ncol, 0) {}

    // Для каждой ячейки: 0 - не занята, 1 - постоянно тёмная, иначе слово*16 + номер бита (1..8, 1 - старший)
    std::vector<int> run()
    {
        int chr = 1; // слова нумеруются с 1
        int row = 4, col = 0;
        do {
            if (row == m_nrow && col == 0)
                corner1(chr++);
            if (row == m_nrow - 2 && col == 0 && m_ncol % 4 != 0)
                corner2(chr++);
            if (row == m_nrow - 2 && col == 0 && m_ncol % 8 == 4)
                corner3(chr++);
            if (row == m_nrow + 4 && col == 2 && m_ncol % 8 == 0)
                corner4(chr++);
            do {
                if (row < m_nrow && col >= 0 && m_cell[row * m_ncol + col] == 0)
                    utah(row, col, chr++);
                row -= 2;
                col += 2;
            } while (row >= 0 && col < m_ncol);
            row += 1;
            col += 3;
            do {
                if (row >= 0 && col < m_ncol && m_cell[row * m_ncol + col] == 0)
                    utah(row, col, chr++);
                row += 2;
                col -= 2;
            } while (row < m_nrow && col >= 0);
            row += 3;
            col += 1;
        } while (row < m_nrow || col < m_ncol);
        if (m_cell[m_nrow * m_ncol - 1] == 0) { // правый нижний угол остался пустым - фиксированный узор
            m_cell[m_nrow * m_ncol - 1] = 1;
            m_cell[(m_nrow - 2) * m_ncol + m_ncol - 2] = 1;
        }
        return m_cell;
    }

private:
    void module(int row, int col, int chr, int bit)
    {
        if (row < 0) {
            row += m_nrow;
            col += 4 - ((m_nrow + 4) % 8);
        }
        if (col < 0) {
            col += m_ncol;
            row += 4 - ((m_ncol + 4) % 8);
        }
        m_cell[row * m_ncol + col] = chr * 16 + bit; // >= 17: отличается от 1 (постоянно тёмная ячейка)
    }
    void utah(int row, int col, int chr)
    {
        module(row - 2, col - 2, chr, 1);
        module(row - 2, col - 1, chr, 2);
        module(row - 1, col - 2, chr, 3);
        module(row - 1, col - 1, chr, 4);
        module(row - 1, col, chr, 5);
        module(row, col - 2, chr, 6);
        module(row, col - 1, chr, 7);
        module(row, col, chr, 8);
    }
    void corner1(int chr)
    {
        module(m_nrow - 1, 0, chr, 1);
        module(m_nrow - 1, 1, chr, 2);
        module(m_nrow - 1, 2, chr, 3);
        module(0, m_ncol - 2, chr, 4);
        module(0, m_ncol - 1, chr, 5);
        module(1, m_ncol - 1, chr, 6);
        module(2, m_ncol - 1, chr, 7);
        module(3, m_ncol - 1, chr, 8);
    }
    void corner2(int chr)
    {
        module(m_nrow - 3, 0, chr, 1);
        module(m_nrow - 2, 0, chr, 2);
        module(m_nrow - 1, 0, chr, 3);
        module(0, m_ncol - 4, chr, 4);
        module(0, m_ncol - 3, chr, 5);
        module(0, m_ncol - 2, chr, 6);
        module(0, m_ncol - 1, chr, 7);
        module(1, m_ncol - 1, chr, 8);
    }
    void corner3(int chr)
    {
        module(m_nrow - 3, 0, chr, 1);
        module(m_nrow - 2, 0, chr, 2);
        module(m_nrow - 1, 0, chr, 3);
        module(0, m_ncol - 2, chr, 4);
        module(0, m_ncol - 1, chr, 5);
        module(1, m_ncol - 1, chr, 6);
        module(2, m_ncol - 1, chr, 7);
        module(3, m_ncol - 1, chr, 8);
    }
    void corner4(int chr)
    {
        module(m_nrow - 1, 0, chr, 1);
        module(m_nrow - 1, m_ncol - 1, chr, 2);
        module(0, m_ncol - 3, chr, 3);
        module(0, m_ncol - 2, chr, 4);
        module(0, m_ncol - 1, chr, 5);
        module(1, m_ncol - 3, chr, 6);
        module(1, m_ncol - 2, chr, 7);
        module(1, m_ncol - 1, chr, 8);
    }

    int m_nrow, m_ncol;
    std::vector<int> m_cell;
};

} // namespace

Symbol encode(const QByteArray &text, bool gs1, QString *error)
{
    std::vector<int> words = asciiWords(text, gs1);
    const SizeInfo *info = nullptr;
    for (const SizeInfo &s : kSizes)
        if (static_cast<int>(words.size()) <= s.dataWords) {
            info = &s;
            break;
        }
    if (!info) {
        if (error)
            *error = QStringLiteral("данных слишком много для Data Matrix (%1 слов, максимум 1558)").arg(words.size());
        return {};
    }

    // Добиваем до ёмкости: первое слово-заполнитель 129, остальные «случайные» (ISO 16022, 5.2.1.1)
    if (static_cast<int>(words.size()) < info->dataWords) {
        words.push_back(129);
        while (static_cast<int>(words.size()) < info->dataWords) {
            const int pos = static_cast<int>(words.size()) + 1;
            int pad = 129 + ((149 * pos) % 253) + 1;
            if (pad > 254)
                pad -= 254;
            words.push_back(pad);
        }
    }

    // Блоки Рида-Соломона: слова раздаются блокам по кругу, защитные слова чередуются так же
    const int blocks = info->blocks;
    const int eccPerBlock = info->eccWords / blocks;
    std::vector<std::vector<int>> blockData(blocks);
    for (int i = 0; i < info->dataWords; ++i)
        blockData[i % blocks].push_back(words[i]);
    std::vector<std::vector<int>> blockEcc(blocks);
    for (int b = 0; b < blocks; ++b)
        blockEcc[b] = eccFor(blockData[b], eccPerBlock);
    std::vector<int> all(words.begin(), words.end());
    all.resize(info->dataWords + info->eccWords);
    for (int b = 0; b < blocks; ++b)
        for (int i = 0; i < eccPerBlock; ++i)
            all[info->dataWords + i * blocks + b] = blockEcc[b][i];

    // Размещение в общей матрице данных
    const int mapSize = info->region * info->regions;
    Placer placer(mapSize, mapSize);
    const std::vector<int> cells = placer.run();

    Symbol sym;
    sym.size = info->size;
    sym.modules.fill(false, sym.size * sym.size);
    const int block = info->region + 2;
    // Узор поиска в каждой области: сплошные левая и нижняя стороны, пунктир сверху и справа
    for (int ry = 0; ry < info->regions; ++ry)
        for (int rx = 0; rx < info->regions; ++rx)
            for (int i = 0; i < block; ++i) {
                const int y0 = ry * block, x0 = rx * block;
                sym.modules[(y0 + i) * sym.size + x0] = true;                       // левая сторона
                sym.modules[(y0 + block - 1) * sym.size + x0 + i] = true;           // нижняя сторона
                if (i % 2 == 0)
                    sym.modules[y0 * sym.size + x0 + i] = true;                     // верх: тёмные на чётных
                if (i % 2 == 1)
                    sym.modules[(y0 + i) * sym.size + x0 + block - 1] = true;       // право: тёмные на нечётных
            }
    for (int r = 0; r < mapSize; ++r)
        for (int c = 0; c < mapSize; ++c) {
            const int v = cells[r * mapSize + c];
            bool dark = false;
            if (v == 1) {
                dark = true;
            } else if (v >= 16) {
                const int chr = v / 16;          // слово (с 1)
                const int bit = v % 16;          // 1..8, 1 - старший бит
                dark = (all[chr - 1] >> (8 - bit)) & 1;
            }
            const int y = (r / info->region) * block + (r % info->region) + 1;
            const int x = (c / info->region) * block + (c % info->region) + 1;
            sym.modules[y * sym.size + x] = dark;
        }
    return sym;
}

QImage render(const Symbol &symbol, int modulepx, int quietModules)
{
    if (symbol.isNull())
        return {};
    const int px = qMax(1, modulepx);
    const int total = (symbol.size + 2 * quietModules) * px;
    QImage img(total, total, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    for (int r = 0; r < symbol.size; ++r)
        for (int c = 0; c < symbol.size; ++c)
            if (symbol.dark(r, c))
                p.drawRect((c + quietModules) * px, (r + quietModules) * px, px, px);
    return img;
}

} // namespace DataMatrix

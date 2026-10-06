#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPixmap>
#include <QString>

// Фото товаров лежат в общей базе (таблица product_photos), поэтому их видят
// все компьютеры и сайт. Любой файл приводится к JPEG: фото до 1280 px и
// миниатюра до 240 px.
namespace PhotoStore {

constexpr int kMaxPhotosPerProduct = 12;
constexpr int kFullSize = 1280;
constexpr int kThumbSize = 240;

struct Prepared {
    QByteArray data;
    QByteArray thumb;
    QString sourceName; // имя исходного файла (для подписей в интерфейсе)
    bool isNull() const { return data.isEmpty(); }
};

// Читает картинку (jpg/png/webp/bmp...), учитывает поворот из EXIF, подкладывает
// белый фон под прозрачность и уменьшает.
bool prepare(const QString &path, Prepared *out, QString *error = nullptr);

QList<int> ids(int productId);               // в порядке показа; первое - обложка
int count(int productId);
QByteArray fullData(int photoId);
QByteArray thumbData(int photoId);
int add(int productId, const Prepared &p, QString *error = nullptr); // id фото или -1
// Такое же фото (по содержимому) уже есть у товара - повторная загрузка пропускается.
bool exists(int productId, const Prepared &p);
bool remove(int photoId);
bool makeCover(int photoId);

// Накопленные в диалоге изменения, применяются одной транзакцией.
struct Changes {
    QList<Prepared> added;
    QList<int> removed;
    int coverExistingId = 0;   // сделать обложкой уже сохранённое фото
    int coverAddedIndex = -1;  // ...или одно из добавляемых
    bool isEmpty() const { return added.isEmpty() && removed.isEmpty() && !coverExistingId && coverAddedIndex < 0; }
};
bool apply(int productId, const Changes &changes, QString *error = nullptr);

// Подбор товара по имени файла: "NK-AF1-42.jpg", "NK-AF1-42_2.jpg", "NK-AF1-42 (3).png"
// или штрихкод в названии файла. Номер задаёт порядок фото одного товара.
class SkuIndex
{
public:
    SkuIndex();
    struct Hit {
        int productId = -1;
        QString sku;
        int order = 0;
    };
    Hit match(const QString &fileName) const;
    int productCount() const { return m_skuById.size(); }

private:
    QHash<QString, int> m_byKey;   // артикул / артикул на Маркете / штрихкод (в нижнем регистре)
    QHash<int, QString> m_skuById;
};

} // namespace PhotoStore

// Кэш миниатюр для таблиц: сначала prefetch() для видимых товаров (один запрос на
// пачку), затем thumb() без обращений к базе.
namespace PhotoCache {

constexpr int kIconSize = 56;

QPixmap thumb(int productId);                // квадрат kIconSize; "нет фото" - заглушка
bool hasPhoto(int productId);
void prefetch(const QList<int> &productIds);
void invalidate(int productId);
void clear();
QByteArray coverJpeg(int productId);         // большая обложка (для подсказки), кэшируется

} // namespace PhotoCache

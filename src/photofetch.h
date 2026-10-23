#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// Фото для карточек товаров, которые уже есть в базе. Откуда берутся картинки (в таком порядке):
//  1) с самого Яндекс Маркета - кабинеты из таблицы market_accounts, по артикулу на Маркете (у товара без него - по
//     обычному артикулу);
//  2) из карточек Маркета, подготовленных в программе (market_cards.pictures);
//  3) из интернета: поиск картинок по названию товара (ImageSearch), берётся снимок на светлом фоне.
// Картинки скачиваются, приводятся к JPEG и сохраняются у товара в общей базе (product_photos), поэтому их сразу видят
// все компьютеры и сайт.
namespace PhotoFetch {

struct Options {
    QList<int> productIds;           // какие товары; пусто - все
    bool onlyWithoutPhotos = true;   // только товарам, у которых ещё нет ни одного фото
    bool fromMarket = true;          // спрашивать Маркет по API
    bool fromCards = true;           // ссылки из карточек Маркета в программе
    bool fromWeb = true;             // искать в интернете по названию - тем, для кого нигде нет ссылок
    int perProduct = 1;              // сколько фото добавлять каждому товару (1..kMaxPhotosPerProduct)
};

struct Result {
    int considered = 0;              // товаров подходило под условия
    int withLinks = 0;               // для скольких нашлись ссылки на фото на Маркете и в карточках
    int fromWeb = 0;                 // скольким товарам фото найдено в интернете
    int productsFilled = 0;          // у скольких товаров добавилось хотя бы одно фото
    int added = 0;                   // всего добавлено фото
    int duplicates = 0;              // пропущено: такое же фото у товара уже есть
    int failed = 0;                  // не скачалось или не картинка
    QStringList noLinks;             // артикулы товаров, для которых фото нигде не нашлось
    QStringList sources;             // "артикул ← картинка", откуда взято фото из интернета (проверить, то ли нашлось)
    QStringList problems;            // подробности по ошибкам (не больше kMaxProblems)
    QStringList marketErrors;        // кабинеты, которые не ответили
    QStringList webErrors;           // что ответили поисковики, когда не ответили
    bool webBlocked = false;         // поисковики подряд не отвечали - поиск прекращён, остальные товары не обработаны
    bool cancelled = false;
};

constexpr int kMaxProblems = 50;

// Возвращает false из progress - остановить (уже сохранённые фото остаются). total == 0, пока ссылки собираются.
using Progress = std::function<bool(int done, int total, const QString &text)>;

Result run(const Options &options, const Progress &progress = {});

// Ссылка, пригодная для скачивания: "//host/..." получает https, остальные схемы кроме http и https отбрасываются,
// ссылки на фото с нашего же сайта (/pub/photo/) не нужны. Пустая строка - не подходит.
QString usableLink(const QString &link);

} // namespace PhotoFetch

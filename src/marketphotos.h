#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// Фото для карточек товаров, которые уже есть в базе: ссылки на фото берутся с самого Яндекс Маркета (кабинеты из
// таблицы market_accounts, по артикулу на Маркете - у товара без него по обычному артикулу) и из карточек Маркета,
// подготовленных в программе (market_cards.pictures). Картинки скачиваются, приводятся к JPEG и сохраняются у товара
// в общей базе (product_photos), поэтому их сразу видят все компьютеры и сайт.
namespace MarketPhotos {

struct Options {
    QList<int> productIds;           // какие товары; пусто - все
    bool onlyWithoutPhotos = true;   // только товарам, у которых ещё нет ни одного фото
    bool fromMarket = true;          // спрашивать Маркет по API
    bool fromCards = true;           // ссылки из карточек Маркета в программе
};

struct Result {
    int considered = 0;              // товаров подходило под условия
    int withLinks = 0;               // для скольких нашлись ссылки на фото
    int productsFilled = 0;          // у скольких товаров добавилось хотя бы одно фото
    int added = 0;                   // всего добавлено фото
    int duplicates = 0;              // пропущено: такое же фото у товара уже есть
    int failed = 0;                  // не скачалось или не картинка
    QStringList noLinks;             // артикулы товаров, для которых ссылок нет
    QStringList problems;            // подробности по ошибкам (не больше kMaxProblems)
    QStringList marketErrors;        // кабинеты, которые не ответили
    bool cancelled = false;
};

constexpr int kMaxProblems = 50;

// Возвращает false из progress - остановить (уже сохранённые фото остаются). total == 0, пока ссылки собираются.
using Progress = std::function<bool(int done, int total, const QString &text)>;

Result run(const Options &options, const Progress &progress = {});

// Ссылка, пригодная для скачивания: "//host/..." получает https, остальные схемы кроме http и https отбрасываются,
// ссылки на фото с нашего же сайта (/pub/photo/) не нужны. Пустая строка - не подходит.
QString usableLink(const QString &link);

} // namespace MarketPhotos

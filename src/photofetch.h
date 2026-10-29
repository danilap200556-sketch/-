#pragma once

#include "imagesearch.h"
#include "photostore.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// Фото для карточек товаров, которые уже есть в базе. Откуда берутся картинки (в таком порядке):
//  1) с самого Яндекс Маркета - кабинеты из таблицы market_accounts, по артикулу на Маркете (у товара без него - по
//     обычному артикулу);
//  2) из карточек Маркета, подготовленных в программе (market_cards.pictures);
//  3) из интернета ПО АРТИКУЛУ: открытые магазины, каталог New Balance, затем Google по ключу (см. ImageSearch). Среди
//     найденного «проверенными» считаются снимки, где артикул нашёлся в данных товара; остальные - только по желанию.
// Картинки скачиваются, приводятся к JPEG и сохраняются у товара в общей базе (product_photos), поэтому их сразу видят
// все компьютеры и сайт. Можно сначала только собрать найденное (collect), показать и сохранить выбранное (save).
namespace PhotoFetch {

struct Options {
    QList<int> productIds;           // какие товары; пусто - все
    bool onlyWithoutPhotos = true;   // только товарам, у которых ещё нет ни одного фото
    bool fromMarket = true;          // спрашивать Маркет по API
    bool fromCards = true;           // ссылки из карточек Маркета в программе
    bool fromWeb = true;             // искать в интернете по артикулу - тем, для кого нигде нет ссылок
    int webSources = ImageSearch::AllSources; // какие из магазинов / New Balance / Google (если fromWeb)
    ImageSearch::Settings google;    // ключ и cx Google Custom Search (можно пустые)
    int perProduct = 1;              // сколько фото добавлять каждому товару (1..kMaxPhotosPerProduct)
    bool replaceExisting = false;    // найдено новое - прежние фото товара удаляются (нет нового - остаются)
    bool acceptUnverified = false;   // сохранять и непроверенные снимки (артикул нигде не упомянут)
    // Готовые ссылки на фото по товарам (например, из Excel, см. PhotoExcel): скачиваются первыми и считаются проверенными.
    // Для «только из файла» выключите остальные источники и задайте productIds = ключи этого списка.
    QHash<int, QStringList> extraLinks;
};

struct Result {
    int considered = 0;              // товаров подходило под условия
    int withLinks = 0;               // для скольких нашлись ссылки на фото на Маркете и в карточках
    int fromWeb = 0;                 // скольким товарам фото найдено в интернете
    int productsFilled = 0;          // у скольких товаров добавилось хотя бы одно фото
    int added = 0;                   // всего добавлено фото
    int replaced = 0;                // удалено прежних фото при замене
    int duplicates = 0;              // пропущено: такое же фото у товара уже есть
    int unchanged = 0;               // товаров, у которых при замене нашлись те же самые фото - менять нечего
    int failed = 0;                  // не скачалось или не картинка
    QStringList noLinks;             // артикулы товаров, для которых фото нигде не нашлось
    QStringList allFailed;           // артикулы, у которых ссылки были, но ни одна не скачалась (и искать в интернете не просили)
    QStringList unverified;          // артикулы, для которых нашлись только непроверенные снимки (не сохранены)
    QStringList sources;             // "артикул ← источник, ссылка": откуда взято фото из интернета
    QStringList problems;            // подробности по ошибкам (не больше kMaxProblems)
    QStringList marketErrors;        // кабинеты, которые не ответили
    QStringList webErrors;           // что ответили источники в интернете, когда не ответили
    int skippedForPreview = 0;       // товаров не взято в режим просмотра: слишком много за раз (kMaxProposals)
    bool webBlocked = false;         // источники подряд не отвечали - поиск прекращён, остальные товары не обработаны
    bool cancelled = false;
};

constexpr int kMaxProblems = 50;

// Один найденный снимок.
struct Candidate {
    PhotoStore::Prepared photo;
    QString source;                  // «Яндекс Маркет», «Карточки Маркета», «Магазин: …», «New Balance», «Google»
    QString origin;                  // ссылка на картинку
    QString title;                   // подпись источника (если была)
    bool verified = true;            // артикул подтверждён; у Маркета и карточек всегда true
};

// Найденное для одного товара (режим просмотра).
struct Proposal {
    int productId = 0;
    QString sku;
    QString name;
    int existingPhotos = 0;
    QList<Candidate> candidates;     // лучшие первыми
};

// Выбор пользователя по одному товару.
struct Choice {
    int productId = 0;
    QString sku;
    QList<Candidate> chosen;
};

// Возвращает false из progress - остановить (уже сохранённые фото остаются). total == 0, пока ссылки собираются.
using Progress = std::function<bool(int done, int total, const QString &text)>;

// Находит и сразу сохраняет (только проверенное, если не включено acceptUnverified).
Result run(const Options &options, const Progress &progress = {});

// Находит, но ничего не сохраняет: кандидаты по товарам (не больше kMaxProposals товаров за раз, остальные - в
// result.noLinks не попадают, а считаются в result.considered; см. skippedForPreview).
constexpr int kMaxProposals = 150;
Result collect(const Options &options, QList<Proposal> *proposals, const Progress &progress = {});

// Сохраняет выбранное. replaceExisting - у каждого товара из choices с выбранными фото прежние фото удаляются.
Result save(const QList<Choice> &choices, bool replaceExisting);

// Ссылка, пригодная для скачивания: "//host/..." получает https, остальные схемы кроме http и https отбрасываются,
// ссылки на фото с нашего же сайта (/pub/photo/) не нужны. Пустая строка - не подходит.
QString usableLink(const QString &link);

} // namespace PhotoFetch

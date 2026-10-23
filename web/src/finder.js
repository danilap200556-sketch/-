'use strict';
const bc = require('./barcode');
const { likeEscape } = require('./util');

// Поиск товара по тому, что ввёл человек или отсканировал сканер: штрихкод, артикул, артикул на
// Маркете или часть названия. Те же правила, что у ProductFinder в приложении (src/productfinder.cpp).
// -> { ids: number[], exact: boolean }
async function findProducts(db, input, limit = 300) {
  const text = String(input ?? '').trim();
  if (!text) return { ids: [], exact: false };
  const code = bc.normalize(text);
  if (code) {
    const r = await db.query('SELECT product_id FROM product_barcodes WHERE barcode = $1', [code]);
    if (r.rowCount) return { ids: [r.rows[0].product_id], exact: true };
  }
  for (const column of ['sku', 'market_sku']) {
    const r = await db.query(`SELECT id FROM products WHERE lower(${column}) = lower($1) ORDER BY sku LIMIT 50`, [text]);
    if (r.rowCount) return { ids: r.rows.map((x) => x.id), exact: r.rowCount === 1 };
  }
  const like = `%${likeEscape(text)}%`;
  const r = await db.query(
    `SELECT p.id FROM products p
      WHERE p.sku ILIKE $1 OR p.name ILIKE $1 OR p.market_sku ILIKE $1
         OR EXISTS (SELECT 1 FROM product_barcodes b WHERE b.product_id = p.id AND b.barcode ILIKE $1)
      ORDER BY p.sku LIMIT ${Number(limit) | 0}`, [like]);
  return { ids: r.rows.map((x) => x.id), exact: false };
}

// Подсказки «по мере ввода»: товары, у которых введённое встречается в артикуле, названии, артикуле на Маркете или
// штрихкоде. Несколько слов через пробел - каждое должно встретиться («nike 43»). Сначала артикул целиком, потом
// артикулы, начинающиеся с введённого, остальные - по артикулу. Те же правила, что у ProductFinder::suggest в приложении.
// -> [{ id, sku, name, market_sku }]
async function suggestProducts(db, input, limit = 12) {
  const words = String(input ?? '').trim().split(/\s+/).filter(Boolean).slice(0, 5);
  if (!words.length) return [];
  const params = [];
  const where = words.map((w) => {
    params.push(`%${likeEscape(w)}%`);
    const n = params.length;
    return `(p.sku ILIKE $${n} OR p.name ILIKE $${n} OR p.market_sku ILIKE $${n}
             OR EXISTS (SELECT 1 FROM product_barcodes b WHERE b.product_id = p.id AND b.barcode ILIKE $${n}))`;
  });
  params.push(words.join(' '));
  const whole = params.length;
  const r = await db.query(
    `SELECT p.id, p.sku, p.name, COALESCE(p.market_sku, '') AS market_sku FROM products p
      WHERE ${where.join(' AND ')}
      ORDER BY (lower(p.sku) = lower($${whole})) DESC, starts_with(lower(p.sku), lower($${whole})) DESC, p.sku
      LIMIT ${Math.min(Math.max(Number(limit) | 0, 1), 50)}`, params);
  return r.rows;
}

// Только точное совпадение (для операций, которые что-то списывают/добавляют): штрихкод или артикул
// целиком. -> { id } | { error }
async function findExact(db, input) {
  const text = String(input ?? '').trim();
  const r = await findProducts(db, text);
  if (!r.ids.length) return { error: `Товар «${text}» не найден: нет такого артикула или штрихкода` };
  if (!r.exact) {
    // Несколько товаров с одним артикулом - выбирать за человека нельзя; частичное совпадение - тоже.
    const exactSku = await db.query('SELECT id FROM products WHERE lower(sku) = lower($1) OR lower(market_sku) = lower($1)', [text]);
    if (exactSku.rowCount === 1) return { id: exactSku.rows[0].id };
    return { error: exactSku.rowCount > 1 ? `Под «${text}» подходит несколько товаров - введите артикул или штрихкод точнее`
      : `Товар «${text}» не найден: нужен артикул или штрихкод целиком` };
  }
  return { id: r.ids[0] };
}

// Артикулы на Маркете (offerId) товаров, найденных по тексту: по ним сопоставляются строки заказов.
async function offerIdsFor(db, text) {
  const { ids } = await findProducts(db, text, 500);
  if (!ids.length) return new Set();
  const r = await db.query("SELECT COALESCE(NULLIF(TRIM(market_sku), ''), sku) AS offer FROM products WHERE id = ANY($1::int[])", [ids]);
  return new Set(r.rows.map((x) => x.offer));
}

module.exports = { findProducts, findExact, suggestProducts, offerIdsFor };

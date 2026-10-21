'use strict';
const { searchSql } = require('./routes/products');
const { csvCell } = require('./util');

// Выгрузка полных данных товаров (вкладка «Товары»): одна строка на товар, остатки по складам - отдельными
// колонками. Те же колонки и порядок, что у кнопки «Выгрузить в Excel» на вкладке «Товары» в приложении
// (src/productexport.cpp).

const MAX_ROWS = 100_000;
const tz = process.env.DISPLAY_TZ || 'Europe/Moscow';
const dayFmt = new Intl.DateTimeFormat('sv-SE', { timeZone: tz, year: 'numeric', month: '2-digit', day: '2-digit' });

const price = (v) => String(Math.round((Number(v) || 0) * 100) / 100);

// opts: { q } - товары, найденные по тексту поиска (пусто - все); или { ids: number[] } - только эти.
// -> { rows: string[][] (первая строка - заголовки), numericColumns, decimalColumns, columnWidths, count, truncated }
async function productTable(db, { q = '', ids = null } = {}) {
  let cond; let params;
  if (ids) { cond = 'p.id = ANY($1::int[])'; params = [ids]; } else { const s = searchSql(q, 1); cond = s.sql; params = s.params; }

  // по очереди: db может быть клиентом транзакции, у которого запросы идут друг за другом
  const products = await db.query(
      `SELECT p.id, p.sku, p.name, COALESCE(p.description, '') AS description, p.price::float8 AS price,
              COALESCE(p.market_sku, '') AS market_sku, COALESCE(p.custom_code, '') AS custom_code, p.created_at,
              COALESCE((SELECT string_agg(b.barcode, '; ' ORDER BY b.created_at, b.barcode) FROM product_barcodes b WHERE b.product_id = p.id), '') AS barcodes,
              (SELECT COUNT(*)::int FROM product_photos ph WHERE ph.product_id = p.id) AS photos
         FROM products p WHERE ${cond} ORDER BY p.sku LIMIT ${MAX_ROWS + 1}`, params);
  const whs = await db.query('SELECT id, name FROM warehouses ORDER BY name');
  const stock = await db.query(
      `SELECT s.product_id, s.warehouse_id, s.quantity, l.code AS location
         FROM stock s LEFT JOIN locations l ON l.id = s.location_id
        WHERE s.product_id IN (SELECT p.id FROM products p WHERE ${cond})`, params);

  const truncated = products.rows.length > MAX_ROWS;
  const items = products.rows.slice(0, MAX_ROWS);
  const byProduct = new Map();
  for (const s of stock.rows) {
    if (!byProduct.has(s.product_id)) byProduct.set(s.product_id, new Map());
    byProduct.get(s.product_id).set(s.warehouse_id, s);
  }

  const head = ['Артикул', 'Название', 'Описание', 'Цена, ₽', 'Артикул на Маркете', 'Свой код', 'Штрихкоды', 'Всего на складах',
    ...whs.rows.map((w) => `Остаток: ${w.name}`), 'Места хранения', 'Фото, шт', 'Добавлен'];
  const rows = [head];
  for (const p of items) {
    const own = byProduct.get(p.id) || new Map();
    const perWarehouse = whs.rows.map((w) => own.get(w.id)?.quantity ?? 0);
    const places = whs.rows.filter((w) => own.get(w.id)?.location).map((w) => `${w.name}: ${own.get(w.id).location}`);
    rows.push([p.sku, p.name, p.description, price(p.price), p.market_sku, p.custom_code, p.barcodes,
      String(perWarehouse.reduce((a, b) => a + b, 0)), ...perWarehouse.map(String), places.join('; '),
      String(p.photos), p.created_at ? dayFmt.format(new Date(p.created_at)) : '']);
  }

  const n = whs.rows.length;
  return {
    rows,
    numericColumns: [3, 7, ...whs.rows.map((_, i) => 8 + i), 9 + n],
    decimalColumns: [3],
    columnWidths: [22, 45, 40, 11, 22, 16, 30, 12, ...whs.rows.map(() => 14), 28, 9, 12],
    count: items.length,
    truncated,
  };
}

// CSV для Excel (разделитель «;», BOM): цена с запятой, как ждёт русский Excel; защита от формул.
function toCsv(table) {
  const dec = new Set(table.decimalColumns);
  const lines = table.rows.map((r, i) => r.map((v, c) => csvCell(i > 0 && dec.has(c) ? String(v).replace('.', ',') : v)).join(';'));
  return '﻿' + lines.join('\r\n') + '\r\n';
}

module.exports = { productTable, toCsv, MAX_ROWS };

'use strict';
const bc = require('./barcode');
const ops = require('./stock');
const { findExact } = require('./finder');

// Приход / списание списком: много позиций одним документом, всё сразу или ничего.
// Тот же формат строк, что у диалога «Приход / списание списком» в приложении (src/batchparser.cpp).

const MAX_LINES = 2000;
const MAX_TEXT = 200_000;

// Строка списка:  КОД            -> 1 шт  (так выглядит каждый скан сканером)
//                 КОД 5          -> 5 шт
//                 КОД;5  КОД<Tab>5  (вставка из Excel: лишние колонки справа игнорируются)
// КОД - штрихкод, артикул или артикул на Маркете. -> [{ n, key, qty, error? }]
function parseLines(text) {
  const out = [];
  const raw = String(text ?? '').slice(0, MAX_TEXT).split(/\r\n|\n|\r/);
  raw.forEach((rawLine, i) => {
    const line = rawLine.replace(/ /g, ' ').trim();
    if (!line) return;
    const item = { n: i + 1, key: '', qty: 1 };
    let qtyRaw = '';
    const compact = bc.normalize(line);
    if (/^\d{8,14}$/.test(compact) && !bc.validate(compact)) {
      item.key = compact; // целиком штрихкод (возможно, с пробелами) - одна штука
    } else if (/[\t;]/.test(line)) {
      const parts = line.split(/[\t;]/);
      item.key = parts[0].trim();
      qtyRaw = (parts[1] ?? '').trim();
    } else {
      const m = /^(.*\S)\s+(\S+)$/.exec(line);
      if (m && /^[\d.,]+$/.test(m[2])) { item.key = m[1].trim(); qtyRaw = m[2]; } else item.key = line;
    }
    if (!item.key) item.error = 'нет артикула или штрихкода';
    if (qtyRaw !== '') {
      const q = /^(\d+)(?:[.,]0+)?$/.exec(qtyRaw);
      if (!q) item.error = item.error || `количество «${qtyRaw}» - нужно целое число`;
      else if (Number(q[1]) === 0) item.error = item.error || 'количество должно быть больше нуля';
      else if (Number(q[1]) > ops.MAX_QTY) item.error = item.error || 'слишком большое количество';
      else item.qty = Number(q[1]);
    }
    out.push(item);
  });
  return out;
}

// Находит товары, складывает повторы одного товара. kind: 'receipt' | 'writeoff'.
// -> { items: [{ id, sku, name, cover_id, qty, lines: [n], current, after }], problems: [{ n, text }], totalQty }
async function prepare(db, text, kind, warehouseId) {
  const lines = parseLines(text);
  const problems = [];
  if (!lines.length) return { items: [], problems: [{ n: 0, text: 'Список пуст: впишите или отсканируйте хотя бы одну позицию' }], totalQty: 0 };
  if (lines.length > MAX_LINES) return { items: [], problems: [{ n: 0, text: `Слишком много строк (больше ${MAX_LINES}) - разбейте список на части` }], totalQty: 0 };

  const cache = new Map();
  const byId = new Map();
  for (const l of lines) {
    if (l.error) { problems.push({ n: l.n, text: `${l.key || '(пусто)'}: ${l.error}` }); continue; }
    if (!cache.has(l.key)) cache.set(l.key, await findExact(db, l.key));
    const hit = cache.get(l.key);
    if (hit.error) { problems.push({ n: l.n, text: hit.error }); continue; }
    const it = byId.get(hit.id) || { id: hit.id, qty: 0, lines: [] };
    it.qty += l.qty; it.lines.push(l.n);
    byId.set(hit.id, it);
  }
  const items = [...byId.values()].sort((a, b) => a.id - b.id);
  if (items.length) {
    const info = (await db.query(
      `SELECT p.id, p.sku, p.name, (SELECT id FROM product_photos ph WHERE ph.product_id = p.id ORDER BY ph.position, ph.id LIMIT 1) AS cover_id,
              COALESCE(s.quantity, 0)::int AS current
         FROM products p LEFT JOIN stock s ON s.product_id = p.id AND s.warehouse_id = $2 WHERE p.id = ANY($1::int[])`,
      [items.map((i) => i.id), warehouseId])).rows;
    const m = new Map(info.map((r) => [r.id, r]));
    for (const it of items) {
      Object.assign(it, m.get(it.id));
      it.after = kind === 'writeoff' ? it.current - it.qty : it.current + it.qty;
      if (it.qty > ops.MAX_QTY) problems.push({ n: it.lines[0], text: `${it.sku}: слишком большое количество (${it.qty})` });
      else if (kind === 'writeoff' && it.after < 0) problems.push({ n: it.lines[0], text: `${it.sku}: на складе ${it.current} шт, а списать нужно ${it.qty} шт` });
      else if (it.after > 2_000_000_000) problems.push({ n: it.lines[0], text: `${it.sku}: остаток получится слишком большим` });
    }
  }
  problems.sort((a, b) => a.n - b.n);
  return { items, problems, totalQty: items.reduce((n, i) => n + i.qty, 0) };
}

// Применяет всё одной транзакцией. Если хоть одна позиция не проходит - не меняется ничего.
// -> { ok: true, items } | { ok: false, problems }
async function apply(db, text, kind, warehouseId, comment) {
  let result;
  try {
    result = await db.tx(async (c) => {
      const prep = await prepare({ query: (...a) => c.query(...a) }, text, kind, warehouseId);
      if (prep.problems.length) return { ok: false, problems: prep.problems };
      // блокируем строки остатков в одном порядке (по товару), чтобы две партии не зашли в тупик
      const problems = [];
      const doc = ops.newDoc(); // все позиции - один документ: отменяется разом
      for (const it of prep.items) {
        try {
          if (kind === 'writeoff') await ops.takeOut(c, it.id, warehouseId, it.qty, comment, 'writeoff', null, doc);
          else await ops.receive(c, it.id, warehouseId, it.qty, comment, 'receipt', null, doc);
        } catch (e) {
          if (!(e instanceof ops.UserError)) throw e;
          problems.push({ n: it.lines[0], text: `${it.sku}: ${e.message}` });
        }
      }
      if (problems.length) throw Object.assign(new Error('batch-rollback'), { problems });
      return { ok: true, items: prep.items, totalQty: prep.totalQty };
    });
  } catch (e) {
    if (e.problems) return { ok: false, problems: e.problems };
    throw e;
  }
  return result;
}

module.exports = { parseLines, prepare, apply, MAX_LINES, MAX_TEXT };

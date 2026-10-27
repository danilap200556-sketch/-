'use strict';
// Инвентаризация склада по штрихкодам: те же правила, что у модуля Stocktake в приложении (src/stocktake.cpp).
// Подсчёт (stocktakes) открывается на склад и общий для всех; каждый скан добавляет штуку товару (stocktake_items).
// Завершение: остаток посчитанных товаров становится равным посчитанному, а у товаров склада, которых в подсчёте нет,
// - нулю (если отмечено). Всё одним документом «Инвентаризация» в одной транзакции - его можно отменить в «Документах».
const ops = require('./stock');

const MAX_COUNT = 100_000_000;

// Таблицы создаёт настольное приложение при подключении. Пока их нет, раздел показывает подсказку.
let ready = false;
let checkedAt = 0;
async function supported(q) {
  if (ready) return true;
  const now = Date.now();
  if (now - checkedAt < 15_000) return false;
  checkedAt = now;
  const r = await q.query(
    `SELECT COUNT(*)::int AS n FROM information_schema.tables
      WHERE table_schema = current_schema() AND table_name IN ('stocktakes', 'stocktake_items')`);
  ready = r.rows[0].n === 2;
  return ready;
}
const resetFeatureCache = () => { ready = false; checkedAt = 0; };

const INFO_SQL = `
  SELECT st.id, st.warehouse_id, w.name AS warehouse, st.status, st.started_by, st.started_at, st.finished_at, st.finished_by,
         st.document_id, st.zero_missing,
         (SELECT COUNT(*)::int FROM stocktake_items i WHERE i.stocktake_id = st.id) AS positions,
         (SELECT COALESCE(SUM(i.counted), 0)::bigint FROM stocktake_items i WHERE i.stocktake_id = st.id) AS units
    FROM stocktakes st JOIN warehouses w ON w.id = st.warehouse_id`;

async function current(q, warehouseId) {
  return (await q.query(`${INFO_SQL} WHERE st.warehouse_id = $1 AND st.status = 'open'`, [warehouseId])).rows[0] ?? null;
}
async function get(q, id) {
  return (await q.query(`${INFO_SQL} WHERE st.id = $1`, [id])).rows[0] ?? null;
}
// Открытые подсчёты всех складов и последние завершённые.
async function overview(q) {
  const open = (await q.query(`${INFO_SQL} WHERE st.status = 'open' ORDER BY w.name`)).rows;
  const last = (await q.query(`${INFO_SQL} WHERE st.status <> 'open' ORDER BY st.id DESC LIMIT 15`)).rows;
  return { open, last };
}

// Открытый подсчёт склада (создаёт, если нет). -> id
async function start(q, warehouseId, user = '') {
  await q.query(
    `INSERT INTO stocktakes (warehouse_id, started_by) VALUES ($1, $2)
     ON CONFLICT (warehouse_id) WHERE status = 'open' DO NOTHING`, [warehouseId, user]);
  const cur = await current(q, warehouseId);
  if (!cur) throw new ops.UserError('Не удалось начать подсчёт: склад не найден.');
  return cur.id;
}

const ROW_SQL = `
  SELECT up.product_id, p.sku, p.name, COALESCE(s.quantity, 0)::int AS book, up.counted
    FROM up JOIN products p ON p.id = up.product_id JOIN stocktakes st ON st.id = up.stocktake_id
    LEFT JOIN stock s ON s.product_id = up.product_id AND s.warehouse_id = st.warehouse_id`;

// +delta штук товару (delta может быть отрицательным). -> { product_id, sku, name, book, counted }
async function add(q, id, productId, delta, user = '') {
  if (!Number.isInteger(delta) || Math.abs(delta) > MAX_COUNT) throw new ops.ValidationError('Слишком большое количество.');
  let r;
  try {
    r = await q.query(
      `WITH up AS (INSERT INTO stocktake_items AS i (stocktake_id, product_id, counted, updated_by)
         SELECT st.id, $2::int, GREATEST($3::int, 0), $4::text FROM stocktakes st WHERE st.id = $1::int AND st.status = 'open'
         ON CONFLICT (stocktake_id, product_id) DO UPDATE
           SET counted = LEAST(GREATEST(i.counted + $3::int, 0), ${MAX_COUNT}), updated_at = NOW(), updated_by = EXCLUDED.updated_by
         RETURNING stocktake_id, product_id, counted) ${ROW_SQL}`, [id, productId, delta, user]);
  } catch (e) {
    if (e.code === '23503') throw new ops.UserError('Товар уже удалён.');
    throw e;
  }
  if (!r.rowCount) throw new ops.UserError('Подсчёт уже завершён или сброшен: начните новый.');
  return r.rows[0];
}

// Задаёт посчитанное количество; отрицательное убирает товар из подсчёта.
async function setCounted(q, id, productId, counted, user = '') {
  if (!Number.isInteger(counted) || counted > MAX_COUNT) throw new ops.ValidationError('Количество - целое число от 0 до 100 000 000.');
  if (counted < 0) {
    const r = await q.query(
      `DELETE FROM stocktake_items i USING stocktakes st WHERE i.stocktake_id = $1 AND i.product_id = $2 AND st.id = i.stocktake_id AND st.status = 'open'`,
      [id, productId]);
    if (!r.rowCount && (await get(q, id))?.status !== 'open') throw new ops.UserError('Подсчёт уже завершён или сброшен.');
    return null;
  }
  let r;
  try {
    r = await q.query(
      `WITH up AS (INSERT INTO stocktake_items (stocktake_id, product_id, counted, updated_by)
         SELECT st.id, $2::int, $3::int, $4::text FROM stocktakes st WHERE st.id = $1::int AND st.status = 'open'
         ON CONFLICT (stocktake_id, product_id) DO UPDATE SET counted = EXCLUDED.counted, updated_at = NOW(), updated_by = EXCLUDED.updated_by
         RETURNING stocktake_id, product_id, counted) ${ROW_SQL}`, [id, productId, counted, user]);
  } catch (e) {
    if (e.code === '23503') throw new ops.UserError('Товар уже удалён.');
    throw e;
  }
  if (!r.rowCount) throw new ops.UserError('Подсчёт уже завершён или сброшен: начните новый.');
  return r.rows[0];
}

const LIST_COLS = 'p.id AS product_id, p.sku, p.name, COALESCE(s.quantity, 0)::int AS book';
// Посчитанное: последние сканы сверху. onlyDiff - только расхождения. -> { rows, total }
async function countedRows(q, id, { onlyDiff = false, limit = 300 } = {}) {
  const base = `FROM stocktake_items i JOIN stocktakes st ON st.id = i.stocktake_id JOIN products p ON p.id = i.product_id
    LEFT JOIN stock s ON s.product_id = i.product_id AND s.warehouse_id = st.warehouse_id
    WHERE i.stocktake_id = $1 ${onlyDiff ? 'AND i.counted <> COALESCE(s.quantity, 0)' : ''}`;
  const rows = (await q.query(`SELECT ${LIST_COLS}, i.counted ${base} ORDER BY i.updated_at DESC, p.sku LIMIT ${Number(limit) | 0}`, [id])).rows;
  const total = (await q.query(`SELECT COUNT(*)::int AS n ${base}`, [id])).rows[0].n;
  return { rows, total };
}
// Числится на складе, в подсчёте нет. -> { rows, total }
async function missingRows(q, id, { limit = 300 } = {}) {
  const base = `FROM stocktakes st JOIN stock s ON s.warehouse_id = st.warehouse_id JOIN products p ON p.id = s.product_id
    WHERE st.id = $1 AND s.quantity <> 0
      AND NOT EXISTS (SELECT 1 FROM stocktake_items i WHERE i.stocktake_id = st.id AND i.product_id = s.product_id)`;
  const rows = (await q.query(`SELECT ${LIST_COLS}, 0 AS counted ${base} ORDER BY p.sku LIMIT ${Number(limit) | 0}`, [id])).rows;
  const total = (await q.query(`SELECT COUNT(*)::int AS n ${base}`, [id])).rows[0].n;
  return { rows, total };
}

async function summary(q, id) {
  const s = (await q.query(
    `SELECT COUNT(*)::int AS positions, COALESCE(SUM(i.counted), 0)::bigint AS units,
            (COUNT(*) FILTER (WHERE i.counted <> COALESCE(sk.quantity, 0)))::int AS diff_positions,
            COALESCE(SUM(GREATEST(i.counted - COALESCE(sk.quantity, 0), 0)), 0)::bigint AS surplus,
            COALESCE(SUM(GREATEST(COALESCE(sk.quantity, 0) - i.counted, 0)), 0)::bigint AS shortage
       FROM stocktake_items i JOIN stocktakes st ON st.id = i.stocktake_id
       LEFT JOIN stock sk ON sk.product_id = i.product_id AND sk.warehouse_id = st.warehouse_id
      WHERE i.stocktake_id = $1`, [id])).rows[0];
  const m = (await q.query(
    `SELECT COUNT(*)::int AS n, COALESCE(SUM(s.quantity), 0)::bigint AS units
       FROM stocktakes st JOIN stock s ON s.warehouse_id = st.warehouse_id
      WHERE st.id = $1 AND s.quantity <> 0
        AND NOT EXISTS (SELECT 1 FROM stocktake_items i WHERE i.stocktake_id = st.id AND i.product_id = s.product_id)`, [id])).rows[0];
  return {
    positions: s.positions, units: Number(s.units), diffPositions: s.diff_positions, surplus: Number(s.surplus), shortage: Number(s.shortage),
    missingPositions: m.n, missingUnits: Number(m.units),
  };
}

// Проводит подсчёт одним документом. -> { changed, documentId }
async function finish(db, id, { zeroMissing = true, user = '' } = {}) {
  return db.tx(async (c) => {
    const closed = await c.query(
      `UPDATE stocktakes SET status = 'done', finished_at = NOW(), finished_by = $2, zero_missing = $3
        WHERE id = $1 AND status = 'open' RETURNING warehouse_id`, [id, user, Boolean(zeroMissing)]);
    if (!closed.rowCount) throw new ops.UserError('Подсчёт уже завершён или сброшен.');
    const warehouseId = closed.rows[0].warehouse_id;
    const warehouse = (await c.query('SELECT name FROM warehouses WHERE id = $1', [warehouseId])).rows[0]?.name ?? '';
    const comment = `инвентаризация склада «${warehouse}» №${id}`;
    const counted = (await c.query('SELECT product_id, counted FROM stocktake_items WHERE stocktake_id = $1', [id])).rows;
    // Остатки склада блокируем: пока идёт запись, их не изменит другое окно, поэтому «было» верно.
    const have = new Map((await c.query('SELECT product_id, quantity FROM stock WHERE warehouse_id = $1 FOR UPDATE', [warehouseId])).rows
      .map((r) => [r.product_id, r.quantity]));
    const changes = [];
    const seen = new Set();
    for (const r of counted) {
      seen.add(r.product_id);
      const from = have.get(r.product_id) ?? 0;
      if (from !== r.counted) changes.push({ productId: r.product_id, from, to: r.counted });
    }
    if (zeroMissing) {
      for (const [pid, qty] of [...have].sort((a, b) => a[0] - b[0])) if (qty !== 0 && !seen.has(pid)) changes.push({ productId: pid, from: qty, to: 0 });
    }
    const doc = ops.newDoc('inventory', comment, user);
    if (changes.length) await ops.setLevels(c, warehouseId, changes, comment, doc);
    let documentId = null;
    if (changes.length && await ops.docsSupported(c)) {
      documentId = (await c.query('SELECT id FROM stock_documents WHERE doc_key = $1', [doc.key])).rows[0]?.id ?? null;
    }
    await c.query('UPDATE stocktakes SET document_id = $2 WHERE id = $1', [id, documentId]);
    return { changed: changes.length, documentId };
  });
}

async function discard(q, id, user = '') {
  const r = await q.query(
    `UPDATE stocktakes SET status = 'discarded', finished_at = NOW(), finished_by = $2 WHERE id = $1 AND status = 'open'`, [id, user]);
  if (!r.rowCount) throw new ops.UserError('Подсчёт уже завершён или сброшен.');
}

module.exports = { MAX_COUNT, supported, resetFeatureCache, current, get, overview, start, add, setCounted, countedRows, missingRows, summary, finish, discard };

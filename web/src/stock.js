'use strict';
// Операции с остатками. Типы движений и комментарии - те же, что у настольного
// приложения (src/database.cpp), чтобы журнал выглядел одинаково с обеих сторон.
// В отличие от приложения, списание проверяет остаток прямо в UPDATE, поэтому два
// одновременных списания (сайт + приложение) не уведут остаток в минус.

const crypto = require('crypto');

const MAX_QTY = 100_000_000;

class UserError extends Error {}
// Неверный ввод (400), в отличие от конфликта с текущим состоянием (409).
class ValidationError extends UserError {}

function qty(value, { allowZero = false } = {}) {
  const s = String(value ?? '').trim();
  if (!/^\d+$/.test(s)) throw new ValidationError('Количество - целое неотрицательное число');
  const n = Number(s);
  if (n > MAX_QTY) throw new ValidationError('Слишком большое количество');
  if (n === 0 && !allowZero) throw new ValidationError('Количество должно быть больше нуля');
  return n;
}

// Документ - одна операция склада (приход, списание, перемещение, инвентаризация, импорт, отмена): внутри одна
// или много записей журнала, номер виден в разделе «Документы», отменить можно целиком. Как и в приложении,
// строка документа (stock_documents) создаётся вместе с первой записью, поэтому у неудавшейся операции номера нет.
// doc = null - запись без документа (так работают старые вызовы и тесты).
function newDoc(kind = 'other', comment = '', user = '') {
  return {
    key: crypto.randomUUID(),
    kind,
    comment: String(comment ?? '').trim().slice(0, 300),
    user: String(user ?? '').trim().slice(0, 100),
    saved: false,
  };
}
// После ROLLBACK TO SAVEPOINT строка документа могла откатиться вместе с записью (импорт: строка = точка сохранения).
const docRolledBack = (doc) => { if (doc) doc.saved = false; };

// Столбцы журнала doc_id и reverts_id создаёт настольное приложение (свежая версия, один запуск с этой базой).
// Пока их нет, сайт работает по-старому: записи без документа, отмены нет. Проверка - раз в 15 секунд,
// после первого успеха больше не повторяется.
let cancelReady = false;
let checkedAt = 0;
async function cancelSupported(q) {
  if (cancelReady) return true;
  const now = Date.now();
  if (now - checkedAt < 15_000) return false;
  checkedAt = now;
  const r = await q.query(
    `SELECT COUNT(*)::int AS n FROM information_schema.columns
      WHERE table_schema = current_schema() AND table_name = 'stock_movements' AND column_name IN ('doc_id', 'reverts_id')`);
  cancelReady = r.rows[0].n === 2;
  return cancelReady;
}

// Таблицу stock_documents создаёт настольное приложение при подключении (как и столбцы выше). Без неё записи
// получают только doc_id, а раздел «Документы» показывает подсказку.
let docsReady = false;
let docsCheckedAt = 0;
async function docsSupported(q) {
  if (docsReady) return true;
  const now = Date.now();
  if (now - docsCheckedAt < 15_000) return false;
  docsCheckedAt = now;
  const r = await q.query(
    `SELECT COUNT(*)::int AS n FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 'stock_documents'`);
  docsReady = r.rows[0].n === 1;
  return docsReady;
}
const resetFeatureCache = () => { cancelReady = false; checkedAt = 0; docsReady = false; docsCheckedAt = 0; };

// WHERE NOT EXISTS вместо ON CONFLICT: номер (identity) тратится только на реально создаваемую строку.
async function ensureDoc(c, doc) {
  if (doc.saved) return;
  await c.query(
    `INSERT INTO stock_documents (doc_key, kind, comment, username)
     SELECT $1::text, $2::text, $3::text, $4::text WHERE NOT EXISTS (SELECT 1 FROM stock_documents WHERE doc_key = $1)`,
    [doc.key, doc.kind, doc.comment, doc.user]);
  doc.saved = true;
}

async function addMovement(c, productId, warehouseId, related, type, delta, comment, doc = null, reverts = null) {
  if (await cancelSupported(c)) {
    if (doc && await docsSupported(c)) await ensureDoc(c, doc);
    await c.query(
      `INSERT INTO stock_movements (product_id, warehouse_id, related_warehouse_id, type, delta, comment, doc_id, reverts_id)
       VALUES ($1, $2, $3, $4, $5, $6, $7, $8)`,
      [productId, warehouseId, related, type, delta, comment, doc ? doc.key : null, reverts]);
  } else {
    await c.query(
      `INSERT INTO stock_movements (product_id, warehouse_id, related_warehouse_id, type, delta, comment)
       VALUES ($1, $2, $3, $4, $5, $6)`,
      [productId, warehouseId, related, type, delta, comment]);
  }
}

async function receive(c, productId, warehouseId, n, comment, type = 'receipt', related = null, doc = null) {
  await c.query(
    `INSERT INTO stock (product_id, warehouse_id, quantity) VALUES ($1, $2, $3)
     ON CONFLICT (product_id, warehouse_id) DO UPDATE SET quantity = stock.quantity + EXCLUDED.quantity`,
    [productId, warehouseId, n]);
  await addMovement(c, productId, warehouseId, related, type, n, comment, doc);
}

async function takeOut(c, productId, warehouseId, n, comment, type = 'writeoff', related = null, doc = null) {
  const r = await c.query(
    `UPDATE stock SET quantity = quantity - $3
      WHERE product_id = $1 AND warehouse_id = $2 AND quantity >= $3 RETURNING quantity`,
    [productId, warehouseId, n]);
  if (r.rowCount === 0) {
    const have = await c.query('SELECT quantity FROM stock WHERE product_id = $1 AND warehouse_id = $2',
      [productId, warehouseId]);
    throw new UserError(`Недостаточно остатка: на складе ${have.rows[0]?.quantity ?? 0} шт, требуется списать ${n} шт`);
  }
  await addMovement(c, productId, warehouseId, related, type, -n, comment, doc);
}

async function transfer(c, productId, fromId, toId, n, comment, doc = newDoc('transfer', comment)) {
  if (fromId === toId) throw new UserError('Исходный и целевой склад совпадают');
  await takeOut(c, productId, fromId, n, comment, 'transfer_out', toId, doc);
  await receive(c, productId, toId, n, comment, 'transfer_in', fromId, doc);
}

// counted - посчитанное фактическое количество; пишется разница с текущим.
async function inventory(c, productId, warehouseId, counted, comment, doc = null) {
  await c.query(
    `INSERT INTO stock (product_id, warehouse_id, quantity) VALUES ($1, $2, 0)
     ON CONFLICT (product_id, warehouse_id) DO NOTHING`, [productId, warehouseId]);
  const cur = (await c.query(
    'SELECT quantity FROM stock WHERE product_id = $1 AND warehouse_id = $2 FOR UPDATE',
    [productId, warehouseId])).rows[0].quantity;
  const delta = counted - cur;
  if (delta === 0) return;
  const full = `${comment ? comment + ' ' : ''}(инвентаризация: было ${cur}, стало ${counted})`;
  await c.query('UPDATE stock SET quantity = $3 WHERE product_id = $1 AND warehouse_id = $2',
    [productId, warehouseId, counted]);
  await addMovement(c, productId, warehouseId, null, 'inventory_adjust', delta, full, doc);
}

// Инвентаризация склада: остатки товаров из changes ([{ productId, from, to }]) становятся равными to - одним
// запросом на пачку (в подсчёте могут быть тысячи товаров). Пишет в журнал по записи на товар; вызывать внутри
// транзакции, остатки склада к этому моменту должны быть заблокированы (SELECT ... FOR UPDATE).
async function setLevels(c, warehouseId, changes, comment, doc = null) {
  const CHUNK = 1000;
  for (let i = 0; i < changes.length; i += CHUNK) {
    const part = changes.slice(i, i + CHUNK);
    const pids = part.map((x) => x.productId);
    const to = part.map((x) => x.to);
    const deltas = part.map((x) => x.to - x.from);
    const comments = part.map((x) => `${comment ? comment + ' ' : ''}(инвентаризация: было ${x.from}, стало ${x.to})`);
    await c.query(
      `INSERT INTO stock (product_id, warehouse_id, quantity)
       SELECT x.pid, $2::int, x.q FROM unnest($1::int[], $3::int[]) AS x(pid, q)
       ON CONFLICT (product_id, warehouse_id) DO UPDATE SET quantity = EXCLUDED.quantity`,
      [pids, warehouseId, to]);
    if (await cancelSupported(c)) {
      if (doc && await docsSupported(c)) await ensureDoc(c, doc);
      await c.query(
        `INSERT INTO stock_movements (product_id, warehouse_id, type, delta, comment, doc_id)
         SELECT x.pid, $2::int, 'inventory_adjust', x.d, x.c, $5::text FROM unnest($1::int[], $3::int[], $4::text[]) AS x(pid, d, c)`,
        [pids, warehouseId, deltas, comments, doc ? doc.key : null]);
    } else {
      await c.query(
        `INSERT INTO stock_movements (product_id, warehouse_id, type, delta, comment)
         SELECT x.pid, $2::int, 'inventory_adjust', x.d, x.c FROM unnest($1::int[], $3::int[], $4::text[]) AS x(pid, d, c)`,
        [pids, warehouseId, deltas, comments]);
    }
  }
}

const TYPE_LABELS = {
  receipt: 'Приход',
  writeoff: 'Списание',
  transfer_out: 'Перемещение (расход)',
  transfer_in: 'Перемещение (приход)',
  inventory_adjust: 'Инвентаризация',
  cancel: 'Отмена',
};

// ---- Отмена операций
// Журнал только дополняется: отмена - это новая запись (тип cancel) с противоположным изменением и ссылкой
// reverts_id на отменяемую. Одну запись можно отменить один раз (уникальный индекс), саму отмену - нельзя
// (проведите операцию заново). Остаток не должен уйти в минус: если после операции товар уже расходован,
// отмена отклоняется целиком. Перемещение всегда отменяется парой записей.
class CancelRefused extends UserError {
  constructor(problems) {
    super(problems.length === 1 ? problems[0] : `Отменить нельзя:\n- ${problems.join('\n- ')}`);
    this.problems = problems;
  }
}

const ROW_SQL = `SELECT m.id, m.product_id, p.sku, p.name, m.warehouse_id, w.name AS wname, m.related_warehouse_id,
                        m.type, m.delta, m.comment, m.created_at, m.doc_id,
                        (SELECT r.id FROM stock_movements r WHERE r.reverts_id = m.id) AS reverted_by
                   FROM stock_movements m JOIN products p ON p.id = m.product_id JOIN warehouses w ON w.id = m.warehouse_id`;

// Все записи документа, к которому относится запись id (кроме отмен). Без документа - только она сама.
async function documentIds(q, id) {
  const r = await q.query(
    `SELECT m.id FROM stock_movements m
      WHERE m.type <> 'cancel' AND (m.id = $1 OR m.doc_id = (SELECT doc_id FROM stock_movements WHERE id = $1))
      ORDER BY m.id`, [id]);
  return r.rows.map((x) => x.id);
}

// Что произойдёт при отмене записей ids (ничего не меняет; lock - внутри транзакции, с блокировкой строк).
// -> { rows: [{..., current, after}], problems: string[] }
async function planCancel(q, ids, { lock = false } = {}) {
  const want = [...new Set(ids.map(Number).filter((n) => Number.isInteger(n) && n > 0 && n <= 2147483647))];
  const problems = [];
  if (!want.length) return { rows: [], problems: ['Не выбрано ни одной записи журнала'] };
  const forUpdate = lock ? ' FOR UPDATE OF m' : '';
  const found = new Map();
  for (const r of (await q.query(`${ROW_SQL} WHERE m.id = ANY($1::int[]) ORDER BY m.id${forUpdate}`, [want])).rows) found.set(r.id, r);
  for (const id of want) if (!found.has(id)) problems.push(`Запись №${id} не найдена (возможно, товар или склад уже удалены)`);

  // Перемещение - это две записи: расход и приход. Отменяются только вместе.
  for (const r of [...found.values()]) {
    if (r.type !== 'transfer_out' && r.type !== 'transfer_in') continue;
    // Время сравнивается внутри базы (с точностью до микросекунд), а не в JS: Date теряет микросекунды.
    const mates = (await q.query(
      `${ROW_SQL} JOIN stock_movements o ON o.id = $1
        WHERE m.id <> o.id AND m.type IN ('transfer_out', 'transfer_in') AND (
          (o.doc_id IS NOT NULL AND m.doc_id = o.doc_id)
          OR (o.doc_id IS NULL AND m.doc_id IS NULL AND m.product_id = o.product_id AND m.created_at = o.created_at
              AND m.type <> o.type AND m.warehouse_id = o.related_warehouse_id AND m.related_warehouse_id = o.warehouse_id
              AND m.delta = -o.delta))
        ORDER BY m.id${forUpdate}`, [r.id])).rows;
    for (const m of mates) found.set(m.id, m);
  }

  const rows = [...found.values()].sort((a, b) => a.id - b.id);
  const ok = [];
  for (const r of rows) {
    if (r.type === 'cancel') problems.push(`Запись №${r.id} сама является отменой - отменить её нельзя. Если нужно вернуть как было, проведите операцию заново`);
    else if (r.reverted_by) problems.push(`Запись №${r.id} (${r.sku}, ${TYPE_LABELS[r.type] || r.type}) уже отменена записью №${r.reverted_by}`);
    else ok.push(r);
  }

  // Остаток не должен уйти в минус: считаем итог по каждому товару и складу.
  const net = new Map();
  for (const r of ok) {
    const k = `${r.product_id}:${r.warehouse_id}`;
    net.set(k, { pid: r.product_id, wid: r.warehouse_id, sku: r.sku, wname: r.wname, sum: (net.get(k)?.sum ?? 0) - r.delta });
  }
  const after = new Map();
  for (const e of [...net.values()].sort((a, b) => a.pid - b.pid || a.wid - b.wid)) {
    const cur = (await q.query(
      `SELECT quantity FROM stock WHERE product_id = $1 AND warehouse_id = $2${lock ? ' FOR UPDATE' : ''}`, [e.pid, e.wid])).rows[0]?.quantity ?? 0;
    after.set(`${e.pid}:${e.wid}`, { cur, next: cur + e.sum });
    if (cur + e.sum < 0) {
      problems.push(`${e.sku}, склад «${e.wname}»: отмена уберёт ${-e.sum} шт, а на складе сейчас ${cur} шт - товар уже расходован после этой операции`);
    } else if (cur + e.sum > 2_000_000_000) {
      problems.push(`${e.sku}, склад «${e.wname}»: остаток получится слишком большим`);
    }
  }
  return {
    rows: ok.map((r) => ({ ...r, current: after.get(`${r.product_id}:${r.warehouse_id}`).cur, after: after.get(`${r.product_id}:${r.warehouse_id}`).next })),
    problems,
  };
}

// Отменяет записи ids одной транзакцией: либо все (с их парами по перемещению), либо ничего.
// -> { rows } (то, что было отменено). Бросает CancelRefused со списком причин.
async function cancel(db, ids, reason = '', user = '') {
  const text = String(reason ?? '').trim().slice(0, 200);
  try {
    return await db.tx(async (c) => {
      if (!(await cancelSupported(c))) {
        throw new UserError('Отмена операций заработает после обновления настольного приложения: запустите свежую версию один раз с этой базой.');
      }
      const plan = await planCancel(c, ids, { lock: true });
      if (plan.problems.length) throw new CancelRefused(plan.problems);
      const comment = text ? `Отмена: ${text}` : 'Отмена';
      const doc = newDoc('cancel', comment, user);
      const sums = new Map();
      for (const r of plan.rows) {
        const k = `${r.product_id}:${r.warehouse_id}`;
        sums.set(k, { pid: r.product_id, wid: r.warehouse_id, sum: (sums.get(k)?.sum ?? 0) - r.delta });
      }
      for (const e of [...sums.values()].sort((a, b) => a.pid - b.pid || a.wid - b.wid)) {
        await c.query(
          `INSERT INTO stock (product_id, warehouse_id, quantity) VALUES ($1, $2, $3)
           ON CONFLICT (product_id, warehouse_id) DO UPDATE SET quantity = stock.quantity + EXCLUDED.quantity`,
          [e.pid, e.wid, e.sum]);
      }
      for (const r of plan.rows) await addMovement(c, r.product_id, r.warehouse_id, r.related_warehouse_id, 'cancel', -r.delta, comment, doc, r.id);
      return { rows: plan.rows, doc: doc.key };
    });
  } catch (e) {
    // Две отмены одной записи одновременно: вторую останавливает уникальный индекс reverts_id.
    if (e.code === '23505') throw new CancelRefused(['Эту запись уже отменили (возможно, с другого устройства). Обновите журнал']);
    throw e;
  }
}

module.exports = {
  UserError, ValidationError, CancelRefused, qty, receive, takeOut, transfer, inventory, setLevels, newDoc, docRolledBack,
  cancelSupported, docsSupported, resetFeatureCache, documentIds, planCancel, cancel, TYPE_LABELS, MAX_QTY,
};

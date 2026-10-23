'use strict';
// Документы склада (таблица stock_documents): список, состав, состояние. То же, что раздел «Документы» в
// настольном приложении (src/documents.cpp), чтобы номера и тексты совпадали с обеих сторон.
const { likeEscape } = require('./util');

const KINDS = [
  ['receipt', 'Приходы'],
  ['writeoff', 'Списания'],
  ['transfer', 'Перемещения'],
  ['inventory', 'Инвентаризации'],
  ['cancel', 'Отмены'],
];
const KIND_TITLES = {
  receipt: 'Приход', writeoff: 'Списание', transfer: 'Перемещение', inventory: 'Инвентаризация', cancel: 'Отмена',
};
const kindTitle = (kind) => KIND_TITLES[kind] || 'Операция';
const title = (d) => `${kindTitle(d.kind)} №${d.id}`;
const isCancelled = (d) => d.kind !== 'cancel' && d.lines > 0 && d.reverted >= d.lines;

function statusText(d) {
  if (d.kind === 'cancel') return d.reverts ? `отмена документа №${d.reverts}` : 'отмена';
  if (d.lines === 0) return 'пустой';
  if (d.reverted >= d.lines) return 'отменён';
  if (d.reverted > 0) return `отменён частично (${d.reverted} из ${d.lines})`;
  return 'действует';
}

const INFO_SELECT = `
  SELECT d.id, d.doc_key, d.kind, d.created_at, d.username, d.comment,
         COUNT(m.id)::int AS lines, COUNT(DISTINCT m.product_id)::int AS positions,
         COALESCE(SUM(ABS(m.delta)), 0)::bigint AS abs_units, COUNT(rv.id)::int AS reverted,
         COUNT(DISTINCT m.warehouse_id)::int AS wh_count, COALESCE(MIN(w.name), '') AS wh_first,
         COALESCE(MAX(CASE WHEN m.type = 'transfer_out' THEN w.name END), '') AS wh_from,
         COALESCE(MAX(CASE WHEN m.type = 'transfer_in' THEN w.name END), '') AS wh_to,
         CASE WHEN d.kind = 'cancel' THEN COALESCE((SELECT string_agg(DISTINCT od.id::text, ', ')
              FROM stock_movements c JOIN stock_movements o ON o.id = c.reverts_id
              JOIN stock_documents od ON od.doc_key = o.doc_id WHERE c.doc_id = d.doc_key), '') ELSE '' END AS reverts`;
const INFO_JOINS = `
    LEFT JOIN stock_movements m ON m.doc_id = d.doc_key
    LEFT JOIN warehouses w ON w.id = m.warehouse_id
    LEFT JOIN stock_movements rv ON rv.reverts_id = m.id`;
const INFO_GROUP = 'GROUP BY d.id, d.doc_key, d.kind, d.created_at, d.username, d.comment';

function warehousesText(r) {
  if (r.kind === 'transfer' && r.wh_from && r.wh_to) return `${r.wh_from} → ${r.wh_to}`;
  if (r.wh_count <= 1) return r.wh_first;
  return `несколько складов (${r.wh_count})`;
}

function toInfo(r) {
  const abs = Number(r.abs_units);
  return {
    id: r.id, key: r.doc_key, kind: r.kind, createdAt: r.created_at, username: r.username, comment: r.comment,
    lines: r.lines, positions: r.positions,
    units: r.kind === 'transfer' ? Math.floor(abs / 2) : abs, // перемещение - расход и приход одних и тех же штук
    reverted: r.reverted, warehouses: warehousesText(r), reverts: r.reverts,
  };
}

const likeOf = (term) => `%${likeEscape(term)}%`;

// -> { docs: Info[], more: boolean }
async function list(q, { kind = '', search = '', page = 1, pageSize = 50 } = {}) {
  const params = [];
  let where = 'TRUE';
  if (KIND_TITLES[kind]) { params.push(kind); where += ` AND kind = $${params.length}`; }
  const term = String(search ?? '').trim();
  if (term) {
    params.push(term); const idN = params.length;
    params.push(likeOf(term)); const likeN = params.length;
    where += ` AND (id::text = $${idN} OR comment ILIKE $${likeN} OR username ILIKE $${likeN} OR EXISTS (
      SELECT 1 FROM stock_movements mm JOIN products pp ON pp.id = mm.product_id
       WHERE mm.doc_id = stock_documents.doc_key AND (pp.sku ILIKE $${likeN} OR pp.name ILIKE $${likeN})))`;
  }
  // Сначала отбираются нужные документы (с ограничением), и только к ним подсчитываются записи.
  const r = await q.query(
    `${INFO_SELECT}
       FROM (SELECT * FROM stock_documents WHERE ${where} ORDER BY id DESC LIMIT ${pageSize + 1} OFFSET ${(Math.max(1, page) - 1) * pageSize}) d
       ${INFO_JOINS} ${INFO_GROUP} ORDER BY d.id DESC`, params);
  const docs = r.rows.map(toInfo);
  return { docs: docs.slice(0, pageSize), more: docs.length > pageSize };
}

async function get(q, id) {
  const r = await q.query(`${INFO_SELECT} FROM (SELECT * FROM stock_documents WHERE id = $1) d ${INFO_JOINS} ${INFO_GROUP}`, [id]);
  return r.rows[0] ? toInfo(r.rows[0]) : null;
}

async function lines(q, docKey) {
  const r = await q.query(
    `SELECT m.id, m.product_id, p.sku, p.name, w.name AS warehouse, m.type, m.delta, COALESCE(m.comment, '') AS comment,
            (SELECT r.id FROM stock_movements r WHERE r.reverts_id = m.id) AS reverted_by, m.reverts_id
       FROM stock_movements m JOIN products p ON p.id = m.product_id JOIN warehouses w ON w.id = m.warehouse_id
      WHERE m.doc_id = $1 ORDER BY m.id`, [docKey]);
  return r.rows;
}

async function idByKey(q, key) {
  const r = await q.query('SELECT id FROM stock_documents WHERE doc_key = $1', [key]);
  return r.rows[0]?.id ?? 0;
}

// Записи документа, которые ещё можно отменить (отмены и уже отменённые не считаются).
async function cancellableIds(q, docKey) {
  const r = await q.query(
    `SELECT m.id FROM stock_movements m WHERE m.doc_id = $1 AND m.type <> 'cancel'
        AND NOT EXISTS (SELECT 1 FROM stock_movements r WHERE r.reverts_id = m.id) ORDER BY m.id`, [docKey]);
  return r.rows.map((x) => x.id);
}

// Номер последнего документа, в котором ещё есть что отменять (0 - такого нет).
async function lastCancellable(q) {
  const r = await q.query(
    `SELECT d.id FROM stock_documents d WHERE d.kind <> 'cancel' AND EXISTS (
       SELECT 1 FROM stock_movements m WHERE m.doc_id = d.doc_key AND m.type <> 'cancel'
          AND NOT EXISTS (SELECT 1 FROM stock_movements r WHERE r.reverts_id = m.id)) ORDER BY d.id DESC LIMIT 1`);
  return r.rows[0]?.id ?? 0;
}

module.exports = { KINDS, kindTitle, title, isCancelled, statusText, list, get, lines, idByKey, cancellableIds, lastCancellable };

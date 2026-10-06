'use strict';
const { html } = require('../html');
const { csrfField, pager } = require('../layout');
const bc = require('../barcode');
const ops = require('../stock');
const { searchSql } = require('./products');
const { parseId, fmtDate, str, csvCell } = require('../util');

const PAGE = 100;

module.exports = (app, { db, send, requireLogin }) => {
  const warehouses = async () => (await db.query('SELECT id, name FROM warehouses ORDER BY name')).rows;

  async function stockRows(q, wid, limit, offset) {
    const s = searchSql(q, 2);
    const { rows } = await db.query(
      `SELECT p.id AS pid, p.sku, p.name, s.warehouse_id AS wid, w.name AS wname, l.code AS location, s.quantity
         FROM stock s JOIN products p ON p.id = s.product_id JOIN warehouses w ON w.id = s.warehouse_id
         LEFT JOIN locations l ON l.id = s.location_id
        WHERE ($1::int IS NULL OR s.warehouse_id = $1) AND ${s.sql}
        ORDER BY p.sku, w.name LIMIT ${limit} OFFSET ${offset}`, [wid, ...s.params]);
    return rows;
  }

  app.get('/stock', requireLogin, async (req, res) => {
    const q = str(req.query.q, 100);
    const wid = parseId(req.query.w);
    const pg = Math.max(1, parseInt(req.query.page, 10) || 1);
    const [rows, whs] = await Promise.all([stockRows(q, wid, PAGE + 1, (pg - 1) * PAGE), warehouses()]);
    const params = { q, w: wid || '' };
    send(req, res, {
      title: 'Остатки', active: 'stock',
      body: html`
      <form method="get" action="/stock" class="toolbar">
        <input type="search" name="q" value="${q}" placeholder="Артикул, название или штрихкод" ${q ? '' : html`autofocus`}>
        <select name="w"><option value="">Все склады</option>
          ${whs.map((w) => html`<option value="${w.id}" ${w.id === wid ? html`selected` : ''}>${w.name}</option>`)}</select>
        <button>Показать</button>
        <a class="button primary" href="/operation">Приход / списание / перемещение</a>
        <a class="button" href="/stock.csv?${new URLSearchParams(params).toString()}">Скачать для Excel</a>
      </form>
      <div class="table-wrap"><table>
        <thead><tr><th>Артикул</th><th>Название</th><th>Склад</th><th>Место</th><th class="num">Остаток</th><th></th></tr></thead>
        <tbody>${rows.slice(0, PAGE).map((r) => html`<tr>
          <td><a href="/products/${r.pid}">${r.sku}</a></td><td>${r.name}</td><td>${r.wname}</td><td>${r.location}</td>
          <td class="num"><b>${r.quantity}</b></td>
          <td><a href="/operation?product=${encodeURIComponent(r.sku)}&warehouse=${r.wid}">операция</a></td></tr>`)}
          ${rows.length ? '' : html`<tr><td colspan="6" class="muted">Остатков не найдено</td></tr>`}</tbody>
      </table></div>
      ${pager('/stock', params, pg, rows.length > PAGE)}`,
    });
  });

  app.get('/stock.csv', requireLogin, async (req, res) => {
    const rows = await stockRows(str(req.query.q, 100), parseId(req.query.w), 50000, 0);
    const lines = [['Артикул', 'Название', 'Склад', 'Место хранения', 'Остаток'].join(';')];
    for (const r of rows) lines.push([r.sku, r.name, r.wname, r.location ?? '', r.quantity].map(csvCell).join(';'));
    res.set('Content-Type', 'text/csv; charset=utf-8');
    res.set('Content-Disposition', 'attachment; filename="stock.csv"');
    res.send('﻿' + lines.join('\r\n') + '\r\n');
  });

  // ---- Операция с остатком
  const KINDS = [['receipt', 'Приход'], ['writeoff', 'Списание'], ['transfer', 'Перемещение между складами'], ['inventory', 'Инвентаризация (задать фактическое количество)']];

  async function operationView(req, res, { error = '', f = {} } = {}, status = 200) {
    const whs = await warehouses();
    const wsel = (name, sel) => html`<select name="${name}">${whs.map((w) => html`<option value="${w.id}" ${String(w.id) === String(sel) ? html`selected` : ''}>${w.name}</option>`)}</select>`;
    send(req, res, {
      title: 'Операция с остатком', active: 'stock',
      body: html`
      <form method="post" action="/operation" class="card narrow">
        ${error ? html`<div class="flash err">${error}</div>` : ''}
        ${csrfField(req.ctx)}
        <label class="field"><span>Что сделать</span><select name="kind" id="kind">
          ${KINDS.map(([k, t]) => html`<option value="${k}" ${k === (f.kind || 'receipt') ? html`selected` : ''}>${t}</option>`)}</select></label>
        <label class="field"><span>Артикул или штрихкод</span>
          <input name="product" value="${f.product ?? ''}" required maxlength="100" autocomplete="off" ${f.product ? '' : html`autofocus`}></label>
        <label class="field"><span id="wlabel">Склад</span>${wsel('warehouse', f.warehouse)}</label>
        <label class="field" data-only="transfer"><span>На склад</span>${wsel('dest', f.dest)}</label>
        <label class="field"><span id="qlabel">Количество, шт</span>
          <input name="quantity" value="${f.quantity ?? ''}" required inputmode="numeric" maxlength="10" autocomplete="off" ${f.product ? html`autofocus` : ''}></label>
        <label class="field"><span>Комментарий</span><input name="comment" value="${f.comment ?? ''}" maxlength="300"></label>
        <button class="primary">Выполнить</button>
        ${whs.length ? '' : html`<p class="flash err">Сначала <a href="/warehouses">создайте склад</a>.</p>`}
      </form>`,
    }, status);
  }

  app.get('/operation', requireLogin, (req, res) =>
    operationView(req, res, { f: { product: str(req.query.product, 100), warehouse: str(req.query.warehouse, 10), kind: str(req.query.kind, 20) } }));

  app.post('/operation', requireLogin, async (req, res) => {
    const f = {
      kind: str(req.body.kind, 20), product: str(req.body.product, 100), warehouse: str(req.body.warehouse, 10),
      dest: str(req.body.dest, 10), quantity: str(req.body.quantity, 12), comment: str(req.body.comment, 300),
    };
    const fail = (msg, status = 400) => operationView(req, res, { error: msg, f }, status);
    try {
      if (!KINDS.some(([k]) => k === f.kind)) return fail('Выберите действие');
      const wid = parseId(f.warehouse);
      if (!wid) return fail('Выберите склад');
      const n = ops.qty(f.quantity, { allowZero: f.kind === 'inventory' });
      // Товар - по точному артикулу, иначе по штрихкоду (в т.ч. из сканера).
      const found = (await db.query(
        `SELECT id, sku, name FROM products WHERE sku = $1
         UNION ALL SELECT p.id, p.sku, p.name FROM product_barcodes b JOIN products p ON p.id = b.product_id WHERE b.barcode = $2
         LIMIT 1`, [f.product, bc.normalize(f.product)])).rows[0];
      if (!found) return fail(`Товар «${f.product}» не найден: нет такого артикула или штрихкода`, 404);
      const dest = f.kind === 'transfer' ? parseId(f.dest) : null;
      if (f.kind === 'transfer' && !dest) return fail('Выберите склад назначения');

      const names = new Map((await warehouses()).map((w) => [w.id, w.name]));
      if (!names.has(wid) || (dest && !names.has(dest))) return fail('Склад не найден');

      await db.tx(async (c) => {
        if (f.kind === 'receipt') await ops.receive(c, found.id, wid, n, f.comment);
        else if (f.kind === 'writeoff') await ops.takeOut(c, found.id, wid, n, f.comment);
        else if (f.kind === 'transfer') await ops.transfer(c, found.id, wid, dest, n, f.comment);
        else await ops.inventory(c, found.id, wid, n, f.comment);
      });
      const now = (await db.query('SELECT quantity FROM stock WHERE product_id = $1 AND warehouse_id = $2', [found.id, wid])).rows[0];
      const verb = { receipt: 'Приход', writeoff: 'Списание', transfer: 'Перемещено', inventory: 'Инвентаризация' }[f.kind];
      res.flash('ok', `${verb}: ${found.sku}, ${n} шт. Теперь на складе «${names.get(wid)}»: ${now?.quantity ?? 0} шт`);
      res.redirect(`/stock?q=${encodeURIComponent(found.sku)}`);
    } catch (e) {
      if (e instanceof ops.ValidationError) return fail(e.message, 400);
      if (e instanceof ops.UserError) return fail(e.message, 409);
      if (e.code === '22003') return fail('Слишком большое количество', 400);
      throw e;
    }
  });

  // ---- Журнал движений
  app.get('/movements', requireLogin, async (req, res) => {
    const q = str(req.query.q, 100);
    const pg = Math.max(1, parseInt(req.query.page, 10) || 1);
    const s = searchSql(q, 1);
    const { rows } = await db.query(
      `SELECT m.created_at, p.id AS pid, p.sku, w.name AS wname, m.type, m.delta, m.comment
         FROM stock_movements m JOIN products p ON p.id = m.product_id JOIN warehouses w ON w.id = m.warehouse_id
        WHERE ${s.sql} ORDER BY m.id DESC LIMIT ${PAGE + 1} OFFSET ${(pg - 1) * PAGE}`, s.params);
    send(req, res, {
      title: 'Движения', active: 'movements',
      body: html`
      <form method="get" action="/movements" class="toolbar">
        <input type="search" name="q" value="${q}" placeholder="Артикул, название или штрихкод"><button>Найти</button></form>
      <div class="table-wrap"><table>
        <thead><tr><th>Дата</th><th>Артикул</th><th>Склад</th><th>Тип</th><th class="num">Изменение</th><th>Комментарий</th></tr></thead>
        <tbody>${rows.slice(0, PAGE).map((m) => html`<tr><td>${fmtDate(m.created_at)}</td><td><a href="/products/${m.pid}">${m.sku}</a></td>
          <td>${m.wname}</td><td>${ops.TYPE_LABELS[m.type] || m.type}</td>
          <td class="num ${m.delta < 0 ? 'neg' : 'pos'}">${m.delta > 0 ? '+' : ''}${m.delta}</td><td>${m.comment}</td></tr>`)}
          ${rows.length ? '' : html`<tr><td colspan="6" class="muted">Движений не найдено</td></tr>`}</tbody>
      </table></div>
      ${pager('/movements', { q }, pg, rows.length > PAGE)}`,
    });
  });
};

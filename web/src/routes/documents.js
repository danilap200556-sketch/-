'use strict';
const { html } = require('../html');
const xlsx = require('../xlsx');
const { csvCell, parseId, fmtDate, str } = require('../util');
const { pager } = require('../layout');
const ops = require('../stock');
const docs = require('../documents');

// Раздел «Документы»: каждая операция склада - отдельный нумерованный документ (одна или много строк).
// Список, состав, выгрузка. Отмена целиком - в routes/cancel.js (там же страница «что изменится»).
module.exports = (app, { db, send, requireLogin }) => {
  const PAGE = 50;

  const notReady = (req, res) => {
    res.flash('err', 'Документы заработают после обновления настольного приложения: запустите свежую версию один раз с этой базой.');
    return res.redirect('/movements');
  };
  const ready = async () => (await ops.docsSupported(db)) && (await ops.cancelSupported(db));

  const statusCell = (d) => {
    const cls = docs.isCancelled(d) ? 'muted' : '';
    return html`<span class="${cls}">${docs.statusText(d)}</span>`;
  };

  app.get('/documents', requireLogin, async (req, res) => {
    if (!(await ready())) return notReady(req, res);
    const kind = str(req.query.kind, 20);
    const q = str(req.query.q, 100);
    const pg = Math.max(1, parseInt(req.query.page, 10) || 1);
    const { docs: rows, more } = await docs.list(db, { kind, search: q, page: pg, pageSize: PAGE });
    const editor = !req.ctx.user.read_only;
    const last = editor ? await docs.lastCancellable(db) : 0;
    send(req, res, {
      title: 'Документы', active: 'documents',
      body: html`
      <p class="muted">Каждый приход, списание, перемещение и инвентаризация - отдельный документ: внутри одна позиция или много.
        Ошиблись - откройте документ и отмените его целиком: остатки вернутся, в журнале появится документ «Отмена».</p>
      <form method="get" action="/documents" class="toolbar">
        <select name="kind" aria-label="Вид документа">
          <option value="">Все документы</option>
          ${docs.KINDS.map(([k, label]) => html`<option value="${k}" ${k === kind ? 'selected' : ''}>${label}</option>`)}
        </select>
        <input type="search" name="q" value="${q}" placeholder="Номер, комментарий, автор, артикул или название">
        <button>Найти</button>
        ${last ? html`<a class="button" href="/documents/last/cancel" title="Последний документ, в котором ещё есть что отменять">Отменить последний документ (№${last})</a>` : ''}
      </form>
      <div class="table-wrap"><table>
        <thead><tr><th>№</th><th>Дата</th><th>Вид</th><th>Склад</th><th class="num">Позиций</th><th class="num">Штук</th><th>Автор</th><th>Комментарий</th><th>Состояние</th></tr></thead>
        <tbody>${rows.map((d) => html`<tr class="${docs.isCancelled(d) ? 'cancelled' : ''}">
          <td><a href="/documents/${d.id}">${d.id}</a></td><td>${fmtDate(d.createdAt)}</td><td>${docs.kindTitle(d.kind)}</td><td>${d.warehouses}</td>
          <td class="num">${d.positions}</td><td class="num">${d.units}</td><td>${d.username}</td><td>${d.comment}</td><td>${statusCell(d)}</td></tr>`)}
          ${rows.length ? '' : html`<tr><td colspan="9" class="muted">Документов не найдено</td></tr>`}</tbody>
      </table></div>
      ${pager('/documents', { kind, q }, pg, more)}`,
    });
  });

  // Выгрузка состава документа: общая таблица для Excel и CSV (как «Выгрузить документ» в приложении).
  const sheetOf = (d, lines) => {
    const head = [['Документ', docs.title(d)], ['Дата', fmtDate(d.createdAt)], ['Автор', d.username], ['Комментарий', d.comment],
      ['Состояние', docs.statusText(d)], []].map((r) => [r[0] ?? '', r[1] ?? '', '', '', '', '']);
    const rows = [...head, ['Артикул', 'Название', 'Склад', 'Изменение, шт', 'Запись журнала', 'Отмена'],
      ...lines.map((l) => [l.sku, l.name, l.warehouse, String(l.delta), String(l.id),
        l.reverted_by ? `отменена записью №${l.reverted_by}` : (l.reverts_id ? `отменяет запись №${l.reverts_id}` : '')])];
    return { name: `Документ ${d.id}`, rows, numericColumns: [3], columnWidths: [18, 40, 20, 14, 14, 26] };
  };

  app.get('/documents/:id', requireLogin, async (req, res) => {
    if (!(await ready())) return notReady(req, res);
    const id = parseId(req.params.id);
    const d = id ? await docs.get(db, id) : null;
    if (!d) { res.flash('err', 'Документ не найден'); return res.redirect('/documents'); }
    const lines = await docs.lines(db, d.key);
    const editor = !req.ctx.user.read_only;
    const open = editor && d.kind !== 'cancel' ? (await docs.cancellableIds(db, d.key)).length : 0;
    send(req, res, {
      title: docs.title(d), active: 'documents',
      body: html`
      <div class="card">
        <p><b>${fmtDate(d.createdAt)}</b>${d.username ? html` · ${d.username}` : ''} · позиций: ${d.positions}, штук: ${d.units}${d.warehouses ? html` · ${d.warehouses}` : ''}</p>
        ${d.comment ? html`<p>Комментарий: ${d.comment}</p>` : ''}
        <p>Состояние: <b>${docs.statusText(d)}</b></p>
        <div class="toolbar">
          ${open ? html`<a class="button" href="/documents/${d.id}/cancel">Отменить документ целиком${open < d.lines ? ` (осталось ${open} из ${d.lines})` : ''}</a>` : ''}
          <a class="button" href="/documents/${d.id}/export.xlsx">Выгрузить в Excel</a>
          <a class="button" href="/documents/${d.id}/export.csv">Выгрузить в CSV</a>
          <a class="button" href="/documents">Все документы</a>
        </div>
      </div>
      <div class="table-wrap"><table>
        <thead><tr><th>Артикул</th><th>Название</th><th>Склад</th><th class="num">Изменение, шт</th><th>Запись журнала</th><th>Отмена</th></tr></thead>
        <tbody>${lines.map((l) => html`<tr class="${l.reverted_by ? 'cancelled' : ''}">
          <td><a href="/products/${l.product_id}">${l.sku}</a></td><td>${l.name}</td><td>${l.warehouse}</td>
          <td class="num ${l.delta < 0 ? 'neg' : 'pos'}">${l.delta > 0 ? '+' : ''}${l.delta}</td><td class="muted">${l.id}</td>
          <td class="muted">${l.reverted_by ? `отменена записью №${l.reverted_by}` : (l.reverts_id ? `отменяет запись №${l.reverts_id}` : '')}</td></tr>`)}
          ${lines.length ? '' : html`<tr><td colspan="6" class="muted">В документе нет записей</td></tr>`}</tbody>
      </table></div>`,
    });
  });

  const stamp = (d) => `${d.kind}_${d.id}`;
  const exporter = (format) => async (req, res) => {
    if (!(await ready())) return notReady(req, res);
    const id = parseId(req.params.id);
    const d = id ? await docs.get(db, id) : null;
    if (!d) { res.flash('err', 'Документ не найден'); return res.redirect('/documents'); }
    const sheet = sheetOf(d, await docs.lines(db, d.key));
    const utf = encodeURIComponent(`${docs.title(d)}.${format}`);
    if (format === 'xlsx') {
      res.set({
        'Content-Type': 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
        'Content-Disposition': `attachment; filename="document_${stamp(d)}.xlsx"; filename*=UTF-8''${utf}`,
      });
      return res.send(xlsx.write([sheet]));
    }
    res.set({
      'Content-Type': 'text/csv; charset=utf-8',
      'Content-Disposition': `attachment; filename="document_${stamp(d)}.csv"; filename*=UTF-8''${utf}`,
    });
    return res.send('﻿' + sheet.rows.map((r) => r.map(csvCell).join(';')).join('\r\n') + '\r\n');
  };
  app.get('/documents/:id/export.xlsx', requireLogin, exporter('xlsx'));
  app.get('/documents/:id/export.csv', requireLogin, exporter('csv'));
};

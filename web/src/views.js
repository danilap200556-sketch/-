'use strict';
const { html } = require('./html');
const { TYPE_LABELS } = require('./stock');
const { fmtDate } = require('./util');

// Миниатюра товара для списков; клик открывает крупный просмотр (public/app.js).
function thumbImg(productId, coverId, big = false) {
  const cls = big ? 'thumb big' : 'thumb';
  if (!coverId) return html`<span class="${cls} none">нет фото</span>`;
  return html`<img class="${cls}" src="/photos/${coverId}/thumb" data-product="${productId}" alt="" loading="lazy">`;
}

// Ссылка "быстрая правка" - после сохранения человек вернётся на ту же страницу списка.
function editLink(ctx, productId, back) {
  if (ctx.user.read_only) return '';
  return html`<a class="editlink" href="/products/${productId}/quick?back=${encodeURIComponent(back)}" title="Быстрая правка карточки">✎</a>`;
}

const COVER_SQL = '(SELECT id FROM product_photos ph WHERE ph.product_id = p.id ORDER BY ph.position, ph.id LIMIT 1)';

// Запрос журнала: id, created_at, pid, sku, wname, type, delta, comment (+ doc_id, reverts_id, reverted_by, doc_size,
// если сайт уже умеет отмену). Одна и та же колонка «Статус» нужна и в журнале, и в карточке товара.
const MOVEMENT_STATUS_SQL = `m.doc_id, m.reverts_id,
  (SELECT r.id FROM stock_movements r WHERE r.reverts_id = m.id) AS reverted_by,
  CASE WHEN m.doc_id IS NULL THEN 1 ELSE (SELECT COUNT(*)::int FROM stock_movements d WHERE d.doc_id = m.doc_id AND d.type <> 'cancel') END AS doc_size`;

// Номер документа записи (если в базе есть stock_documents): ссылка из журнала на раздел «Документы».
const DOC_NO_SQL = '(SELECT d.id FROM stock_documents d WHERE d.doc_key = m.doc_id) AS doc_no';

// Таблица журнала движений. cancelOn - сайт уже умеет отмену (в базе есть нужные столбцы);
// редактор видит «Отменить» (и «весь документ», если операция из нескольких записей), «только просмотр» - статус.
function movementTable(ctx, rows, { back = '/movements', cancelOn = false, withSku = true, docsOn = false } = {}) {
  const editor = !ctx.user.read_only;
  const action = (m) => {
    if (!cancelOn) return '';
    if (m.reverted_by) return html`<span class="muted">отменена записью №${m.reverted_by}</span>`;
    if (m.type === 'cancel') return html`<span class="muted">отменяет запись №${m.reverts_id}</span>`;
    if (!editor) return '';
    const q = (extra) => `/movements/cancel?${new URLSearchParams({ id: m.id, ...extra, back }).toString()}`;
    const transfer = m.type === 'transfer_out' || m.type === 'transfer_in';
    return html`<a class="button small" href="${q({})}" title="${transfer ? 'Перемещение отменяется целиком: расход и приход вместе' : 'Вернуть остаток как было до этой записи'}">Отменить</a>
      ${m.doc_size > 1 && !transfer ? html`<a class="button small" href="${q({ doc: '1' })}" title="Одна операция из ${m.doc_size} записей (пакет, импорт, правка карточки)">весь документ (${m.doc_size})</a>` : ''}`;
  };
  const cols = 6 + (withSku ? 1 : 0) + (cancelOn ? 1 : 0);
  return html`<div class="table-wrap"><table>
    <thead><tr><th>№</th><th>Дата</th>${withSku ? html`<th>Артикул</th>` : ''}<th>Склад</th><th>Тип</th><th class="num">Изменение</th><th>Комментарий</th>${cancelOn ? html`<th>Отмена</th>` : ''}</tr></thead>
    <tbody>${rows.map((m) => html`<tr class="${m.reverted_by ? 'cancelled' : ''}"><td class="muted">${m.id}</td><td>${fmtDate(m.created_at)}</td>
      ${withSku ? html`<td><a href="/products/${m.pid}">${m.sku}</a> ${editLink(ctx, m.pid, back)}</td>` : ''}
      <td>${m.wname}</td><td>${TYPE_LABELS[m.type] || m.type}</td>
      <td class="num ${m.delta < 0 ? 'neg' : 'pos'}">${m.delta > 0 ? '+' : ''}${m.delta}</td><td>${m.comment}${docsOn && m.doc_no ? html` <a class="muted" href="/documents/${m.doc_no}" title="Открыть документ">документ №${m.doc_no}</a>` : ''}</td>${cancelOn ? html`<td class="actions">${action(m)}</td>` : ''}</tr>`)}
      ${rows.length ? '' : html`<tr><td colspan="${cols}" class="muted">Движений не найдено</td></tr>`}</tbody>
  </table></div>`;
}

module.exports = { thumbImg, editLink, COVER_SQL, MOVEMENT_STATUS_SQL, DOC_NO_SQL, movementTable };

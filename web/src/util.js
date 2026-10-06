'use strict';
const { html } = require('./html');

const INT_MAX = 2147483647;

// id из адреса: только положительное целое в пределах INTEGER, иначе null.
function parseId(v) {
  const s = String(v ?? '');
  if (!/^\d{1,10}$/.test(s)) return null;
  const n = Number(s);
  return n >= 1 && n <= INT_MAX ? n : null;
}

// Для ILIKE: спецсимволы пользовательского ввода должны искаться буквально.
const likeEscape = (s) => s.replace(/[\\%_]/g, (c) => '\\' + c);

const tz = process.env.DISPLAY_TZ || 'Europe/Moscow';
const dateFmt = new Intl.DateTimeFormat('ru-RU', {
  timeZone: tz, day: '2-digit', month: '2-digit', year: 'numeric', hour: '2-digit', minute: '2-digit',
});
const fmtDate = (d) => (d ? dateFmt.format(new Date(d)) : '');

const moneyFmt = new Intl.NumberFormat('ru-RU', { minimumFractionDigits: 2, maximumFractionDigits: 2 });
const fmtMoney = (n) => moneyFmt.format(Number(n) || 0);

// Цена: до 2 знаков, запятая или точка.
function parsePrice(v) {
  const s = String(v ?? '').trim().replace(',', '.');
  if (s === '') return 0;
  if (!/^\d{1,8}(\.\d{1,2})?$/.test(s)) return null;
  const n = Number(s);
  return n <= 10_000_000 ? n : null;
}

const str = (v, max = 500) => String(v ?? '').trim().slice(0, max);

// CSV для Excel: защита от формул (=, +, -, @) в ячейках.
function csvCell(v) {
  let s = String(v ?? '');
  if (/^[=+\-@\t\r]/.test(s)) s = "'" + s;
  return /[";\n\r]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
}

const field = (label, name, value, extra = {}) => html`
  <label class="field"><span>${label}</span>
    <input name="${name}" value="${value ?? ''}" ${extra.required ? html`required` : ''} maxlength="${extra.max || 500}"
      ${extra.type ? html`type="${extra.type}"` : ''} ${extra.autofocus ? html`autofocus` : ''}
      ${extra.inputmode ? html`inputmode="${extra.inputmode}"` : ''} ${extra.autocomplete ? html`autocomplete="${extra.autocomplete}"` : ''}>
  </label>`;

module.exports = { parseId, likeEscape, fmtDate, fmtMoney, parsePrice, str, csvCell, field, INT_MAX };

'use strict';
const { html } = require('./html');

function nav(ctx, active) {
  const items = [
    ['/stock', 'Остатки', 'stock'],
    ['/products', 'Товары', 'products'],
    ['/warehouses', 'Склады', 'warehouses'],
    ['/movements', 'Движения', 'movements'],
  ];
  items.push(['/import', ctx.user.read_only ? 'Экспорт' : 'Импорт / экспорт', 'import']);
  if (!ctx.user.read_only) items.push(['/orders', 'Заказы Маркета', 'orders']);
  if (ctx.user.is_admin) items.push(['/users', 'Пользователи', 'users']);
  return html`<nav>
    ${items.map(([href, label, key]) => html`<a href="${href}" class="${key === active ? 'active' : ''}">${label}</a>`)}
    <span class="spacer"></span>
    <a href="/account" class="${active === 'account' ? 'active' : ''}">${ctx.user.username}${ctx.user.read_only ? ' (просмотр)' : ''}</a>
    <form method="post" action="/logout" class="inline"><input type="hidden" name="_csrf" value="${ctx.csrf}"><button class="link">Выйти</button></form>
  </nav>`;
}

function page(ctx, { title, active, body, scripts = [] }) {
  return html`<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex, nofollow">
<title>${title} · Склад</title>
<link rel="stylesheet" href="/static/style.css">
<script src="/static/app.js" defer></script>
${scripts.map((src) => html`<script src="${src}" defer></script>`)}
</head>
<body>
${ctx.user ? nav(ctx, active) : ''}
<main>
${ctx.flash ? html`<div class="flash ${ctx.flash.kind}" role="status">${ctx.flash.text}</div>` : ''}
<h1>${title}</h1>
${body}
</main>
</body>
</html>`;
}

const csrfField = (ctx) => html`<input type="hidden" name="_csrf" value="${ctx.csrf}">`;

// Пагинация ссылками: ?page=N с сохранением остальных параметров.
function pager(basePath, params, page, hasMore) {
  const link = (p, label) => {
    const q = new URLSearchParams(params);
    q.set('page', String(p));
    return html`<a href="${basePath}?${q.toString()}">${label}</a>`;
  };
  if (page <= 1 && !hasMore) return '';
  return html`<p class="pager">${page > 1 ? link(page - 1, '← Назад') : ''} <span>стр. ${page}</span> ${hasMore ? link(page + 1, 'Дальше →') : ''}</p>`;
}

module.exports = { page, csrfField, pager };

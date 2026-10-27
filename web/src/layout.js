'use strict';
const { html, Raw } = require('./html');

// Значки меню (контурные, 24x24). Внутри SVG нет style-атрибутов: сайт закрыт строгой политикой CSP.
const ICONS = {
  stock: '<path d="M4 20h16M5 20v-8h3.6v8M10.2 20V7h3.6v13M15.4 20V3.5H19V20"/>',
  products: '<path d="M12 3.5l7.5 4.2v8.6L12 20.5l-7.5-4.2V7.7zM4.5 7.7L12 12l7.5-4.3M12 12v8.5"/>',
  warehouses: '<path d="M3.5 10L12 4l8.5 6v10h-17zM9 20v-7h6v7"/>',
  stocktake: '<path d="M7 4.5h10a2 2 0 012 2V20H5V6.5a2 2 0 012-2zM9 4.5h6M8.5 13.5l2.3 2.3 4.7-4.8"/>',
  documents: '<path d="M7 3.5h7.5L19 8v12.5H7zM14 3.5V8.5h5M9.8 12.5h6.4M9.8 16h6.4"/>',
  movements: '<path d="M4 8h13M13.5 4.5L17 8l-3.5 3.5M20 16H7M10.5 12.5L7 16l3.5 3.5"/>',
  import: '<path d="M12 3.5v11M7.8 10.5l4.2 4.2 4.2-4.2M4 14.5V20h16v-5.5"/>',
  orders: '<path d="M7 4.5h10a2 2 0 012 2V20H5V6.5a2 2 0 012-2zM9 4.5h6M8.5 11h7M8.5 14.5h7M8.5 18h4"/>',
  users: '<circle cx="12" cy="8.2" r="3.6"/><path d="M4.8 20c.7-5.5 13.7-5.5 14.4 0"/>',
  account: '<circle cx="12" cy="8.2" r="3.6"/><path d="M4.8 20c.7-5.5 13.7-5.5 14.4 0"/>',
};
const icon = (key) => new Raw(`<svg class="ico" viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[key] || ''}</svg>`);
const LOGO = new Raw('<svg class="logo" viewBox="0 0 36 36" width="36" height="36" aria-hidden="true"><defs><linearGradient id="lg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#6d7bff"/><stop offset="1" stop-color="#9b5cf6"/></linearGradient></defs><rect width="36" height="36" rx="10" fill="url(#lg)"/><g fill="none" stroke="#fff" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 7.8l9.9 5.5v11.4L18 30.2l-9.9-5.5V13.3zM8.1 13.3L18 18.8l9.9-5.5M18 18.8v11.4"/></g></svg>');
const THEME_ICON = new Raw('<svg class="ico" viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M19 14.5C11 19 4 12 9.3 4.2 9 11.5 13 15.5 19 14.5z"/></svg>');


function nav(ctx, active) {
  const items = [
    ['/stock', 'Остатки', 'stock'],
    ['/products', 'Товары', 'products'],
    ['/warehouses', 'Склады', 'warehouses'],
    ['/movements', 'Движения', 'movements'],
    ['/documents', 'Документы', 'documents'],
  ];
  if (!ctx.user.read_only) items.splice(3, 0, ['/stocktake', 'Инвентаризация', 'stocktake']);
  items.push(['/import', ctx.user.read_only ? 'Экспорт' : 'Импорт / экспорт', 'import']);
  if (!ctx.user.read_only) items.push(['/orders', 'Заказы Маркета', 'orders']);
  if (ctx.user.is_admin) items.push(['/users', 'Пользователи', 'users']);
  return html`<nav>
    <a href="/" class="brand" aria-label="Склад">${LOGO}<span><b>Склад</b><small>учёт и Маркет</small></span></a>
    ${items.map(([href, label, key]) => html`<a href="${href}" class="${key === active ? 'active' : ''}">${icon(key)}<span>${label}</span></a>`)}
    <span class="spacer"></span>
    <form method="get" action="/find" class="navsearch" role="search">
      <input type="search" name="q" id="navsearch" placeholder="Штрихкод / артикул" title="Поиск товара. Клавиша / ставит сюда курсор; сканер работает и без клика" aria-label="Найти товар по штрихкоду или артикулу" maxlength="100" autocomplete="off" inputmode="search">
    </form>
    <div class="usercard">
      <a href="/account" class="${active === 'account' ? 'active' : ''}">${icon('account')}<span>${ctx.user.username}${ctx.user.read_only ? ' (просмотр)' : ''}</span></a>
      <button type="button" class="themebtn" id="theme-toggle" title="Светлая / тёмная тема" aria-label="Светлая / тёмная тема">${THEME_ICON}</button>
      <form method="post" action="/logout" class="inline"><input type="hidden" name="_csrf" value="${ctx.csrf}"><button class="link">Выйти</button></form>
    </div>
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
<script src="/static/theme.js"></script>
<script src="/static/app.js" defer></script>
${scripts.map((src) => html`<script src="${src}" defer></script>`)}
</head>
<body class="${ctx.user ? 'app' : 'guest'}">
${ctx.user ? nav(ctx, active) : ''}
<main>
${ctx.user ? '' : html`<div class="brand-big">${LOGO}<b>Склад</b><span>Остатки, заказы и Яндекс Маркет в одном окне</span></div>`}
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

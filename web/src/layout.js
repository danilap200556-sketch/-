'use strict';
const { html, Raw } = require('./html');

// Значок учётной записи (контурный, 24x24). Внутри SVG нет style-атрибутов: сайт закрыт строгой политикой CSP.
const ACCOUNT_ICON = new Raw('<svg class="ico" viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="8.2" r="3.6"/><path d="M4.8 20c.7-5.5 13.7-5.5 14.4 0"/></svg>');
// Логотип - как в приложении: горчичный квадрат с тёмной коробкой.
const LOGO = new Raw('<svg class="logo" viewBox="0 0 36 36" width="36" height="36" aria-hidden="true"><rect width="36" height="36" rx="9" fill="#e5b53a"/><g fill="none" stroke="#0f1f18" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 7.8l9.9 5.5v11.4L18 30.2l-9.9-5.5V13.3zM8.1 13.3L18 18.8l9.9-5.5M18 18.8v11.4"/></g></svg>');
const THEME_ICON = new Raw('<svg class="ico" viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M19 14.5C11 19 4 12 9.3 4.2 9 11.5 13 15.5 19 14.5z"/></svg>');

// Разделы сайта по группам - как в верхнем меню приложения. Группа без доступных разделов не показывается;
// ссылка группы ведёт на её первый раздел.
function sections(ctx) {
  const sklad = [
    ['/stock', 'Остатки', 'stock'],
    ['/products', 'Товары', 'products'],
    ['/warehouses', 'Склады', 'warehouses'],
    ['/movements', 'Движения', 'movements'],
    ['/documents', 'Документы', 'documents'],
  ];
  if (!ctx.user.read_only) sklad.splice(3, 0, ['/stocktake', 'Инвентаризация', 'stocktake']);
  sklad.push(['/import', ctx.user.read_only ? 'Экспорт' : 'Импорт / экспорт', 'import']);
  const market = ctx.user.read_only ? [] : [['/orders', 'Заказы Маркета', 'orders']];
  const system = [];
  if (ctx.user.is_admin) system.push(['/users', 'Пользователи', 'users']);
  system.push(['/account', 'Учётная запись', 'account']);
  return [['Склад', sklad], ['Маркетплейс', market], ['Система', system]].filter(([, items]) => items.length);
}

function nav(ctx, active) {
  const groups = sections(ctx);
  const current = groups.find(([, items]) => items.some(([, , key]) => key === active)) || groups[0];
  return html`<header class="topbar">
    <a href="/" class="brand" aria-label="Склад">${LOGO}<span><b>Склад</b><small>учёт и Маркет</small></span></a>
    <nav class="groups" aria-label="Разделы">
      ${groups.map((g) => html`<a href="${g[1][0][0]}" class="${g === current ? 'active' : ''}"${g === current ? new Raw(' aria-current="true"') : ''}>${g[0]}</a>`)}
    </nav>
    <span class="spacer"></span>
    <form method="get" action="/find" class="navsearch" role="search">
      <input type="search" name="q" id="navsearch" placeholder="Штрихкод / артикул" title="Поиск товара. Клавиша / ставит сюда курсор; сканер работает и без клика" aria-label="Найти товар по штрихкоду или артикулу" maxlength="100" autocomplete="off" inputmode="search">
    </form>
    <div class="usercard">
      <a href="/account" class="${active === 'account' ? 'active' : ''}">${ACCOUNT_ICON}<span>${ctx.user.username}${ctx.user.read_only ? ' (просмотр)' : ''}</span></a>
      <button type="button" class="themebtn" id="theme-toggle" title="Светлая / тёмная тема" aria-label="Светлая / тёмная тема">${THEME_ICON}</button>
      <form method="post" action="/logout" class="inline"><input type="hidden" name="_csrf" value="${ctx.csrf}"><button class="link">Выйти</button></form>
    </div>
  </header>
  <nav class="tabs" aria-label="Страницы раздела «${current[0]}»">
    ${current[1].map(([href, label, key]) => html`<a href="${href}" class="${key === active ? 'active' : ''}"${key === active ? new Raw(' aria-current="page"') : ''}>${label}</a>`)}
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

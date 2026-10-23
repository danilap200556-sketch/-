'use strict';
const { html } = require('../html');
const { csrfField } = require('../layout');
const { findProducts, suggestProducts } = require('../finder');
const bc = require('../barcode');
const { likeEscape, str } = require('../util');
const { thumbImg, COVER_SQL } = require('../views');

// Быстрый поиск из строки в шапке (и "сканер в любом месте страницы"): штрихкод, артикул или часть названия.
//  - найден один товар -> его карточка
//  - несколько -> список товаров с этим поиском
//  - ничего: незнакомый штрихкод можно сразу привязать к нужному товару
module.exports = (app, { db, send, requireLogin }) => {
  // Подсказки при вводе артикула, названия или штрихкода (поля с data-suggest, public/app.js). Строки из одних цифр
  // (8 и больше) - это штрихкод от сканера: для них подсказок нет.
  app.get('/find/suggest', requireLogin, async (req, res) => {
    const q = str(req.query.q, 100);
    res.set('Cache-Control', 'no-store');
    if (q.length < 2 || /^[0-9 ]{8,}$/.test(q)) return res.json({ items: [] });
    res.json({ items: await suggestProducts(db, q, 12) });
  });

  app.get('/find', requireLogin, async (req, res) => {
    const q = str(req.query.q, 100);
    if (!q) return res.redirect('/products');
    const r = await findProducts(db, q);
    if (r.ids.length === 1) return res.redirect(`/products/${r.ids[0]}`);
    if (r.ids.length > 1) return res.redirect(`/products?q=${encodeURIComponent(q)}`);

    const code = bc.normalize(q);
    const canBind = !req.ctx.user.read_only && !bc.validate(code);
    let candidates = [];
    const p = str(req.query.p, 100);
    if (canBind && p) {
      const like = `%${likeEscape(p)}%`;
      candidates = (await db.query(
        `SELECT p.id, p.sku, p.name, ${COVER_SQL} AS cover_id FROM products p
          WHERE p.sku ILIKE $1 OR p.name ILIKE $1 OR p.market_sku ILIKE $1 ORDER BY p.sku LIMIT 30`, [like])).rows;
    }
    send(req, res, {
      title: 'Ничего не найдено', active: 'products',
      body: html`
      <p>По запросу «<b>${q}</b>» товаров нет.</p>
      ${canBind ? html`<div class="card">
        <h2>Привязать штрихкод ${code} к товару</h2>
        <p class="muted">Это новый штрихкод с коробки? Найдите товар по артикулу или названию и нажмите «Привязать» - в следующий раз он найдётся по этому штрихкоду.</p>
        <form method="get" action="/find" class="toolbar">
          <input type="hidden" name="q" value="${q}">
          <input type="search" name="p" value="${p}" placeholder="Артикул или название товара" autofocus>
          <button>Найти товар</button>
        </form>
        ${p ? html`<div class="table-wrap"><table><tbody>${candidates.map((c) => html`<tr>
          <td>${thumbImg(c.id, c.cover_id)}</td><td>${c.sku}</td><td>${c.name}</td>
          <td><form method="post" action="/products/${c.id}/barcodes" class="inline">${csrfField(req.ctx)}
            <input type="hidden" name="barcode" value="${code}"><button class="primary">Привязать</button></form></td></tr>`)}
          ${candidates.length ? '' : html`<tr><td class="muted">Такого товара нет</td></tr>`}</tbody></table></div>` : ''}
      </div>` : ''}
      <p><a class="button" href="/products">К списку товаров</a>${req.ctx.user.read_only ? '' : html` <a class="button" href="/products/new">+ Добавить товар</a>`}</p>`,
    }, 404);
  });
};

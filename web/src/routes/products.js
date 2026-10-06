'use strict';
const { html } = require('../html');
const { csrfField, pager } = require('../layout');
const bc = require('../barcode');
const { TYPE_LABELS } = require('../stock');
const { parseId, likeEscape, fmtDate, fmtMoney, parsePrice, str, field } = require('../util');
const { thumbImg, COVER_SQL } = require('../views');

const PAGE = 50;

// Условие поиска по артикулу, названию, артикулу на Маркете и штрихкоду (в т.ч. от сканера).
function searchSql(q, paramIdx) {
  if (!q) return { sql: 'TRUE', params: [] };
  const like = `%${likeEscape(q)}%`;
  return {
    sql: `(p.sku ILIKE $${paramIdx} OR p.name ILIKE $${paramIdx} OR p.market_sku ILIKE $${paramIdx}
           OR EXISTS (SELECT 1 FROM product_barcodes b WHERE b.product_id = p.id AND b.barcode ILIKE $${paramIdx}))`,
    params: [like],
  };
}

module.exports = (app, { db, send, requireLogin, requireAdmin, requireEditor }) => {
  const readForm = (b) => {
    const d = {
      sku: str(b.sku, 100), name: str(b.name, 300), description: str(b.description, 2000),
      custom_code: str(b.custom_code, 200), market_sku: str(b.market_sku, 200), price: parsePrice(b.price),
      priceRaw: str(b.price, 20),
    };
    let error = '';
    if (!d.sku) error = 'Введите артикул';
    else if (!d.name) error = 'Введите название';
    else if (d.price === null) error = 'Цена - число не больше 10 000 000, до двух знаков после запятой';
    return { d, error };
  };

  // Форма товара. В режиме "только просмотр" поля заблокированы и кнопки сохранения нет.
  const productForm = (ctx, action, d, error, submit) => {
    const ro = ctx.user.read_only;
    const f = (label, name, value, max, extra = {}) => html`
      <label class="field"><span>${label}</span><input name="${name}" value="${value ?? ''}" maxlength="${max}" ${ro ? html`readonly` : ''}
        ${extra.required && !ro ? html`required` : ''} ${extra.inputmode ? html`inputmode="${extra.inputmode}"` : ''}></label>`;
    return html`
    <form method="post" action="${action}" class="card">
      ${error ? html`<div class="flash err">${error}</div>` : ''}
      ${csrfField(ctx)}
      ${f('Артикул', 'sku', d.sku, 100, { required: true })}
      ${f('Название', 'name', d.name, 300, { required: true })}
      ${f('Цена, ₽', 'price', d.priceRaw ?? d.price, 20, { inputmode: 'decimal' })}
      ${f('Артикул на Яндекс Маркете (если отличается от нашего)', 'market_sku', d.market_sku, 200)}
      ${f('Свой код (для QR-этикеток)', 'custom_code', d.custom_code, 200)}
      <label class="field"><span>Описание</span><textarea name="description" rows="3" maxlength="2000" ${ro ? html`readonly` : ''}>${d.description ?? ''}</textarea></label>
      ${ro ? '' : html`<button class="primary">${submit}</button>`}
    </form>`;
  };

  // ---- Список (таблица или карточки)
  app.get('/products', requireLogin, async (req, res) => {
    const q = str(req.query.q, 100);
    const cards = req.query.view === 'cards';
    const pg = Math.max(1, parseInt(req.query.page, 10) || 1);
    const s = searchSql(q, 1);
    const [{ rows }, total] = await Promise.all([
      db.query(
        `SELECT p.id, p.sku, p.name, p.price, ${COVER_SQL} AS cover_id,
                COALESCE((SELECT SUM(quantity) FROM stock WHERE product_id = p.id), 0)::int AS total
           FROM products p WHERE ${s.sql} ORDER BY p.sku LIMIT ${PAGE + 1} OFFSET ${(pg - 1) * PAGE}`, s.params),
      db.query(`SELECT COUNT(*)::int AS n FROM products p WHERE ${s.sql}`, s.params),
    ]);
    const more = rows.length > PAGE;
    const ctx = req.ctx;
    const editor = !ctx.user.read_only;
    const items = rows.slice(0, PAGE);
    const check = (p) => (editor ? html`<input type="checkbox" name="ids" value="${p.id}" class="sel" aria-label="Выбрать">` : '');
    const viewLink = (v, label) => html`<a class="button ${(cards ? 'cards' : 'table') === v ? 'primary' : ''}" href="/products?${new URLSearchParams({ q, view: v }).toString()}">${label}</a>`;
    const list = cards
      ? html`<div class="cards">${items.map((p) => html`<div class="pcard">${check(p)}
          <a href="/products/${p.id}">${thumbImg(p.id, p.cover_id, true)}</a>
          <a href="/products/${p.id}" class="sku">${p.sku}</a><div class="pname">${p.name}</div>
          <div class="muted">${fmtMoney(p.price)} ₽ · на складах: <b>${p.total}</b></div></div>`)}</div>`
      : html`<div class="table-wrap"><table>
          <thead><tr>${editor ? html`<th class="chk"><input type="checkbox" id="sel-all" aria-label="Выбрать все на странице"></th>` : ''}<th></th><th>Артикул</th><th>Название</th><th class="num">Цена</th><th class="num">Всего на складах</th></tr></thead>
          <tbody>${items.map((p) => html`<tr>${editor ? html`<td class="chk">${check(p)}</td>` : ''}<td>${thumbImg(p.id, p.cover_id)}</td>
            <td><a href="/products/${p.id}">${p.sku}</a></td><td>${p.name}</td>
            <td class="num">${fmtMoney(p.price)}</td><td class="num">${p.total}</td></tr>`)}
          ${items.length ? '' : html`<tr><td colspan="6" class="muted">Ничего не найдено</td></tr>`}</tbody>
        </table></div>`;
    send(req, res, {
      title: 'Товары', active: 'products',
      body: html`
      <form method="get" action="/products" class="toolbar">
        <input type="search" name="q" value="${q}" placeholder="Артикул, название или штрихкод (можно сканером)" ${q ? '' : html`autofocus`}>
        <input type="hidden" name="view" value="${cards ? 'cards' : 'table'}">
        <button>Найти</button>
        ${editor ? html`<a class="button primary" href="/products/new">+ Добавить товар</a>` : ''}
        ${viewLink('table', 'Таблица')}${viewLink('cards', 'Карточки')}
      </form>
      <p class="muted">Найдено: ${total.rows[0].n}</p>
      ${editor ? html`<form method="post" action="/bulk" id="selform">
        ${csrfField(ctx)}<input type="hidden" name="q" value="${q}">
        <div class="toolbar sel-bar">
          <span class="muted">Отмеченные товары:</span>
          <button>Массовое редактирование</button>
          <button formaction="/bulk/photos">Добавить фото</button>
          ${ctx.user.is_admin ? html`<button formaction="/bulk/delete" class="danger" data-confirm="Удалить отмеченные товары вместе с остатками, движениями и фото?">Удалить</button>` : ''}
          <label class="check"><input type="checkbox" name="all" value="1"> применить ко <b>всем ${total.rows[0].n}</b> найденным</label>
        </div>
        ${list}
      </form>` : list}
      ${pager('/products', { q, view: cards ? 'cards' : 'table' }, pg, more)}`,
    });
  });

  // ---- Создание
  app.get('/products/new', requireLogin, requireEditor, (req, res) =>
    send(req, res, { title: 'Новый товар', active: 'products', body: productForm(req.ctx, '/products', { price: '' }, '', 'Создать') }));

  app.post('/products', requireLogin, async (req, res) => {
    const { d, error } = readForm(req.body);
    const fail = (msg, status = 400) =>
      send(req, res, { title: 'Новый товар', active: 'products', body: productForm(req.ctx, '/products', d, msg, 'Создать') }, status);
    if (error) return fail(error);
    try {
      const { rows } = await db.query(
        `INSERT INTO products (sku, name, description, photo_path, price, custom_code, market_sku)
         VALUES ($1, $2, $3, '', $4, $5, $6) RETURNING id`,
        [d.sku, d.name, d.description, d.price, d.custom_code, d.market_sku]);
      res.flash('ok', 'Товар создан. Теперь можно добавить фото и штрихкоды');
      res.redirect(`/products/${rows[0].id}`);
    } catch (e) {
      if (e.code === '23505') return fail('Товар с таким артикулом уже есть', 409);
      throw e;
    }
  });

  // ---- Карточка товара
  async function showProduct(req, res, { error = '', barcodeError = '', form = null } = {}, status = 200) {
    const id = parseId(req.params.id);
    if (!id) return res.redirect('/products');
    const { rows } = await db.query('SELECT * FROM products WHERE id = $1', [id]);
    const p = rows[0];
    if (!p) { res.flash('err', 'Товар не найден'); return res.redirect('/products'); }
    const [codes, stockRows, moves, photos] = await Promise.all([
      db.query('SELECT barcode FROM product_barcodes WHERE product_id = $1 ORDER BY created_at, barcode', [id]),
      db.query(`SELECT w.id AS wid, w.name AS wname, COALESCE(s.quantity, 0) AS qty, s.location_id,
                       (SELECT json_agg(json_build_object('id', l.id, 'code', l.code) ORDER BY l.code)
                          FROM locations l WHERE l.warehouse_id = w.id) AS locations
                  FROM warehouses w LEFT JOIN stock s ON s.warehouse_id = w.id AND s.product_id = $1
                 ORDER BY w.name`, [id]),
      db.query(`SELECT m.created_at, w.name AS wname, m.type, m.delta, m.comment
                  FROM stock_movements m JOIN warehouses w ON w.id = m.warehouse_id
                 WHERE m.product_id = $1 ORDER BY m.id DESC LIMIT 15`, [id]),
      db.query('SELECT id FROM product_photos WHERE product_id = $1 ORDER BY position, id', [id]),
    ]);
    const ctx = req.ctx;
    const ro = ctx.user.read_only;
    const d = form || { ...p, price: p.price, priceRaw: String(p.price).replace('.', ',') };
    send(req, res, {
      title: `${p.sku} · ${p.name}`, active: 'products',
      body: html`
      <div class="grid2">
        <div>
          ${productForm(ctx, `/products/${id}`, d, error, 'Сохранить')}
          ${ctx.user.is_admin ? html`<form method="post" action="/products/${id}/delete" data-confirm="Удалить товар ${p.sku} вместе с остатками, штрихкодами и фото?">
            ${csrfField(ctx)}<button class="danger">Удалить товар</button></form>` : ''}
        </div>
        <div>
          <div class="card">
            <h2>Фото <span class="muted">(${photos.rows.length} из 12)</span></h2>
            <div class="gallery">${photos.rows.map((ph, i) => html`<figure>
              <img class="thumb big" src="/photos/${ph.id}/thumb" data-product="${id}" data-photo="${ph.id}" alt="" loading="lazy">
              ${ro ? '' : html`<figcaption>
                ${i === 0 ? html`<span class="muted">главное</span>` : html`<form method="post" action="/products/${id}/photos/${ph.id}/cover" class="inline">${csrfField(ctx)}<button class="link">сделать главным</button></form>`}
                <form method="post" action="/products/${id}/photos/${ph.id}/delete" class="inline" data-confirm="Убрать это фото?">${csrfField(ctx)}<button class="link danger">убрать</button></form>
              </figcaption>`}</figure>`)}
              ${photos.rows.length ? '' : html`<p class="muted">Фото пока нет</p>`}</div>
            ${ro ? '' : html`<form method="post" action="/products/${id}/photos" enctype="multipart/form-data" class="inline-row">
              ${csrfField(ctx)}<input type="file" name="photos" accept="image/jpeg,image/png,image/webp" multiple required>
              <button>Загрузить фото</button></form>
              <p class="muted">jpg, png или webp, до 12 МБ каждое. Первое фото - главное, его видно в списках.</p>`}
          </div>
          <div class="card">
            <h2>Штрихкоды</h2>
            ${ro ? '' : html`<p class="muted">С коробок производителя (EAN-13 и др.) или свои. Один штрихкод - только у одного товара.</p>`}
            ${barcodeError ? html`<div class="flash err">${barcodeError}</div>` : ''}
            <ul class="codes">${codes.rows.map((c) => html`<li><code>${c.barcode}</code>
              ${ro ? '' : html`<form method="post" action="/products/${id}/barcodes/delete" class="inline">${csrfField(ctx)}
                <input type="hidden" name="barcode" value="${c.barcode}"><button class="link danger" title="Убрать штрихкод">убрать</button></form>`}</li>`)}
              ${codes.rows.length ? '' : html`<li class="muted">Штрихкодов нет</li>`}</ul>
            ${ro ? '' : html`<form method="post" action="/products/${id}/barcodes" class="inline-row">${csrfField(ctx)}
              <input name="barcode" placeholder="Отсканируйте или введите код" inputmode="numeric" maxlength="40" autocomplete="off">
              <button>Добавить</button>
              <button name="generate" value="1" title="Создать новый внутренний EAN-13">Сгенерировать свой</button>
            </form>`}
          </div>
          <div class="card">
            <h2>Остатки по складам</h2>
            <div class="table-wrap"><table>
              <thead><tr><th>Склад</th><th class="num">Шт</th><th>Место хранения</th><th></th></tr></thead>
              <tbody>${stockRows.rows.map((s) => {
                const loc = (s.locations || []).find((l) => l.id === s.location_id);
                return html`<tr>
                <td>${s.wname}</td><td class="num">${s.qty}</td>
                <td>${ro ? (loc ? loc.code : '') : html`<form method="post" action="/products/${id}/location" class="inline-row">${csrfField(ctx)}
                  <input type="hidden" name="warehouse" value="${s.wid}">
                  <select name="location">
                    <option value="">— не указано —</option>
                    ${(s.locations || []).map((l) => html`<option value="${l.id}" ${l.id === s.location_id ? html`selected` : ''}>${l.code}</option>`)}
                  </select><button>ОК</button></form>`}</td>
                <td>${ro ? '' : html`<a href="/operation?product=${encodeURIComponent(p.sku)}&warehouse=${s.wid}">операция</a>`}</td></tr>`;
              })}
                ${stockRows.rows.length ? '' : html`<tr><td colspan="4" class="muted">Складов пока нет</td></tr>`}</tbody>
            </table></div>
          </div>
        </div>
      </div>
      <h2>Последние движения</h2>
      <div class="table-wrap"><table>
        <thead><tr><th>Дата</th><th>Склад</th><th>Тип</th><th class="num">Изменение</th><th>Комментарий</th></tr></thead>
        <tbody>${moves.rows.map((m) => html`<tr><td>${fmtDate(m.created_at)}</td><td>${m.wname}</td><td>${TYPE_LABELS[m.type] || m.type}</td>
          <td class="num ${m.delta < 0 ? 'neg' : 'pos'}">${m.delta > 0 ? '+' : ''}${m.delta}</td><td>${m.comment}</td></tr>`)}
          ${moves.rows.length ? '' : html`<tr><td colspan="5" class="muted">Движений пока нет</td></tr>`}</tbody>
      </table></div>`,
    }, status);
  }
  app.locals.showProduct = showProduct;

  app.get('/products/:id', requireLogin, (req, res) => showProduct(req, res));

  app.post('/products/:id', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    if (!id) return res.redirect('/products');
    const { d, error } = readForm(req.body);
    if (error) return showProduct(req, res, { error, form: d }, 400);
    try {
      const r = await db.query(
        `UPDATE products SET sku = $2, name = $3, description = $4, price = $5, custom_code = $6, market_sku = $7 WHERE id = $1`,
        [id, d.sku, d.name, d.description, d.price, d.custom_code, d.market_sku]);
      res.flash(r.rowCount ? 'ok' : 'err', r.rowCount ? 'Сохранено' : 'Товар не найден');
      res.redirect(r.rowCount ? `/products/${id}` : '/products');
    } catch (e) {
      if (e.code === '23505') return showProduct(req, res, { error: 'Товар с таким артикулом уже есть', form: d }, 409);
      throw e;
    }
  });

  app.post('/products/:id/delete', requireLogin, requireAdmin, async (req, res) => {
    const id = parseId(req.params.id);
    if (id) await db.query('DELETE FROM products WHERE id = $1', [id]);
    res.flash('ok', 'Товар удалён');
    res.redirect('/products');
  });

  // ---- Штрихкоды
  app.post('/products/:id/barcodes', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    if (!id) return res.redirect('/products');
    const generate = req.body.generate === '1';
    for (let attempt = 0; attempt < (generate ? 5 : 1); attempt++) {
      const code = generate ? bc.generateInternal() : bc.normalize(req.body.barcode);
      const bad = generate ? null : bc.validate(code);
      if (bad) return showProduct(req, res, { barcodeError: `Штрихкод не добавлен: ${bad}` }, 400);
      let inserted;
      try {
        inserted = await db.query(
          'INSERT INTO product_barcodes (barcode, product_id) VALUES ($1, $2) ON CONFLICT (barcode) DO NOTHING', [code, id]);
      } catch (e) {
        if (e.code !== '23503') throw e;
        res.flash('err', 'Товар не найден');
        return res.redirect('/products');
      }
      if (inserted.rowCount) {
        res.flash('ok', `Штрихкод ${code} добавлен`);
        return res.redirect(`/products/${id}`);
      }
      const owner = (await db.query(
        'SELECT p.id, p.sku FROM product_barcodes b JOIN products p ON p.id = b.product_id WHERE b.barcode = $1', [code])).rows[0];
      if (owner && owner.id === id) {
        res.flash('ok', 'Этот штрихкод уже привязан к товару');
        return res.redirect(`/products/${id}`);
      }
      if (!generate) {
        return showProduct(req, res, { barcodeError: `Штрихкод ${code} уже привязан к товару ${owner?.sku ?? '?'}` }, 409);
      }
    }
    return showProduct(req, res, { barcodeError: 'Не удалось подобрать свободный код, попробуйте ещё раз' }, 500);
  });

  app.post('/products/:id/barcodes/delete', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    if (id) await db.query('DELETE FROM product_barcodes WHERE product_id = $1 AND barcode = $2', [id, bc.normalize(req.body.barcode)]);
    res.flash('ok', 'Штрихкод убран');
    res.redirect(`/products/${id || ''}`);
  });

  // ---- Место хранения товара на складе
  app.post('/products/:id/location', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    const wid = parseId(req.body.warehouse);
    const lid = req.body.location === '' ? null : parseId(req.body.location);
    if (!id || !wid || (req.body.location !== '' && !lid)) return res.redirect('/products');
    if (lid) {
      const ok = await db.query('SELECT 1 FROM locations WHERE id = $1 AND warehouse_id = $2', [lid, wid]);
      if (!ok.rowCount) { res.flash('err', 'Это место относится к другому складу'); return res.redirect(`/products/${id}`); }
    }
    try {
      await db.query(
        `INSERT INTO stock (product_id, warehouse_id, location_id, quantity) VALUES ($1, $2, $3, 0)
         ON CONFLICT (product_id, warehouse_id) DO UPDATE SET location_id = EXCLUDED.location_id`, [id, wid, lid]);
      res.flash('ok', 'Место хранения сохранено');
    } catch (e) {
      if (e.code !== '23503') throw e;
      res.flash('err', 'Товар или склад не найден');
    }
    res.redirect(`/products/${id}`);
  });
};

module.exports.searchSql = searchSql;

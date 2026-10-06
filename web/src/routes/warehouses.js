'use strict';
const { html } = require('../html');
const { csrfField } = require('../layout');
const { parseId, str, field } = require('../util');

module.exports = (app, { db, send, requireLogin, requireAdmin }) => {
  const list = async (req, res, { error = '', form = {} } = {}, status = 200) => {
    const { rows } = await db.query(
      `SELECT w.id, w.name, w.address,
              (SELECT COUNT(*) FROM locations l WHERE l.warehouse_id = w.id)::int AS locs,
              COALESCE((SELECT SUM(quantity) FROM stock s WHERE s.warehouse_id = w.id), 0)::int AS total
         FROM warehouses w ORDER BY w.name`);
    send(req, res, {
      title: 'Склады', active: 'warehouses',
      body: html`
      <form method="post" action="/warehouses" class="card">
        <h2>Новый склад</h2>
        ${error ? html`<div class="flash err">${error}</div>` : ''}
        ${csrfField(req.ctx)}
        <div class="row">${field('Название', 'name', form.name, { required: true, max: 200 })}
          ${field('Адрес', 'address', form.address, { max: 500 })}<button class="primary">Создать</button></div>
      </form>
      <div class="table-wrap"><table>
        <thead><tr><th>Склад</th><th>Адрес</th><th class="num">Мест хранения</th><th class="num">Всего шт</th></tr></thead>
        <tbody>${rows.map((w) => html`<tr><td><a href="/warehouses/${w.id}">${w.name}</a></td><td>${w.address}</td>
          <td class="num">${w.locs}</td><td class="num">${w.total}</td></tr>`)}
          ${rows.length ? '' : html`<tr><td colspan="4" class="muted">Складов пока нет</td></tr>`}</tbody>
      </table></div>`,
    }, status);
  };

  app.get('/warehouses', requireLogin, (req, res) => list(req, res));

  app.post('/warehouses', requireLogin, async (req, res) => {
    const form = { name: str(req.body.name, 200), address: str(req.body.address, 500) };
    if (!form.name) return list(req, res, { error: 'Введите название склада', form }, 400);
    try {
      const { rows } = await db.query('INSERT INTO warehouses (name, address) VALUES ($1, $2) RETURNING id', [form.name, form.address]);
      res.flash('ok', 'Склад создан');
      res.redirect(`/warehouses/${rows[0].id}`);
    } catch (e) {
      if (e.code === '23505') return list(req, res, { error: 'Склад с таким названием уже есть', form }, 409);
      throw e;
    }
  });

  const show = async (req, res, { error = '', locError = '', form = null } = {}, status = 200) => {
    const id = parseId(req.params.id);
    if (!id) return res.redirect('/warehouses');
    const w = (await db.query('SELECT id, name, address FROM warehouses WHERE id = $1', [id])).rows[0];
    if (!w) { res.flash('err', 'Склад не найден'); return res.redirect('/warehouses'); }
    const locs = (await db.query(
      `SELECT l.id, l.code, l.description, (SELECT COUNT(*) FROM stock s WHERE s.location_id = l.id)::int AS used
         FROM locations l WHERE l.warehouse_id = $1 ORDER BY l.code`, [id])).rows;
    const f = form || w;
    const ctx = req.ctx;
    send(req, res, {
      title: `Склад: ${w.name}`, active: 'warehouses',
      body: html`
      <div class="grid2">
        <form method="post" action="/warehouses/${id}" class="card">
          <h2>Данные склада</h2>
          ${error ? html`<div class="flash err">${error}</div>` : ''}
          ${csrfField(ctx)}
          ${field('Название', 'name', f.name, { required: true, max: 200 })}
          ${field('Адрес', 'address', f.address, { max: 500 })}
          <button class="primary">Сохранить</button>
        </form>
        <div class="card">
          <h2>Места хранения</h2>
          <p class="muted">Ячейки, стеллажи, полки: код вида A1-03.</p>
          ${locError ? html`<div class="flash err">${locError}</div>` : ''}
          <div class="table-wrap"><table>
            <thead><tr><th>Код</th><th>Описание</th><th class="num">Товаров</th><th></th></tr></thead>
            <tbody>${locs.map((l) => html`<tr><td>${l.code}</td><td>${l.description}</td><td class="num">${l.used}</td>
              <td><form method="post" action="/warehouses/${id}/locations/${l.id}/delete" class="inline" data-confirm="Удалить место ${l.code}?">
                ${csrfField(ctx)}<button class="link danger">удалить</button></form></td></tr>`)}
              ${locs.length ? '' : html`<tr><td colspan="4" class="muted">Мест пока нет</td></tr>`}</tbody>
          </table></div>
          <form method="post" action="/warehouses/${id}/locations" class="inline-row">${csrfField(ctx)}
            <input name="code" placeholder="Код места, например A1-03" required maxlength="50">
            <input name="description" placeholder="Описание (необязательно)" maxlength="200"><button>Добавить</button></form>
        </div>
      </div>
      ${ctx.user.is_admin ? html`<form method="post" action="/warehouses/${id}/delete"
        data-confirm="Удалить склад ${w.name} вместе с остатками и местами хранения?">${csrfField(ctx)}<button class="danger">Удалить склад</button></form>` : ''}`,
    }, status);
  };

  app.get('/warehouses/:id', requireLogin, (req, res) => show(req, res));

  app.post('/warehouses/:id', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    if (!id) return res.redirect('/warehouses');
    const form = { name: str(req.body.name, 200), address: str(req.body.address, 500) };
    if (!form.name) return show(req, res, { error: 'Введите название склада', form }, 400);
    try {
      const r = await db.query('UPDATE warehouses SET name = $2, address = $3 WHERE id = $1', [id, form.name, form.address]);
      res.flash(r.rowCount ? 'ok' : 'err', r.rowCount ? 'Сохранено' : 'Склад не найден');
      res.redirect(r.rowCount ? `/warehouses/${id}` : '/warehouses');
    } catch (e) {
      if (e.code === '23505') return show(req, res, { error: 'Склад с таким названием уже есть', form }, 409);
      throw e;
    }
  });

  app.post('/warehouses/:id/delete', requireLogin, requireAdmin, async (req, res) => {
    const id = parseId(req.params.id);
    if (id) await db.query('DELETE FROM warehouses WHERE id = $1', [id]);
    res.flash('ok', 'Склад удалён');
    res.redirect('/warehouses');
  });

  app.post('/warehouses/:id/locations', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    if (!id) return res.redirect('/warehouses');
    const code = str(req.body.code, 50);
    if (!code) return show(req, res, { locError: 'Введите код места' }, 400);
    try {
      await db.query('INSERT INTO locations (warehouse_id, code, description) VALUES ($1, $2, $3)',
        [id, code, str(req.body.description, 200)]);
      res.flash('ok', `Место ${code} добавлено`);
      res.redirect(`/warehouses/${id}`);
    } catch (e) {
      if (e.code === '23505') return show(req, res, { locError: 'Такое место на этом складе уже есть' }, 409);
      if (e.code === '23503') { res.flash('err', 'Склад не найден'); return res.redirect('/warehouses'); }
      throw e;
    }
  });

  app.post('/warehouses/:id/locations/:lid/delete', requireLogin, async (req, res) => {
    const id = parseId(req.params.id);
    const lid = parseId(req.params.lid);
    if (id && lid) await db.query('DELETE FROM locations WHERE id = $1 AND warehouse_id = $2', [lid, id]);
    res.flash('ok', 'Место удалено');
    res.redirect(`/warehouses/${id || ''}`);
  });
};

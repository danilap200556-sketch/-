'use strict';
const { html } = require('../html');
const { csrfField } = require('../layout');
const pw = require('../passwords');
const { parseId, fmtDate, field, str } = require('../util');

const NO_ADMIN = 'Нельзя оставить сайт и приложение без администратора';
const ROLES = { admin: 'Администратор', editor: 'Сотрудник', viewer: 'Только просмотр' };
const roleOf = (u) => (u.is_admin ? 'admin' : u.read_only ? 'viewer' : 'editor');
const roleSelect = (name, selected) => html`<select name="${name}">${Object.entries(ROLES).map(([k, t]) =>
  html`<option value="${k}" ${k === selected ? html`selected` : ''}>${t}</option>`)}</select>`;

module.exports = (app, { db, send, requireLogin, requireAdmin }) => {
  const guard = [requireLogin, requireAdmin];

  const view = async (req, res, { error = '', form = {} } = {}, status = 200) => {
    const { rows } = await db.query('SELECT id, username, is_admin, read_only, created_at FROM users ORDER BY username');
    const ctx = req.ctx;
    send(req, res, {
      title: 'Пользователи',
      active: 'users',
      body: html`
      <form method="post" action="/users" class="card">
        <h2>Новый пользователь</h2>
        ${error ? html`<div class="flash err">${error}</div>` : ''}
        ${csrfField(ctx)}
        <div class="row">
          ${field('Логин', 'username', form.username, { required: true, max: 64, autocomplete: 'off' })}
          <label class="field"><span>Пароль (от 8 символов)</span><input type="password" name="password" required minlength="8" maxlength="200" autocomplete="new-password"></label>
          <label class="field"><span>Роль</span>${roleSelect('role', form.role || 'editor')}</label>
          <button class="primary">Создать</button>
        </div>
      </form>
      <div class="table-wrap"><table>
        <thead><tr><th>Логин</th><th>Роль</th><th>Создан</th><th>Новый пароль</th><th></th></tr></thead>
        <tbody>${rows.map((u) => html`<tr>
          <td>${u.username}${u.id === ctx.user.id ? html` <span class="muted">(это вы)</span>` : ''}</td>
          <td><form method="post" action="/users/${u.id}/role" class="inline-row">${csrfField(ctx)}
            ${roleSelect('role', roleOf(u))}<button>ОК</button></form></td>
          <td>${fmtDate(u.created_at)}</td>
          <td><form method="post" action="/users/${u.id}/password" class="inline-row">${csrfField(ctx)}
            <input type="password" name="password" required minlength="8" maxlength="200" placeholder="новый пароль" autocomplete="new-password">
            <button>Задать</button></form></td>
          <td class="actions">
            ${u.id === ctx.user.id ? '' : html`<form method="post" action="/users/${u.id}/delete" class="inline" data-confirm="Удалить пользователя ${u.username}?">${csrfField(ctx)}
              <button class="danger">Удалить</button></form>`}
          </td></tr>`)}</tbody>
      </table></div>`,
    }, status);
  };

  app.get('/users', guard, (req, res) => view(req, res));

  app.post('/users', guard, async (req, res) => {
    const username = str(req.body.username, 64);
    const role = Object.hasOwn(ROLES, req.body.role) ? req.body.role : 'editor';
    const form = { username, role };
    if (!username || /[\x00-\x1f]/.test(username)) return view(req, res, { error: 'Введите логин', form }, 400);
    const bad = pw.validateNewPassword(req.body.password);
    if (bad) return view(req, res, { error: bad, form }, 400);
    const salt = pw.newSalt();
    try {
      await db.query('INSERT INTO users (username, password_hash, salt, is_admin, read_only) VALUES ($1, $2, $3, $4, $5)',
        [username, pw.hashPassword(req.body.password, salt), salt, role === 'admin', role === 'viewer']);
    } catch (e) {
      if (e.code === '23505') return view(req, res, { error: 'Пользователь с таким логином уже существует', form }, 409);
      throw e;
    }
    res.flash('ok', `Пользователь ${username} создан`);
    res.redirect('/users');
  });

  app.post('/users/:id/password', guard, async (req, res) => {
    const id = parseId(req.params.id);
    const bad = pw.validateNewPassword(req.body.password);
    if (!id) return res.redirect('/users');
    if (bad) { res.flash('err', bad); return res.redirect('/users'); }
    const salt = pw.newSalt();
    const r = await db.query('UPDATE users SET password_hash = $1, salt = $2 WHERE id = $3',
      [pw.hashPassword(req.body.password, salt), salt, id]);
    res.flash(r.rowCount ? 'ok' : 'err', r.rowCount ? 'Пароль изменён' : 'Пользователь не найден');
    res.redirect('/users');
  });

  // Правило "всегда остаётся хотя бы один админ" проверяется внутри самого UPDATE/DELETE
  // (как в приложении), а админские строки блокируются на время проверки.
  const withAdminLock = (fn) => db.tx(async (c) => {
    await c.query('SELECT id FROM users WHERE is_admin FOR UPDATE');
    return fn(c);
  });
  const otherAdmin = 'EXISTS (SELECT 1 FROM users o WHERE o.is_admin AND o.id <> users.id)';

  app.post('/users/:id/role', guard, async (req, res) => {
    const id = parseId(req.params.id);
    if (!id || !Object.hasOwn(ROLES, req.body.role)) return res.redirect('/users');
    const role = req.body.role;
    const r = await withAdminLock(async (c) => {
      if (!(await c.query('SELECT 1 FROM users WHERE id = $1', [id])).rowCount) return { missing: true };
      return role === 'admin'
        ? c.query('UPDATE users SET is_admin = TRUE, read_only = FALSE WHERE id = $1', [id])
        : c.query(`UPDATE users SET is_admin = FALSE, read_only = $2 WHERE id = $1 AND (NOT is_admin OR ${otherAdmin})`, [id, role === 'viewer']);
    });
    if (r.missing) res.flash('err', 'Пользователь не найден');
    else if (r.rowCount) res.flash('ok', `Роль изменена: ${ROLES[role]}. Вступит в силу сразу на сайте и при следующем входе в приложение`);
    else res.flash('err', NO_ADMIN);
    res.redirect('/users');
  });

  app.post('/users/:id/delete', guard, async (req, res) => {
    const id = parseId(req.params.id);
    if (!id) return res.redirect('/users');
    if (id === req.ctx.user.id) {
      res.flash('err', 'Себя удалить нельзя - попросите другого администратора');
      return res.redirect('/users');
    }
    const r = await withAdminLock((c) =>
      c.query(`DELETE FROM users WHERE id = $1 AND (NOT is_admin OR ${otherAdmin})`, [id]));
    res.flash(r.rowCount ? 'ok' : 'err', r.rowCount ? 'Пользователь удалён' : NO_ADMIN);
    res.redirect('/users');
  });
};

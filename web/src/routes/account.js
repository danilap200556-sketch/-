'use strict';
const { html } = require('../html');
const { csrfField } = require('../layout');
const pw = require('../passwords');
const sec = require('../security');

module.exports = (app, { db, send, requireLogin, startSession }) => {
  const limiter = new sec.RateLimiter(5, 15 * 60_000);

  const view = (req, res, error = '', status = 200) =>
    send(req, res, {
      title: 'Мой аккаунт',
      active: 'account',
      body: html`<form method="post" action="/account/password" class="card narrow">
        <h2>Сменить пароль</h2>
        <p class="muted">Логин: <b>${req.ctx.user.username}</b>${req.ctx.user.is_admin ? ' (администратор)' : ''}. Пароль меняется и в приложении на компьютере.</p>
        ${error ? html`<div class="flash err">${error}</div>` : ''}
        ${csrfField(req.ctx)}
        <label class="field"><span>Текущий пароль</span><input type="password" name="current" required autocomplete="current-password" maxlength="200"></label>
        <label class="field"><span>Новый пароль (от 8 символов)</span><input type="password" name="password" required minlength="8" autocomplete="new-password" maxlength="200"></label>
        <label class="field"><span>Повторите новый пароль</span><input type="password" name="confirm" required autocomplete="new-password" maxlength="200"></label>
        <button class="primary">Сменить пароль</button>
      </form>`,
    }, status);

  app.get('/account', requireLogin, (req, res) => view(req, res));

  app.post('/account/password', requireLogin, async (req, res) => {
    const { current = '', password = '', confirm = '' } = req.body;
    const key = `pw:${req.ctx.user.id}`;
    if (limiter.blocked(key)) return view(req, res, 'Слишком много неверных попыток. Подождите 15 минут.', 429);
    const { rows } = await db.query('SELECT id, password_hash, salt FROM users WHERE id = $1', [req.ctx.user.id]);
    if (!rows[0] || !pw.verifyPassword(String(current).slice(0, 200), rows[0].salt, rows[0].password_hash)) {
      limiter.fail(key);
      return view(req, res, 'Текущий пароль неверен', 400);
    }
    if (password !== confirm) return view(req, res, 'Пароли не совпадают', 400);
    const bad = pw.validateNewPassword(password);
    if (bad) return view(req, res, bad, 400);
    const salt = pw.newSalt();
    const hash = pw.hashPassword(password, salt);
    await db.query('UPDATE users SET password_hash = $1, salt = $2 WHERE id = $3', [hash, salt, req.ctx.user.id]);
    limiter.reset(key);
    // Старые сессии (в т.ч. на других устройствах) перестают действовать; эта продолжается.
    startSession(res, { id: req.ctx.user.id, password_hash: hash, salt }, req.ctx.session.sid);
    res.flash('ok', 'Пароль изменён');
    res.redirect('/account');
  });
};

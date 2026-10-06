'use strict';
const path = require('path');
const crypto = require('crypto');
const express = require('express');
const { html } = require('./html');
const { page, csrfField } = require('./layout');
const sec = require('./security');
const pw = require('./passwords');
const { field, str } = require('./util');

const COOKIE_SESSION = 'session';
const COOKIE_FLASH = 'flash';

function createApp(cfg, db) {
  const app = express();
  app.disable('x-powered-by');
  app.set('trust proxy', cfg.trustProxy);

  const sessionName = cfg.secureCookies ? '__Host-session' : COOKIE_SESSION;
  const flashName = cfg.secureCookies ? '__Host-flash' : COOKIE_FLASH;
  const cookieOpts = (maxAgeMs) => ({
    httpOnly: true, sameSite: 'lax', secure: cfg.secureCookies, path: '/', ...(maxAgeMs ? { maxAge: maxAgeMs } : {}),
  });

  // --- Заголовки безопасности. Скрипты и стили - только свои файлы (без inline).
  app.use((req, res, next) => {
    res.set({
      'Content-Security-Policy': "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; " +
        "form-action 'self'; base-uri 'none'; frame-ancestors 'none'",
      'X-Content-Type-Options': 'nosniff',
      'X-Frame-Options': 'DENY',
      'Referrer-Policy': 'no-referrer',
      'Cache-Control': 'no-store',
    });
    if (cfg.secureCookies) res.set('Strict-Transport-Security', 'max-age=31536000');
    next();
  });

  app.get('/healthz', (req, res) => res.type('text').send('ok'));
  app.get('/favicon.ico', (req, res) => res.status(204).end());
  app.use('/static', express.static(path.join(__dirname, '..', 'public'), { maxAge: '1h', index: false }));
  app.use(express.urlencoded({ extended: false, limit: '512kb', parameterLimit: 6000 }));
  app.use((req, res, next) => { req.body ??= {}; next(); }); // POST без тела / не того типа

  // Таблицы создаёт настольное приложение; пока база недоступна или не подготовлена - 503.
  let schemaOk = false;
  app.use(async (req, res, next) => {
    if (schemaOk) return next();
    try {
      await db.checkSchema();
      schemaOk = true;
      next();
    } catch (e) {
      console.error('[schema]', e.message);
      res.status(503).type('text').send('Сервис временно недоступен. Подробности - в журнале сервера.');
    }
  });

  // --- Защита от межсайтовых POST: браузер сам сообщает, откуда пришёл запрос.
  app.use((req, res, next) => {
    if (req.method === 'GET' || req.method === 'HEAD') return next();
    const site = req.get('sec-fetch-site');
    let ok = true;
    if (site) {
      ok = site === 'same-origin' || site === 'none';
    } else if (req.get('origin')) {
      try { ok = new URL(req.get('origin')).host === req.get('host'); } catch { ok = false; }
    }
    if (!ok) return res.status(403).type('text').send('Запрос отклонён (межсайтовый запрос)');
    next();
  });

  // --- Контекст запроса: cookie, одноразовое сообщение, пользователь из сессии.
  app.use(async (req, res, next) => {
    const cookies = sec.parseCookies(req.headers.cookie);
    req.cookies = cookies;
    req.ctx = { user: null, csrf: '', flash: null, session: null };

    const flash = sec.unsign(cfg.secret, cookies[flashName]);
    if (flash) {
      req.ctx.flash = { kind: flash.k === 'err' ? 'err' : 'ok', text: String(flash.t || '') };
      res.clearCookie(flashName, cookieOpts());
    }
    res.flash = (kind, text) => res.cookie(flashName, sec.sign(cfg.secret, { k: kind, t: text }), cookieOpts(60_000));

    const s = sec.unsign(cfg.secret, cookies[sessionName]);
    if (s && typeof s.uid === 'number' && s.exp > Date.now()) {
      const { rows } = await db.query(
        'SELECT id, username, is_admin, read_only, password_hash, salt FROM users WHERE id = $1', [s.uid]);
      const u = rows[0];
      if (u && s.v === sec.passwordVersion(u)) {
        // Администратор всегда редактирует; read_only у него (мог остаться от старой роли) не действует.
        req.ctx.user = { id: u.id, username: u.username, is_admin: u.is_admin, read_only: u.read_only && !u.is_admin };
        req.ctx.session = s;
        req.ctx.csrf = sec.csrfToken(cfg.secret, s);
        // Скользящий срок: продлеваем, когда прошла половина.
        if (s.exp - Date.now() < cfg.sessionHours * 1800_000) startSession(res, u, s.sid);
      }
    }
    next();
  });

  function startSession(res, userRow, sid = crypto.randomBytes(12).toString('base64url')) {
    const maxAge = cfg.sessionHours * 3600_000;
    const token = sec.sign(cfg.secret, { uid: userRow.id, v: sec.passwordVersion(userRow), sid, exp: Date.now() + maxAge });
    res.cookie(sessionName, token, cookieOpts(maxAge));
  }
  app.locals.startSession = startSession;

  const send = (req, res, view, status = 200) => {
    res.status(status).type('html').send(page(req.ctx, view).toString());
  };
  app.locals.send = send;

  // POST, которые можно и в режиме "только просмотр".
  const VIEWER_POSTS = new Set(['/logout', '/account/password']);

  // CSRF-токен обязателен для каждого POST вошедшего пользователя. Для multipart-форм
  // (загрузка файлов) тело разбирается в readMultipart(), который сам проверяет токен
  // (заголовок X-CSRF-Token или поле _csrf) - здесь такие запросы только помечаются.
  const requireLogin = (req, res, next) => {
    if (!req.ctx.user) {
      if (req.method === 'GET') {
        const next_ = req.originalUrl.startsWith('/') ? req.originalUrl : '/';
        return res.redirect('/login?next=' + encodeURIComponent(next_));
      }
      return res.redirect('/login');
    }
    if (req.method !== 'GET' && req.method !== 'HEAD') {
      if (req.is('multipart/form-data')) {
        req.csrfPending = !sec.safeEqual(req.get('x-csrf-token') ?? '', req.ctx.csrf);
      } else if (!sec.safeEqual(req.body?._csrf ?? '', req.ctx.csrf)) {
        return res.status(403).type('text').send('Страница устарела. Вернитесь назад, обновите её и повторите.');
      }
      // Роль "только просмотр": на сервере запрещены все изменения, а не только спрятаны кнопки.
      if (req.ctx.user.read_only && !VIEWER_POSTS.has(req.path)) {
        const msg = 'У вас доступ только для просмотра - изменять данные нельзя';
        if (req.accepts(['html', 'json']) === 'json') return res.status(403).json({ error: msg });
        res.flash('err', msg);
        return res.redirect('/');
      }
    }
    next();
  };
  // Страницы с формами изменения: пользователю "только просмотр" они не нужны.
  const requireEditor = (req, res, next) => {
    if (req.ctx.user.read_only) {
      res.flash('err', 'У вас доступ только для просмотра');
      return res.redirect('/');
    }
    next();
  };
  const requireAdmin = (req, res, next) => {
    if (!req.ctx.user.is_admin) {
      res.flash('err', 'Это действие доступно только администратору');
      return res.redirect('/');
    }
    next();
  };
  app.locals.requireAdmin = requireAdmin;

  // --- Вход / выход
  const ipLimit = new sec.RateLimiter(30, 15 * 60_000);
  const userLimit = new sec.RateLimiter(5, 15 * 60_000);

  const loginView = (req, res, { error = '', username = '', next: nextUrl = '' } = {}, status = 200) =>
    send(req, res, {
      title: 'Вход',
      body: html`<form method="post" action="/login" class="card narrow">
        <input type="hidden" name="next" value="${nextUrl}">
        ${error ? html`<div class="flash err">${error}</div>` : ''}
        ${field('Логин', 'username', username, { required: true, max: 64, autofocus: !username, autocomplete: 'username' })}
        <label class="field"><span>Пароль</span><input type="password" name="password" required maxlength="200" autocomplete="current-password" ${username ? html`autofocus` : ''}></label>
        <button class="primary">Войти</button>
        <p class="muted">Логин и пароль - те же, что в приложении на компьютере.</p>
      </form>`,
    }, status);

  const safeNext = (n) => (typeof n === 'string' && /^\/(?![/\\])[^\r\n]*$/.test(n) ? n : '/');

  app.get('/login', (req, res) => {
    if (req.ctx.user) return res.redirect('/');
    loginView(req, res, { next: safeNext(req.query.next) });
  });

  app.post('/login', async (req, res) => {
    const username = str(req.body.username, 64);
    const password = typeof req.body.password === 'string' ? req.body.password.slice(0, 200) : '';
    const nextUrl = safeNext(req.body.next);
    const ipKey = `ip:${req.ip}`;
    const userKey = `u:${req.ip}:${username.toLowerCase()}`;
    if (ipLimit.blocked(ipKey) || userLimit.blocked(userKey)) {
      return loginView(req, res, { error: 'Слишком много неудачных попыток. Подождите 15 минут.', username, next: nextUrl }, 429);
    }
    const { rows } = await db.query('SELECT id, username, is_admin, read_only, password_hash, salt FROM users WHERE username = $1', [username]);
    const u = rows[0];
    // Для несуществующего логина считаем хеш тоже - чтобы по времени ответа логины не подбирались.
    const ok = u ? pw.verifyPassword(password, u.salt, u.password_hash)
                 : (pw.verifyPassword(password, pw.newSalt(), ''), false);
    if (!ok) {
      ipLimit.fail(ipKey);
      userLimit.fail(userKey);
      return loginView(req, res, { error: 'Неверный логин или пароль', username, next: nextUrl }, 401);
    }
    userLimit.reset(userKey);
    startSession(res, u);
    res.redirect(nextUrl);
  });

  app.post('/logout', requireLogin, (req, res) => {
    res.clearCookie(sessionName, cookieOpts());
    res.redirect('/login');
  });

  // --- Дальше только для вошедших
  app.get('/', requireLogin, (req, res) => res.redirect('/stock'));

  const ctxFor = { cfg, db, send, requireLogin, requireAdmin, requireEditor, startSession, flashName, sessionName };
  require('./routes/account')(app, ctxFor);
  require('./routes/products')(app, ctxFor);
  require('./routes/warehouses')(app, ctxFor);
  require('./routes/stock')(app, ctxFor);
  require('./routes/photos')(app, ctxFor);
  require('./routes/bulk')(app, ctxFor);
  require('./routes/import')(app, ctxFor);
  require('./routes/orders')(app, ctxFor);
  require('./routes/users')(app, ctxFor);

  app.use((req, res) => {
    if (!req.ctx.user) return res.redirect('/login');
    send(req, res, { title: 'Не найдено', body: html`<p>Такой страницы нет. <a href="/">На главную</a></p>` }, 404);
  });

  // eslint-disable-next-line no-unused-vars
  app.use((err, req, res, next) => {
    // Ошибки разбора запроса (слишком большое тело, битая кодировка) - вина клиента, а не сервера.
    if (err.status >= 400 && err.status < 500 && !res.headersSent) {
      return res.status(err.status).type('text').send(err.expose ? err.message : 'Некорректный запрос');
    }
    console.error('[error]', req.method, req.path, err);
    const dbDown = !err.code || /^(08|57P0|53300|28)/.test(err.code) || /ECONN|ETIMEDOUT|terminated|timeout|ENOTFOUND/i.test(err.message || '');
    const status = dbDown ? 503 : 500;
    const text = dbDown ? 'База данных временно недоступна. Попробуйте через минуту.' : 'Внутренняя ошибка. Попробуйте ещё раз.';
    if (res.headersSent) return;
    try {
      send(req, res, { title: 'Ошибка', body: html`<div class="flash err">${text}</div><p><a href="/">На главную</a></p>` }, status);
    } catch {
      res.status(status).type('text').send(text);
    }
  });

  return app;
}

module.exports = { createApp };

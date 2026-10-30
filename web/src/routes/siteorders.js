'use strict';
const crypto = require('crypto');
const express = require('express');
const sec = require('../security');

// Приём заказов от сайта-магазина. Заказ попадает в очередь (таблица order_writeoffs, state = pending); со склада его
// списывает сотрудник кнопкой «Списать со склада...» в приложении (вкладка «Заказы Маркета»). Если сайт отменил заказ -
// до списания он просто снимается с очереди, а после списания пара возвращается на склад той же кнопкой.
//
//   POST /api/orders              {"order_id": "A-1001", "items": [{"sku": "DD1391-100_42", "qty": 1}, {"barcode": "2000000000017", "qty": 2}]}
//   GET  /api/orders/<order_id>   состояние заказа
//   POST /api/orders/<order_id>/cancel
// Токен: заголовок Authorization: Bearer <SITE_ORDERS_TOKEN> (или X-Orders-Token). Запросы - от сервера сайта, не из браузера.
// Заказ принимается один раз: повтор с тем же номером и составом отвечает 200 (можно безопасно повторять при обрыве связи).

const MAX_ITEMS = 200;
const MAX_QTY = 1000;
const REF_RE = /^[A-Za-z0-9][A-Za-z0-9._:/#-]{0,63}$/;
const PUBLIC_STATE = {
  pending: 'queued', writing: 'processing', written_off: 'written_off', cancel_requested: 'cancel_requested',
  returning: 'processing', returned: 'returned', cancelled: 'cancelled',
};

const sha = (s) => crypto.createHash('sha256').update(String(s)).digest('hex');
const BAD = (message) => Object.assign(new Error(message), { badRequest: true });

// Состав заказа из тела запроса: [{key, name, qty}], одинаковые артикулы складываются.
function parseOrder(body) {
  if (!body || typeof body !== 'object' || Array.isArray(body)) throw BAD('Ожидается JSON-объект {"order_id": "...", "items": [...]}');
  const ref = typeof body.order_id === 'string' || typeof body.order_id === 'number' ? String(body.order_id).trim() : '';
  if (!REF_RE.test(ref)) throw BAD('order_id: нужна строка до 64 символов (буквы, цифры и . _ : / # -)');
  if (!Array.isArray(body.items) || body.items.length === 0) throw BAD('items: нужен непустой список позиций');
  if (body.items.length > MAX_ITEMS) throw BAD(`items: не больше ${MAX_ITEMS} позиций`);
  const byKey = new Map();
  body.items.forEach((it, i) => {
    if (!it || typeof it !== 'object') throw BAD(`items[${i}]: ожидается объект {"sku": "...", "qty": 1}`);
    const raw = it.sku ?? it.article ?? it.key ?? it.offer_id ?? it.barcode ?? it.ean;
    const key = typeof raw === 'string' || typeof raw === 'number' ? String(raw).trim() : '';
    if (!key || key.length > 100) throw BAD(`items[${i}]: нужен sku (артикул) или barcode (штрихкод) до 100 символов`);
    const q = it.qty ?? it.quantity ?? it.count;
    const qty = typeof q === 'number' ? q : (typeof q === 'string' && /^\d+$/.test(q) ? Number(q) : NaN);
    if (!Number.isInteger(qty) || qty < 1 || qty > MAX_QTY) throw BAD(`items[${i}]: qty - целое число от 1 до ${MAX_QTY}`);
    const name = typeof it.name === 'string' ? it.name.trim().slice(0, 200) : '';
    const k = key.toLowerCase();
    const cur = byKey.get(k);
    if (cur) { cur.qty += qty; if (!cur.name && name) cur.name = name; } else byKey.set(k, { key, name, qty });
  });
  for (const l of byKey.values()) if (l.qty > MAX_QTY) throw BAD(`позиция ${l.key}: суммарно больше ${MAX_QTY} шт`);
  return { ref, lines: [...byKey.values()] };
}

const fingerprint = (lines) => JSON.stringify(lines.map((l) => [String(l.key).toLowerCase(), l.qty]).sort());

module.exports = (app, { cfg, db }) => {
  const token = cfg.siteOrdersToken;
  const failures = new sec.RateLimiter(20, 10 * 60_000);
  const router = express.Router();

  router.use((req, res, next) => {
    if (!token) return res.status(503).json({ error: 'Приём заказов выключен: на сервере сайта не задан SITE_ORDERS_TOKEN' });
    const key = `orders:${req.ip}`;
    if (failures.blocked(key)) return res.status(429).json({ error: 'Слишком много запросов с неверным токеном. Подождите 10 минут.' });
    const bearer = /^Bearer\s+(.+)$/i.exec(req.get('authorization') || '');
    const given = (bearer ? bearer[1] : req.get('x-orders-token') || '').trim();
    if (!given || !sec.safeEqual(sha(given), sha(token))) {
      failures.fail(key);
      return res.status(401).json({ error: 'Неверный или отсутствующий токен' });
    }
    next();
  });
  router.use(express.json({ limit: '128kb' }));

  const where = "source = 'site' AND account_id = 0 AND order_ref = $1";

  router.post('/', async (req, res) => {
    let order;
    try { order = parseOrder(req.body); } catch (e) { if (e.badRequest) return res.status(400).json({ error: e.message }); throw e; }
    const ins = await db.query(
      `INSERT INTO order_writeoffs (source, account_id, order_ref, state, lines) VALUES ('site', 0, $1, 'pending', $2::jsonb)
       ON CONFLICT (source, account_id, order_ref) DO NOTHING RETURNING id`, [order.ref, JSON.stringify(order.lines)]);
    if (ins.rows.length) return res.status(201).json({ ok: true, order_id: order.ref, status: 'queued' });
    const ex = (await db.query(`SELECT state, lines FROM order_writeoffs WHERE ${where}`, [order.ref])).rows[0];
    if (ex && fingerprint(ex.lines) === fingerprint(order.lines)) {
      return res.status(200).json({ ok: true, order_id: order.ref, status: 'exists', state: PUBLIC_STATE[ex.state] || ex.state });
    }
    return res.status(409).json({ error: 'Заказ с таким номером уже принят с другим составом', order_id: order.ref });
  });

  router.get('/:ref', async (req, res) => {
    const ex = (await db.query(`SELECT state FROM order_writeoffs WHERE ${where}`, [String(req.params.ref).slice(0, 64)])).rows[0];
    if (!ex) return res.status(404).json({ error: 'Такого заказа нет' });
    res.json({ order_id: req.params.ref, state: PUBLIC_STATE[ex.state] || ex.state });
  });

  router.post('/:ref/cancel', async (req, res) => {
    const ref = String(req.params.ref).slice(0, 64);
    // Одним запросом: состояние читается под блокировкой строки, поэтому гонка с кнопкой в приложении невозможна.
    const r = await db.query(
      `WITH old AS (SELECT id, state FROM order_writeoffs WHERE ${where} FOR UPDATE),
            upd AS (UPDATE order_writeoffs o SET state = CASE old.state WHEN 'pending' THEN 'cancelled' WHEN 'written_off' THEN 'cancel_requested' ELSE o.state END
                    FROM old WHERE o.id = old.id RETURNING o.state AS after)
       SELECT old.state AS before, upd.after FROM old, upd`, [ref]);
    const row = r.rows[0];
    if (!row) return res.status(404).json({ error: 'Такого заказа нет' });
    if (row.before === 'writing' || row.before === 'returning') {
      return res.status(409).json({ error: 'Заказ сейчас обрабатывается на складе, повторите через минуту' });
    }
    const note = row.before === 'written_off' ? 'Заказ уже списан: пара вернётся на склад, когда сотрудник нажмёт кнопку в приложении' : undefined;
    res.status(200).json({ ok: true, order_id: ref, state: PUBLIC_STATE[row.after] || row.after, ...(note ? { note } : {}) });
  });

  // Ошибки разбора JSON и слишком большое тело - вина клиента; отвечаем JSON, а не страницей сайта.
  // eslint-disable-next-line no-unused-vars
  router.use((err, req, res, next) => {
    if (err.status >= 400 && err.status < 500) return res.status(err.status).json({ error: 'Некорректный запрос: ожидается JSON до 128 КБ' });
    console.error('[orders]', err);
    if (err.code === '42P01') return res.status(503).json({ error: 'В базе нет таблицы заказов: обновите приложение на компьютере (оно создаёт таблицы) и запустите его один раз' });
    if (err.code === '42501') return res.status(503).json({ error: 'У пользователя базы для сайта нет прав на таблицу order_writeoffs: выполните web/grants.sql' });
    const dbDown = !err.code || /^(08|57P0|53300|28)/.test(err.code) || /ECONN|ETIMEDOUT|terminated|timeout|ENOTFOUND/i.test(err.message || '');
    res.status(dbDown ? 503 : 500).json({ error: dbDown ? 'База данных временно недоступна' : 'Внутренняя ошибка' });
  });

  app.use('/api/orders', router);
};

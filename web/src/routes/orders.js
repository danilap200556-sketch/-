'use strict';
const crypto = require('crypto');
const { html } = require('../html');
const { csrfField } = require('../layout');
const { MarketApi, MarketError } = require('../market');
const xlsx = require('../xlsx');
const { fmtDate, parseId, str } = require('../util');
const { thumbImg, COVER_SQL } = require('../views');

const TTL_MS = 20 * 60_000;
const MAX_BATCH = 1000; // заказов в одном файле ярлыков (лимит API)

const STATUS_FILTERS = [
  ['Все заказы в обработке — ждут сборки и отгрузки', ['PROCESSING'], []],
  ['Готовы к отгрузке', ['PROCESSING'], ['READY_TO_SHIP']],
  ['В сборке', ['PROCESSING'], ['STARTED']],
  ['Переданы в доставку', ['DELIVERY'], []],
  ['Все заказы за последние 30 дней', [], []],
];
const FORMATS = [['A7', 'A7 — 75×120 мм'], ['A4', 'A4 — лист, формат из настроек кабинета'],
  ['A9_HORIZONTALLY', 'A9 — 58×40 мм, горизонтально'], ['A9', 'A9 — 40×58 мм']];
const STATUS_NAMES = { DELIVERY: 'в доставке', PICKUP: 'в пункте выдачи', DELIVERED: 'доставлен', CANCELLED: 'отменён',
  UNPAID: 'не оплачен', PENDING: 'ожидает подтверждения', RETURNED: 'возвращён', PARTIALLY_RETURNED: 'частично возвращён' };
const humanStatus = (st, sub) => (st === 'PROCESSING'
  ? ({ STARTED: 'в сборке', READY_TO_SHIP: 'готов к отгрузке', SHIPPED: 'отгружен' }[sub] || 'в обработке')
  : (STATUS_NAMES[st] || st));
const humanShip = (iso) => (/^\d{4}-\d{2}-\d{2}$/.test(iso) ? iso.split('-').reverse().join('.') : iso);

const isoDate = (s) => {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(s)) return null;
  const d = new Date(`${s}T00:00:00Z`);
  return Number.isNaN(d.getTime()) || d.toISOString().slice(0, 10) !== s ? null : s;
};
const dayDiff = (a, b) => Math.round((new Date(`${b}T00:00:00Z`) - new Date(`${a}T00:00:00Z`)) / 86_400_000);
const todayIso = () => new Date().toLocaleDateString('sv-SE', { timeZone: process.env.DISPLAY_TZ || 'Europe/Moscow' });
const plusDays = (iso, n) => { const d = new Date(`${iso}T00:00:00Z`); d.setUTCDate(d.getUTCDate() + n); return d.toISOString().slice(0, 10); };
const safeFileName = (s) => s.replace(/[<>:"/\\|?*\u0000-\u001f]/g, '_').replace(/\s+/g, ' ').trim().slice(0, 80);

module.exports = (app, { db, send, requireLogin, requireEditor }) => {
  const cache = new Map();
  const sweep = () => { const now = Date.now(); for (const [k, v] of cache) if (now - v.at > TTL_MS) cache.delete(k); };
  setInterval(sweep, 5 * 60_000).unref();
  const taken = (req, id) => {
    sweep();
    const e = cache.get(String(id));
    return e && e.userId === req.ctx.user.id ? e : null;
  };

  // Ключи кабинетов читаются только здесь, на сервере, и никогда не попадают на страницы.
  const loadAccounts = async () =>
    (await db.query('SELECT id, name, api_key, business_id, campaign_id FROM market_accounts ORDER BY name')).rows;

  // Магазины с одним ключом и кабинетом запрашиваются одним запросом (и ярлыки - одним файлом).
  function groupAccounts(accounts, only) {
    const groups = [];
    for (const a of accounts) {
      if (only && a.id !== only) continue;
      let g = groups.find((x) => x.apiKey === a.api_key && x.businessId === String(a.business_id));
      if (!g) { g = { apiKey: a.api_key, businessId: String(a.business_id), names: new Map() }; groups.push(g); }
      g.names.set(String(a.campaign_id), a.name);
    }
    return groups;
  }

  // Где товар лежит у нас: "Склад А: A1-03 (5 шт); Склад Б (2 шт)" - по артикулу на Маркете или нашему артикулу.
  async function loadLocations() {
    const rows = (await db.query(
      `SELECT COALESCE(NULLIF(TRIM(p.market_sku), ''), p.sku) AS offer, w.name AS wname, l.code, s.quantity
         FROM stock s JOIN products p ON p.id = s.product_id JOIN warehouses w ON w.id = s.warehouse_id
         LEFT JOIN locations l ON l.id = s.location_id WHERE s.quantity > 0 ORDER BY w.name`)).rows;
    const m = new Map();
    for (const r of rows) {
      const place = r.code ? `${r.wname}: ${r.code}` : r.wname;
      m.set(r.offer, [...(m.get(r.offer) || []), `${place} (${r.quantity} шт)`]);
    }
    return new Map([...m].map(([k, v]) => [k, v.join('; ')]));
  }

  async function loadProductInfo() {
    const rows = (await db.query(
      `SELECT p.id, COALESCE(NULLIF(TRIM(p.market_sku), ''), p.sku) AS offer, ${COVER_SQL} AS cover_id FROM products p`)).rows;
    return new Map(rows.map((r) => [r.offer, r]));
  }

  const NO_STOCK = 'нет в наличии у нас';

  function form(req, q, accounts) {
    const link = (label, from, to) => html`<a class="button" href="/orders?${new URLSearchParams({ ...q.base, from, to, go: '1' }).toString()}">${label}</a>`;
    const today = todayIso();
    return html`<form method="get" action="/orders" class="card">
      <input type="hidden" name="go" value="1">
      <div class="row">
        <label class="field"><span>Кабинет</span><select name="acc"><option value="">Все кабинеты</option>
          ${accounts.map((a) => html`<option value="${a.id}" ${String(a.id) === q.acc ? html`selected` : ''}>${a.name}</option>`)}</select></label>
        <label class="field"><span>Заказы</span><select name="status">
          ${STATUS_FILTERS.map((f, i) => html`<option value="${i}" ${String(i) === q.status ? html`selected` : ''}>${f[0]}</option>`)}</select></label>
      </div>
      <div class="row">
        <label class="field"><span>Отгрузка с</span><input type="date" name="from" value="${q.from}"></label>
        <label class="field"><span>по (включительно)</span><input type="date" name="to" value="${q.to}"></label>
        <button class="primary">Загрузить заказы</button>
      </div>
      <p class="toolbar">Быстро: ${link('Отгрузка сегодня', today, today)} ${link('Отгрузка завтра', plusDays(today, 1), plusDays(today, 1))}
        ${link('Любая дата', '', '')}</p>
      <p class="muted">Заказы нужной даты отгрузки загружаются сразу с Маркета; без дат - за последние 30 дней. Окно дат - не больше 30 дней.
      Ярлыки есть только у заказов в обработке.</p>
    </form>`;
  }

  app.get('/orders', requireLogin, requireEditor, async (req, res) => {
    const accounts = await loadAccounts();
    const q = {
      acc: String(parseId(req.query.acc) || ''), status: String(Math.min(Math.max(parseInt(req.query.status, 10) || 0, 0), STATUS_FILTERS.length - 1)),
      from: isoDate(str(req.query.from, 10)) || '', to: isoDate(str(req.query.to, 10)) || '',
    };
    q.base = { acc: q.acc, status: q.status };
    const head = (extra) => ({ title: 'Заказы Маркета', active: 'orders', body: html`${form(req, q, accounts)}${extra}` });
    if (req.query.go !== '1') {
      return send(req, res, head(accounts.length ? '' : html`<p class="flash err">Кабинеты Маркета не настроены. Добавьте их во вкладке «Яндекс Маркет» в приложении на компьютере.</p>`));
    }
    if (!accounts.length) return send(req, res, head(html`<p class="flash err">Кабинеты Маркета не настроены. Добавьте их в приложении на компьютере.</p>`));
    if (q.to && !q.from) q.from = q.to;
    if (q.from && !q.to) q.to = q.from;
    if (q.from && (q.to < q.from)) return send(req, res, head(html`<p class="flash err">Дата «по» раньше даты «с»</p>`), 400);
    if (q.from && dayDiff(q.from, q.to) + 1 > 30) return send(req, res, head(html`<p class="flash err">Маркет отдаёт заказы по дате отгрузки не больше чем за 30 дней - сократите период</p>`), 400);

    const [, statuses, substatuses] = STATUS_FILTERS[Number(q.status)];
    const groups = groupAccounts(accounts, parseId(q.acc));
    const problems = [];
    const loaded = [];
    await Promise.all(groups.map(async (g, gi) => {
      const names = [...g.names.values()].join(', ');
      try {
        const api = new MarketApi(g.apiKey, { log: (m) => console.error(`[market] ${names}: ${m}`) });
        const orders = await api.orders(g.businessId, [...g.names.keys()].map(Number), {
          statuses, substatuses, shipmentFrom: q.from, shipmentTo: q.to,
        });
        for (const o of orders) loaded.push({ ...o, group: gi, account: g.names.get(String(o.campaignId)) || `магазин ${o.campaignId}` });
      } catch (e) {
        if (!(e instanceof MarketError)) throw e;
        problems.push(`${names}: ${e.message}`);
      }
    }));
    // Страховка: если Маркет вернул заказы вне выбранных дат, они не попадут ни в список, ни в ярлыки.
    const orders = loaded.filter((o) => !q.from || (o.shipmentDate >= q.from && o.shipmentDate <= q.to))
      .sort((a, b) => a.shipmentDate.localeCompare(b.shipmentDate) || a.id - b.id);

    const [locations, products] = await Promise.all([loadLocations(), loadProductInfo()]);
    const rid = crypto.randomBytes(12).toString('hex');
    const publicGroups = groups.map((g) => ({ names: [...g.names.values()] }));
    cache.set(rid, { userId: req.ctx.user.id, orders, locations, dateLabel: q.from ? (q.from === q.to ? q.from : `${q.from}_${q.to}`) : '', groups: groups.map((g) => ({ apiKey: g.apiKey, businessId: g.businessId, names: [...g.names.values()] })), at: Date.now() });

    const itemsTotal = orders.reduce((n, o) => n + o.items.reduce((m, i) => m + i.count, 0), 0);
    const rows = orders.flatMap((o) => o.items.map((i) => ({ o, i })));
    const labelButtons = groups.map((g, gi) => {
      const ids = orders.filter((o) => o.group === gi).length;
      const parts = Math.ceil(ids / MAX_BATCH);
      return Array.from({ length: parts }, (_, p) => html`<form method="post" action="/orders/labels" class="inline labels-form">${csrfField(req.ctx)}
        <input type="hidden" name="r" value="${rid}"><input type="hidden" name="g" value="${gi}"><input type="hidden" name="part" value="${p + 1}">
        <input type="hidden" name="format" class="fmt-copy" value="A7">
        <button>Ярлыки (PDF): ${publicGroups[gi].names.join(', ')} — ${Math.min(MAX_BATCH, ids - p * MAX_BATCH)} заказов${parts > 1 ? `, часть ${p + 1}` : ''}</button></form>`);
    });
    send(req, res, {
      title: 'Заказы Маркета', active: 'orders', scripts: ['/static/orders.js'],
      body: html`${form(req, q, accounts)}
      ${problems.map((p) => html`<div class="flash err">Не удалось загрузить: ${p}</div>`)}
      <p>Заказов: <b>${orders.length}</b>, товаров: <b>${itemsTotal}</b> шт${q.from ? html`. Отгрузка ${q.from === q.to ? humanShip(q.from) : `${humanShip(q.from)} — ${humanShip(q.to)}`}` : ''}</p>
      ${orders.length ? html`<div class="card">
        <div class="toolbar"><a class="button" href="/orders/export.xlsx?r=${rid}">Сохранить список в Excel</a>
          <label>Формат ярлыков: <select id="label-format">${FORMATS.map(([v, l]) => html`<option value="${v}">${l}</option>`)}</select></label></div>
        <div class="toolbar">${labelButtons}</div>
        <p id="labels-status" class="muted" hidden></p>
      </div>` : ''}
      <div class="table-wrap"><table>
        <thead><tr><th></th><th>Кабинет</th><th>№ заказа</th><th>Статус</th><th>Отгрузка</th><th>Артикул</th><th>Товар</th><th class="num">Шт</th><th>Где лежит у нас</th></tr></thead>
        <tbody>${rows.map(({ o, i }) => {
          const p = products.get(i.offerId);
          const where = locations.get(i.offerId);
          return html`<tr><td>${p ? html`<a href="/products/${p.id}">${thumbImg(p.id, p.cover_id)}</a>` : ''}</td><td>${o.account}</td><td>${o.id}</td>
            <td>${humanStatus(o.status, o.substatus)}</td><td>${humanShip(o.shipmentDate)}</td><td>${i.offerId}</td><td>${i.name}</td>
            <td class="num">${i.count}</td><td class="${where ? '' : 'neg'}">${where || NO_STOCK}</td></tr>`;
        })}
          ${rows.length ? '' : html`<tr><td colspan="9" class="muted">Заказов нет</td></tr>`}</tbody>
      </table></div>`,
    });
  });

  // ---- Excel: список заказов + сборочный лист (как в приложении)
  app.get('/orders/export.xlsx', requireLogin, requireEditor, (req, res) => {
    const e = taken(req, req.query.r);
    if (!e) { res.flash('err', 'Список устарел - загрузите заказы заново'); return res.redirect('/orders'); }
    const list = [['Кабинет', '№ заказа', 'Статус', 'Оформлен', 'Отгрузка', 'Служба доставки', 'Артикул', 'Товар', 'Кол-во', 'Где лежит у нас']];
    const picks = new Map();
    for (const o of e.orders) {
      for (const i of o.items) {
        list.push([o.account, String(o.id), humanStatus(o.status, o.substatus), o.creationDate ? fmtDate(o.creationDate) : '',
          humanShip(o.shipmentDate), o.deliveryService, i.offerId, i.name, String(i.count), e.locations.get(i.offerId) || '']);
        const p = picks.get(i.offerId) || { name: i.name, total: 0, orders: new Set(), accounts: new Set() };
        p.total += i.count; p.orders.add(o.id); p.accounts.add(o.account); picks.set(i.offerId, p);
      }
    }
    const picking = [['Артикул', 'Товар', 'Всего, шт', 'Заказов', 'Где лежит у нас', 'Кабинеты']];
    for (const [offer, p] of [...picks].sort((a, b) => a[0].localeCompare(b[0]))) {
      picking.push([offer, p.name, String(p.total), String(p.orders.size), e.locations.get(offer) || NO_STOCK, [...p.accounts].sort().join(', ')]);
    }
    const buf = xlsx.write([
      { name: 'Заказы', rows: list, numericColumns: [1, 8], columnWidths: [20, 12, 16, 16, 11, 20, 20, 40, 8, 40] },
      { name: 'Сборка', rows: picking, numericColumns: [2, 3], columnWidths: [20, 40, 10, 9, 45, 30] },
    ]);
    const stamp = new Date().toISOString().slice(0, 16).replace(/[T:]/g, '-');
    res.set({
      'Content-Type': 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
      'Content-Disposition': `attachment; filename="orders_${stamp}.xlsx"; filename*=UTF-8''${encodeURIComponent(`Заказы_Маркет_${stamp}.xlsx`)}`,
    });
    res.send(buf);
  });

  // ---- Ярлыки: один PDF на кабинет (ключ + бизнес) и не больше 1000 заказов в файле
  app.post('/orders/labels', requireLogin, requireEditor, async (req, res) => {
    const wantsJson = req.get('x-requested-with') === 'fetch';
    const fail = (status, msg) => (wantsJson ? res.status(status).json({ error: msg }) : (res.flash('err', msg), res.redirect('/orders')));
    const e = taken(req, req.body.r);
    if (!e) return fail(410, 'Список устарел - загрузите заказы заново');
    const gi = Number(req.body.g);
    const group = e.groups[gi];
    const part = Math.max(1, parseInt(req.body.part, 10) || 1);
    const format = FORMATS.some(([v]) => v === req.body.format) ? req.body.format : 'A7';
    if (!group) return fail(400, 'Неизвестный кабинет');
    const ids = e.orders.filter((o) => o.group === gi).map((o) => o.id).slice((part - 1) * MAX_BATCH, part * MAX_BATCH);
    if (!ids.length) return fail(400, 'Нет заказов для ярлыков');
    try {
      const api = new MarketApi(group.apiKey, { log: (m) => console.error(`[market] ${group.names.join(', ')}: ${m}`) });
      const { pdf, warning } = await api.orderLabels(group.businessId, ids, format);
      const stamp = new Date().toISOString().slice(0, 16).replace(/[T:]/g, '-');
      const partSuffix = Math.ceil(e.orders.filter((o) => o.group === gi).length / MAX_BATCH) > 1 ? `_часть${part}` : '';
      const name = `Ярлыки_${safeFileName(group.names.join('+'))}_${e.dateLabel ? `отгрузка_${e.dateLabel}` : stamp}${partSuffix}.pdf`;
      res.set({
        'Content-Type': 'application/pdf',
        'Content-Disposition': `attachment; filename="labels.pdf"; filename*=UTF-8''${encodeURIComponent(name)}`,
        'X-Labels-Warning': encodeURIComponent(warning),
        'Cache-Control': 'no-store',
      });
      res.send(pdf);
    } catch (err) {
      if (!(err instanceof MarketError)) throw err;
      return fail(502, `${group.names.join(', ')}: ${err.message}`);
    }
  });
};

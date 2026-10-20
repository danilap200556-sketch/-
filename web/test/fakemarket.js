'use strict';
// Поддельный сервер Маркета для тестов: те же адреса и формы ответов, что в спецификации API.
const http = require('http');
const { addDays } = require('../src/market');

const KEY = 'test-key';
const mk = (orderId, campaignId, status, substatus, items, ship) => ({
  orderId, campaignId, status, substatus, creationDate: '2026-09-23T10:15:00+03:00',
  items: items.map(([offerId, offerName, count], i) => ({ id: i + 1, offerId, offerName, count })),
  delivery: { serviceName: 'Маркет Курьер', shipment: { id: 1, shipmentDate: ship } },
});
const ORDERS = [
  mk(1001, 222, 'PROCESSING', 'STARTED', [['NK-AF1-42', 'Кроссовки Nike AF1 42', 1], ['MKT-NB', 'NB 373 43', 2]], '2026-09-25'),
  mk(1002, 222, 'PROCESSING', 'READY_TO_SHIP', [['NK-AF1-42', 'Кроссовки Nike AF1 42', 2]], '2026-09-25'),
  mk(1003, 224, 'PROCESSING', 'STARTED', [['UNKNOWN-SKU', 'Чего-то нет у нас', 1]], '2026-09-26'),
  mk(2001, 225, 'PROCESSING', 'STARTED', [['NK-AF1-42', 'Кроссовки Nike AF1 42', 3]], '2026-09-25'),
  mk(3001, 222, 'PROCESSING', 'STARTED', [['NK-AF1-42', 'Кроссовки Nike AF1 42', 1]], '2026-09-27'),
  mk(3003, 222, 'DELIVERY', 'DELIVERY_SERVICE_RECEIVED', [['NK-AF1-42', 'Кроссовки Nike AF1 42', 1]], '2026-09-25'),
  ...Array.from({ length: 1100 }, (_, i) => mk(7000 + i, 226, 'PROCESSING', 'STARTED', [[`BULK-${i}`, `Bulk ${i}`, 1]], '2026-09-30')),
];
const BUSINESS = { 222: 111, 224: 111, 225: 112, 226: 113 };

function startFakeMarket() {
  const state = { requests: [], files: [], reports: new Map(), failLabels: null };
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://x');
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      const body = chunks.length ? JSON.parse(Buffer.concat(chunks).toString()) : null;
      const send = (code, obj) => { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); };
      if (url.pathname.startsWith('/files/')) {
        state.files.push({ path: url.pathname, apiKey: req.headers['api-key'] ?? null, host: req.headers.host });
        const r = state.reports.get(url.pathname.split('/')[2].replace('.pdf', ''));
        res.writeHead(200, { 'Content-Type': 'application/pdf' });
        return res.end(`%PDF-1.4\n% format=${r.format} orders=${r.ids.join(',')}\n`);
      }
      state.requests.push({ method: req.method, path: url.pathname, query: Object.fromEntries(url.searchParams), body, apiKey: req.headers['api-key'] });
      if (req.headers['api-key'] !== KEY) return send(401, { status: 'ERROR', errors: [{ code: 'UNAUTHORIZED', message: 'bad key' }] });
      let m;
      if ((m = /^\/v1\/businesses\/(\d+)\/orders$/.exec(url.pathname)) && req.method === 'POST') {
        const biz = Number(m[1]);
        const d = body.dates;
        let hi = d?.shipmentDateTo;
        if (d && (!hi || (new Date(hi) - new Date(d.shipmentDateFrom)) < 86_400_000)) hi = addDays(d.shipmentDateFrom, 1);
        const list = ORDERS.filter((o) => BUSINESS[o.campaignId] === biz
          && (!body.campaignIds || body.campaignIds.includes(o.campaignId))
          && (!body.statuses || body.statuses.includes(o.status))
          && (!body.substatuses || body.substatuses.includes(o.substatus))
          && (!d || (o.delivery.shipment.shipmentDate >= d.shipmentDateFrom && o.delivery.shipment.shipmentDate < hi)));
        const start = Number(url.searchParams.get('page_token') || 0);
        const limit = Math.min(Number(url.searchParams.get('limit') || 50), 50);
        const chunk = list.slice(start, start + limit);
        return send(200, { orders: chunk, paging: start + limit < list.length ? { nextPageToken: String(start + limit) } : {} });
      }
      if (url.pathname === '/v2/reports/documents/labels/generate') {
        if (state.failLabels === 'NO_DATA') { const id = `r${state.reports.size + 1}`; state.reports.set(id, { fail: 'NO_DATA', polls: 0 }); return send(200, { status: 'OK', result: { reportId: id, estimatedGenerationTime: 1 } }); }
        const id = `r${state.reports.size + 1}`;
        state.reports.set(id, { ids: body.orderIds, format: url.searchParams.get('format'), polls: 0 });
        return send(200, { status: 'OK', result: { reportId: id, estimatedGenerationTime: 1 } });
      }
      if ((m = /^\/v2\/reports\/info\/(.+)$/.exec(url.pathname))) {
        const r = state.reports.get(m[1]);
        if (++r.polls < 2) return send(200, { status: 'OK', result: { status: 'PROCESSING' } });
        if (r.fail) return send(200, { status: 'OK', result: { status: 'FAILED', subStatus: r.fail } });
        // ссылка на файл ведёт на другое имя хоста (localhost), чем адрес API (127.0.0.1)
        return send(200, { status: 'OK', result: { status: 'DONE', file: `http://localhost:${server.address().port}/files/${m[1]}.pdf` } });
      }
      return send(404, { status: 'ERROR', errors: [{ code: 'NOT_FOUND', message: url.pathname }] });
    });
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve({ server, state, url: `http://127.0.0.1:${server.address().port}` })));
}

module.exports = { startFakeMarket, ORDERS, KEY, BUSINESS, mk };

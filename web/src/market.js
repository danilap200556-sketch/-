'use strict';
// Клиент API Яндекс Маркета для продавцов (только заказы и ярлыки) - то же, что MarketApi в
// приложении (src/marketapi.cpp). Схемы запросов - по официальной спецификации
// github.com/yandex-market/yandex-market-partner-api. Ключ кабинета уходит только на сам API.

const DEFAULT_BASE = 'https://api.partner.market.yandex.ru';
const TIMEOUT_MS = 60_000;
const MAX_PAGES = 2000;
const MAX_PDF_BYTES = 80 * 1024 * 1024;

const baseUrl = () => (process.env.MARKET_API_BASE_URL || DEFAULT_BASE).replace(/\/+$/, '');

// Сообщение понятно человеку и не содержит секретов - его можно показывать на странице.
class MarketError extends Error {}

function describeStatus(status) {
  switch (status) {
    case 400: return 'запрос содержит неправильные данные';
    case 401: return 'неверный или отозванный API-ключ';
    case 403: return 'у API-ключа нет доступа к этому кабинету или методу (проверьте доступы ключа в настройках кабинета)';
    case 404: return 'кабинет, магазин или склад не найден - проверьте ID';
    case 420: return 'превышен лимит запросов, попробуйте через минуту';
    case 423: return 'метод временно заблокирован для этого кабинета';
    default: return status >= 500 ? 'ошибка на стороне Маркета, попробуйте позже' : '';
  }
}

const errorsFromBody = (json) =>
  (Array.isArray(json?.errors) ? json.errors : []).map((e) => (e.message ? `${e.code}: ${e.message}` : e.code)).join('; ');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

class MarketApi {
  constructor(apiKey, { log = () => {}, pollMs = 2000, maxInitialWaitMs = 10_000 } = {}) {
    this.apiKey = apiKey;
    this.log = log;
    this.pollMs = pollMs;
    this.maxInitialWaitMs = maxInitialWaitMs;
  }

  async request(method, path, { query, body } = {}) {
    const url = new URL(baseUrl() + path);
    for (const [k, v] of Object.entries(query || {})) if (v !== undefined && v !== '') url.searchParams.set(k, v);
    let res; let text;
    try {
      res = await fetch(url, {
        method,
        headers: { 'Api-Key': this.apiKey, Accept: 'application/json', ...(body ? { 'Content-Type': 'application/json' } : {}) },
        body: body ? JSON.stringify(body) : undefined,
        signal: AbortSignal.timeout(TIMEOUT_MS),
      });
      text = await res.text();
    } catch (e) {
      throw new MarketError(`нет связи с Маркетом: ${e.name === 'TimeoutError' ? 'время ожидания вышло' : (e.cause?.code || e.message)}`);
    }
    let json = null;
    try { json = JSON.parse(text); } catch { /* не JSON */ }
    if (res.status >= 400 || json?.status === 'ERROR') {
      const parts = [`HTTP ${res.status}`];
      const human = describeStatus(res.status);
      if (human) parts.push(human);
      const details = errorsFromBody(json);
      if (details) parts.push(details);
      else if (json === null && text) parts.push(text.slice(0, 300));
      const err = parts.join(' - ');
      this.log(`${method} ${path}: ${err}`);
      throw new MarketError(err);
    }
    return json ?? {};
  }

  // Заказы магазинов кабинета. Даты отгрузки: shipmentFrom/shipmentTo - 'ГГГГ-ММ-ДД', обе включительно.
  async orders(businessId, campaignIds, { statuses = [], substatuses = [], shipmentFrom = '', shipmentTo = '' } = {}) {
    const out = [];
    const body = { fake: false };
    if (statuses.length) body.statuses = statuses;
    if (substatuses.length) body.substatuses = substatuses;
    if (shipmentFrom) {
      // В API конечная дата не входит в интервал, поэтому "по" - это следующий день.
      body.dates = { shipmentDateFrom: shipmentFrom, shipmentDateTo: addDays(shipmentTo || shipmentFrom, 1) };
    }
    for (let start = 0; start < Math.max(campaignIds.length, 1); start += 50) {
      const ids = campaignIds.slice(start, start + 50);
      const req = { ...body, ...(ids.length ? { campaignIds: ids } : {}) };
      let token = '';
      for (let page = 0; page < MAX_PAGES; page++) {
        const root = await this.request('POST', `/v1/businesses/${businessId}/orders`, {
          query: { limit: '50', page_token: token }, body: req,
        });
        for (const o of root.orders || []) {
          out.push({
            id: o.orderId, campaignId: o.campaignId, status: o.status || '', substatus: o.substatus || '',
            creationDate: o.creationDate || '', deliveryService: o.delivery?.serviceName || '',
            shipmentDate: o.delivery?.shipment?.shipmentDate || '',
            items: (o.items || []).map((i) => ({ offerId: i.offerId, name: i.offerName || '', count: i.count || 0 })),
          });
        }
        token = root.paging?.nextPageToken || '';
        if (!token) break;
      }
    }
    return out;
  }

  // Один PDF с ярлыками на все коробки заказов одного кабинета. Маркет готовит файл асинхронно -
  // ждём до 5 минут. -> { pdf: Buffer, warning }
  async orderLabels(businessId, orderIds, format) {
    const gen = await this.request('POST', '/v2/reports/documents/labels/generate', {
      query: { format }, body: { businessId: Number(businessId), orderIds, sortingType: 'SORT_BY_GIVEN_ORDER' }, // businessId в API - число
    });
    const reportId = gen.result?.reportId;
    if (!reportId) throw new MarketError('Маркет не вернул идентификатор файла с ярлыками');
    this.log(`Маркет готовит файл с ярлыками (${orderIds.length} заказов)...`);
    const estimated = Number(gen.result?.estimatedGenerationTime) || 0;
    await sleep(Math.min(Math.max(estimated, 500), this.maxInitialWaitMs));
    const started = Date.now();
    for (;;) {
      const info = (await this.request('GET', `/v2/reports/info/${encodeURIComponent(reportId)}`)).result || {};
      const { status, subStatus } = info;
      if (status === 'DONE') {
        const warning = subStatus === 'RESOURCE_NOT_FOUND' ? 'часть заказов Маркет не нашёл - ярлыков для них в файле нет' : '';
        if (!info.file) {
          throw new MarketError(subStatus === 'NO_DATA'
            ? 'для этих заказов ярлыков нет (заказы не в статусе сборки?)' : 'Маркет не вернул ссылку на файл');
        }
        return { pdf: await this.download(info.file), warning };
      }
      if (status === 'FAILED') {
        throw new MarketError(subStatus === 'NO_DATA' ? 'для этих заказов ярлыков нет (заказы не в статусе сборки?)'
          : subStatus === 'TOO_LARGE' ? 'слишком много заказов для одного файла'
            : `Маркет не смог подготовить файл с ярлыками${subStatus ? ` (${subStatus})` : ''}`);
      }
      if (Date.now() - started > 5 * 60_000) throw new MarketError('Маркет готовит файл дольше 5 минут - попробуйте позже');
      await sleep(this.pollMs);
    }
  }

  // Ключ отправляем только самому API: ссылка на готовый файл может вести на стороннее хранилище.
  // Редиректы идут вручную, чтобы заголовок с ключом не уехал на чужой хост.
  async download(fileUrl) {
    const apiHost = new URL(baseUrl()).host;
    let url = new URL(fileUrl);
    for (let hop = 0; hop < 5; hop++) {
      // Ссылки на файлы - только https (http допустим лишь при тестовом адресе API без шифрования).
      if (url.protocol !== 'https:' && !baseUrl().startsWith('http:')) throw new MarketError('не удалось скачать файл: небезопасная ссылка');
      let res;
      try {
        res = await fetch(url, {
          headers: url.host === apiHost ? { 'Api-Key': this.apiKey } : {},
          redirect: 'manual', signal: AbortSignal.timeout(TIMEOUT_MS),
        });
      } catch (e) {
        throw new MarketError(`не удалось скачать файл: ${e.cause?.code || e.message}`);
      }
      if (res.status >= 300 && res.status < 400 && res.headers.get('location')) {
        url = new URL(res.headers.get('location'), url);
        continue;
      }
      if (res.status >= 400) throw new MarketError(`не удалось скачать файл: HTTP ${res.status}`);
      if (Number(res.headers.get('content-length')) > MAX_PDF_BYTES) throw new MarketError('файл с ярлыками слишком большой');
      const buf = Buffer.from(await res.arrayBuffer());
      if (buf.length > MAX_PDF_BYTES) throw new MarketError('файл с ярлыками слишком большой');
      return buf;
    }
    throw new MarketError('не удалось скачать файл: слишком много перенаправлений');
  }
}

// 'ГГГГ-ММ-ДД' + n дней (по календарю, без часовых поясов)
function addDays(iso, n) {
  const d = new Date(`${iso}T00:00:00Z`);
  d.setUTCDate(d.getUTCDate() + n);
  return d.toISOString().slice(0, 10);
}

module.exports = { MarketApi, MarketError, addDays, baseUrl };

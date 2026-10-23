'use strict';
const bc = require('./barcode');
const { UserError } = require('./stock');
const mi = require('./marketinfo');

// Обновление данных товаров из Excel/CSV: название, описание, цена, артикул на Маркете, свой код, штрихкоды и параметры
// для карточки Яндекс Маркета (бренд, страна, габариты и вес, ТН ВЭД, теги, 18+).
// Остатки не затрагиваются. Те же правила и тексты, что у диалога «Обновить товары из Excel» в приложении
// (src/productsync.cpp). Шаги: plan() - что изменится (ничего не пишет), apply() - записать одной транзакцией.

const ROLES = {
  ignore: 0, key: 1, barcode: 2, name: 3, description: 4, price: 5, market_sku: 6, custom_code: 7,
  vendor: 8, countries: 9, length: 10, width: 11, height: 12, weight: 13, commodity: 14, tags: 15, adult: 16,
};
// роль колонки -> поле параметров для Маркета (то же, что в marketinfo.FIELDS и в приложении)
const YM_ROLE = new Map([[ROLES.vendor, 'ym_vendor'], [ROLES.countries, 'ym_countries'], [ROLES.length, 'ym_length'], [ROLES.width, 'ym_width'],
  [ROLES.height, 'ym_height'], [ROLES.weight, 'ym_weight'], [ROLES.commodity, 'ym_commodity'], [ROLES.tags, 'ym_tags'], [ROLES.adult, 'ym_adult']]);
const ROLE_LABELS = ['— пропустить —', 'Артикул (по нему ищем товар)', 'Штрихкод (тоже ищет товар; новый код привяжется)',
  'Название', 'Описание', 'Цена', 'Артикул на Маркете', 'Свой код (для QR-этикеток)', 'Бренд (для карточки Маркета)',
  'Страна производства (через «;»)', 'Длина упаковки, см', 'Ширина упаковки, см', 'Высота упаковки, см', 'Вес упаковки, кг', 'ТН ВЭД',
  'Теги (через «;»)', 'Для взрослых 18+ (да/нет)'];
const FIELD = {
  name: 'Название', description: 'Описание', price: 'Цена', market_sku: 'Артикул на Маркете', custom_code: 'Свой код', barcodes: 'Штрихкоды',
  ...Object.fromEntries(mi.FIELDS.map((f) => [f.field, f.label])),
};
const LIMITS = { name: 300, description: 2000, market_sku: 200, custom_code: 200 };
const isYm = (field) => field.startsWith('ym_');
const MAX_ROWS = 20_000;
const MAX_PRICE = 10_000_000;
const CLEARABLE = ['description', 'market_sku', 'custom_code'];

// Роль колонки по её заголовку. Столбцы остатков из выгрузки товаров («Остаток: Склад А», «Всего на складах»,
// «Места хранения», «Фото, шт», «Добавлен») пропускаются.
function guessRole(header) {
  const h = String(header ?? '').toLowerCase().trim();
  if (!h) return ROLES.ignore;
  if (h.startsWith('остаток') || h.startsWith('всего') || h.startsWith('места') || h.startsWith('фото') || h.startsWith('добавлен') || h.startsWith('категори')) return ROLES.ignore;
  if (h.includes('штрих') || h.includes('ean') || h.includes('barcode')) return ROLES.barcode;
  if (h.includes('на маркет') || h.includes('market') || h.includes('offer')) return ROLES.market_sku;
  if (h.includes('свой код') || h.includes('custom') || h.includes('qr')) return ROLES.custom_code;
  if (h.includes('бренд') || h.includes('brand') || h.includes('vendor')) return ROLES.vendor;
  if (h.includes('страна') || h.includes('country')) return ROLES.countries;
  if (h.includes('длина') || h.includes('length')) return ROLES.length;
  if (h.includes('ширина') || h.includes('width')) return ROLES.width;
  if (h.includes('высота') || h.includes('height')) return ROLES.height;
  if (h.startsWith('вес') || h.includes('weight')) return ROLES.weight;
  if (h.includes('тн вэд') || h.includes('твэд') || h.includes('tnved') || h.includes('commodity')) return ROLES.commodity;
  if (h === 'теги' || h === 'tags' || h === 'тег') return ROLES.tags;
  if (h === '18+' || h.includes('взросл') || h.includes('adult')) return ROLES.adult;
  if (h.includes('артикул') || h.includes('sku') || h.includes('ключ')) return ROLES.key;
  if (h.includes('наимен') || h.includes('назв') || h === 'name') return ROLES.name;
  if (h.includes('описан') || h.includes('descr')) return ROLES.description;
  if (h.includes('цена') || h.includes('price') || h.includes('стоим')) return ROLES.price;
  return ROLES.ignore;
}

// Роли для всех колонок строки заголовков; одну роль получает только первая подходящая колонка.
function guessRoles(header, width = header.length) {
  const used = new Set();
  return Array.from({ length: width }, (_, i) => {
    const r = guessRole(header[i]);
    if (r === ROLES.ignore || used.has(r)) return ROLES.ignore;
    used.add(r);
    return r;
  });
}

function guessHeaderRow(rows) {
  let best = 0; let bestScore = 0;
  for (let r = 0; r < Math.min(rows.length, 30); r++) {
    const score = rows[r].filter((c) => guessRole(c) !== ROLES.ignore).length;
    if (score > bestScore) { bestScore = score; best = r; }
  }
  return bestScore >= 2 ? best : 0;
}

// Цена из ячейки: «12 990,5», «12990.5», «1,234.50», «500 руб.», «₽ 500» -> число с двумя знаками; иначе null.
function parsePriceCell(raw) {
  let s = String(raw ?? '').replace(/\s/g, '').replace(/^₽/, '').replace(/(₽|руб\.?|р\.?|rub)$/i, '');
  if (s.includes(',') && s.includes('.')) {
    const sep = Math.max(s.lastIndexOf(','), s.lastIndexOf('.'));
    s = s.slice(0, sep).replace(/[.,]/g, '') + '.' + s.slice(sep + 1);
  } else {
    s = s.replace(',', '.');
  }
  if (!/^\d+(\.\d+)?$/.test(s)) return null;
  const n = Math.round(Number(s) * 100) / 100;
  return n <= MAX_PRICE ? n : null;
}

const cleanText = (v) => String(v ?? '').replace(/\r\n?/g, '\n').replace(/\u00a0/g, ' ').trim();
const fmtPrice = (n) => String(Math.round((Number(n) || 0) * 100) / 100);

// Что изменится. opts: { rows, headerIdx, roles, createMissing, clearEmpty }
// -> { fatal, items, stats, problems }; items: [{ n, status: update|same|create|missing|error, id, sku, name, changes: [{field, label, from, to}], newCodes, create, message }]
async function plan(q, opts) {
  const { rows, headerIdx, roles, createMissing = false, clearEmpty = false } = opts;
  const out = { fatal: null, items: [], stats: { rows: 0, update: 0, same: 0, create: 0, missing: 0, error: 0 }, problems: [] };
  const ymOn = await mi.supported(q);
  // параметры для Маркета: пока приложение не создало столбцы в базе, такие колонки файла пропускаются
  const col = (role) => (YM_ROLE.has(role) && !ymOn ? -1 : roles.indexOf(role));
  const seen = new Set();
  for (const r of roles) { if (r !== ROLES.ignore) { if (seen.has(r)) { out.fatal = `Роль «${ROLE_LABELS[r]}» назначена нескольким колонкам - оставьте её одной колонке`; return out; } seen.add(r); } }
  const keyCol = col(ROLES.key); const codeCol = col(ROLES.barcode);
  if (keyCol < 0 && codeCol < 0) { out.fatal = 'Укажите колонку «Артикул» (или «Штрихкод»): по ней ищется товар'; return out; }
  const dataRoles = [ROLES.name, ROLES.description, ROLES.price, ROLES.market_sku, ROLES.custom_code];
  if (!dataRoles.some((r) => col(r) >= 0) && ![...YM_ROLE.keys()].some((r) => col(r) >= 0) && codeCol < 0) {
    out.fatal = 'Не выбрано, что обновлять: укажите хотя бы одну колонку - название, описание, цену, артикул на Маркете, свой код, штрихкод или параметры для Маркета (бренд, габариты и т. д.)';
    return out;
  }
  const body = rows.slice(headerIdx + 1);
  if (body.length > MAX_ROWS + 5000) { out.fatal = `Слишком много строк (больше ${MAX_ROWS}) - разбейте файл на части`; return out; }

  // строки файла
  const lines = [];
  body.forEach((row, i) => {
    const n = headerIdx + 2 + i;
    const key = cleanText(keyCol >= 0 ? row[keyCol] : '');
    const codes = codeCol >= 0 ? String(row[codeCol] ?? '').split(/[,;\s]+/).filter(Boolean).map(bc.normalize) : [];
    if (!key && !codes.length) return;
    if (key.toLowerCase().startsWith('итого')) return;
    lines.push({ n, row, key, codes });
  });
  if (lines.length > MAX_ROWS) { out.fatal = `Слишком много строк (${lines.length}, больше ${MAX_ROWS}) - разбейте файл на части`; return out; }

  // товары по артикулам и штрихкодам - несколькими запросами на весь файл
  const keys = [...new Set(lines.map((l) => l.key).filter(Boolean))];
  const codes = [...new Set(lines.flatMap((l) => l.codes))];
  const bySku = new Map(); const byCode = new Map(); const ownCodes = new Map();
  const PRODUCT_COLS = `p.id, p.sku, p.name, COALESCE(p.description, '') AS description, p.price::float8 AS price, COALESCE(p.market_sku, '') AS market_sku, COALESCE(p.custom_code, '') AS custom_code${ymOn ? `, ${mi.SELECT_SQL}` : ''}`;
  const products = new Map();
  for (let i = 0; i < keys.length; i += 1000) {
    const r = await q.query(`SELECT k.key, ${PRODUCT_COLS} FROM unnest($1::text[]) AS k(key) JOIN products p ON lower(p.sku) = lower(k.key)`, [keys.slice(i, i + 1000)]);
    for (const x of r.rows) { products.set(x.id, x); if (!bySku.has(x.key)) bySku.set(x.key, []); bySku.get(x.key).push(x.id); }
  }
  for (let i = 0; i < codes.length; i += 1000) {
    const r = await q.query(`SELECT b.barcode, ${PRODUCT_COLS} FROM product_barcodes b JOIN products p ON p.id = b.product_id WHERE b.barcode = ANY($1::text[])`, [codes.slice(i, i + 1000)]);
    for (const x of r.rows) { products.set(x.id, x); byCode.set(x.barcode, x.id); }
  }
  const ids = [...products.keys()];
  for (let i = 0; i < ids.length; i += 5000) {
    const r = await q.query('SELECT product_id, barcode FROM product_barcodes WHERE product_id = ANY($1::int[])', [ids.slice(i, i + 5000)]);
    for (const x of r.rows) { if (!ownCodes.has(x.product_id)) ownCodes.set(x.product_id, new Set()); ownCodes.get(x.product_id).add(x.barcode); }
  }

  const doneProduct = new Map(); // id -> строка файла
  const claimed = new Map(); // новый штрихкод -> строка файла
  const newSkus = new Map(); // создаваемый артикул (в нижнем регистре) -> строка
  const fail = (item, message) => { item.status = 'error'; item.message = message; return item; };

  for (const l of lines) {
    const item = { n: l.n, status: 'same', id: null, sku: l.key, name: '', changes: [], newCodes: [], create: null, byCode: false, message: '' };
    out.items.push(item);
    const label = l.key || l.codes.join(', ');

    // 1) какой товар
    let pid = null;
    if (l.key) {
      const hits = bySku.get(l.key) || [];
      const exact = hits.filter((id) => products.get(id).sku === l.key);
      if (exact.length === 1) pid = exact[0];
      else if (hits.length === 1) pid = hits[0];
      else if (hits.length > 1) { fail(item, `Строка ${l.n}: под артикулом «${l.key}» подходит несколько товаров - впишите артикул точно, как в базе`); continue; }
    }
    const codeOwners = [...new Set(l.codes.map((c) => byCode.get(c)).filter((x) => x !== undefined))];
    if (pid === null && codeOwners.length === 1) { pid = codeOwners[0]; item.byCode = true; } // артикула нет или он не найден - товар по штрихкоду
    else if (codeOwners.length > 1 && pid === null) { fail(item, `Строка ${l.n}: штрихкоды из этой строки принадлежат разным товарам`); continue; }
    else if (pid !== null && codeOwners.some((o) => o !== pid)) { fail(item, `Строка ${l.n}: штрихкод из этой строки уже привязан к другому товару (не к «${products.get(pid).sku}»)`); continue; }

    // 2) новые значения
    const want = {}; let problem = '';
    const text = (role, field) => {
      const c = col(role); if (c < 0) return;
      const v = cleanText(l.row[c]);
      if (v === '') { if (clearEmpty && CLEARABLE.includes(field)) want[field] = ''; return; }
      if (v.length > LIMITS[field]) { problem = `Строка ${l.n}: «${FIELD[field]}» длиннее ${LIMITS[field]} знаков`; return; }
      want[field] = v;
    };
    text(ROLES.name, 'name'); text(ROLES.description, 'description'); text(ROLES.market_sku, 'market_sku'); text(ROLES.custom_code, 'custom_code');
    if (col(ROLES.price) >= 0) {
      const raw = cleanText(l.row[col(ROLES.price)]);
      if (raw !== '') {
        const p = parsePriceCell(raw);
        if (p === null) problem = problem || `Строка ${l.n}: цена «${raw}» - нужно число от 0 до 10 000 000, до двух знаков после запятой`;
        else want.price = p;
      }
    }
    const wantYm = {};
    for (const [role, field] of YM_ROLE) {
      const c = col(role); if (c < 0) continue;
      const raw = cleanText(l.row[c]); if (raw === '') continue;
      const r = mi.parse(mi.BY_FIELD.get(field), raw);
      if (r.why) { problem = problem || `Строка ${l.n}: «${FIELD[field]}» - ${r.why}`; continue; }
      wantYm[field] = r.value;
    }
    if (problem) { fail(item, problem); continue; }
    for (const c of l.codes) {
      const bad = bc.validate(c);
      if (bad) { problem = `Строка ${l.n}: штрихкод ${c}: ${bad}`; break; }
    }
    if (problem) { fail(item, problem); continue; }

    // 3) найден - сравниваем; не найден - создаём или пропускаем
    if (pid !== null) {
      const cur = products.get(pid);
      item.id = pid; item.sku = cur.sku; item.name = cur.name;
      if (doneProduct.has(pid)) { fail(item, `Строка ${l.n}: товар «${cur.sku}» уже был в строке ${doneProduct.get(pid)} - повтор пропущен`); continue; }
      const own = ownCodes.get(pid) || new Set();
      const add = l.codes.filter((c) => !own.has(c));
      const clash = add.find((c) => claimed.has(c));
      if (clash) { fail(item, `Строка ${l.n}: штрихкод ${clash} уже указан в строке ${claimed.get(clash)}`); continue; }
      for (const f of ['name', 'description', 'market_sku', 'custom_code']) {
        if (f in want && want[f] !== cleanText(cur[f])) item.changes.push({ field: f, label: FIELD[f], from: cur[f] ?? '', to: want[f] });
      }
      if ('price' in want && Math.abs(want.price - Number(cur.price)) >= 0.005) item.changes.push({ field: 'price', label: FIELD.price, from: fmtPrice(cur.price), to: fmtPrice(want.price) });
      for (const f of mi.FIELDS) {
        if (!(f.field in wantYm)) continue;
        const now = mi.shown(f, cur);
        if (wantYm[f.field] !== now) item.changes.push({ field: f.field, label: FIELD[f.field], from: now, to: wantYm[f.field] });
      }
      if (add.length) { item.newCodes = [...new Set(add)]; item.changes.push({ field: 'barcodes', label: FIELD.barcodes, from: '', to: item.newCodes.join(', ') }); item.newCodes.forEach((c) => claimed.set(c, l.n)); }
      doneProduct.set(pid, l.n);
      item.status = item.changes.length ? 'update' : 'same';
    } else if (createMissing && l.key) {
      if (newSkus.has(l.key.toLowerCase())) { fail(item, `Строка ${l.n}: товар «${l.key}» уже был в строке ${newSkus.get(l.key.toLowerCase())} - повтор пропущен`); continue; }
      if (l.key.length > 100) { fail(item, `Строка ${l.n}: артикул длиннее 100 знаков`); continue; }
      const add = [...new Set(l.codes)];
      const clash = add.find((c) => claimed.has(c));
      if (clash) { fail(item, `Строка ${l.n}: штрихкод ${clash} уже указан в строке ${claimed.get(clash)}`); continue; }
      item.create = { sku: l.key, name: want.name || l.key, description: want.description ?? '', price: want.price ?? 0, market_sku: want.market_sku ?? '', custom_code: want.custom_code ?? '', ym: wantYm };
      item.name = item.create.name; item.newCodes = add; add.forEach((c) => claimed.set(c, l.n)); newSkus.set(l.key.toLowerCase(), l.n);
      item.status = 'create';
    } else {
      item.status = 'missing';
      item.message = `Строка ${l.n}: товар «${label}» не найден${createMissing ? '' : ' (включите «Создавать новые товары», если его нужно добавить)'}`;
    }
  }
  for (const it of out.items) {
    out.stats.rows++; out.stats[it.status]++;
    if ((it.status === 'error' || it.status === 'missing') && it.message) out.problems.push({ n: it.n, text: it.message.replace(/^Строка \d+: /, '') });
  }
  return out;
}

// Записывает изменения одной транзакцией. -> { stats, problems, backup (таблица «как было» для обновлённых товаров), updated, created }
async function apply(db, opts) {
  const { productTable } = require('./export');
  return db.tx(async (c) => {
    const p = await plan(c, opts);
    if (p.fatal) throw new UserError(p.fatal);
    const upd = p.items.filter((i) => i.status === 'update' && i.changes.some((ch) => ch.field !== 'barcodes' && !isYm(ch.field)));
    const ymUpd = p.items.filter((i) => i.status === 'update' && i.changes.some((ch) => isYm(ch.field)));
    const withCodes = p.items.filter((i) => i.status === 'update' && i.newCodes.length);
    const backupIds = [...new Set([...upd, ...ymUpd, ...withCodes].map((i) => i.id))];
    const backup = backupIds.length ? await productTable(c, { ids: backupIds }) : null;
    const problems = p.problems.slice();

    for (let i = 0; i < upd.length; i += 500) {
      const part = upd.slice(i, i + 500);
      const val = (it, f) => { const ch = it.changes.find((x) => x.field === f); return ch ? ch.to : null; };
      await c.query(
        `UPDATE products p SET name = COALESCE(v.name, p.name), description = COALESCE(v.description, p.description),
                price = COALESCE(v.price, p.price), market_sku = COALESCE(v.market_sku, p.market_sku), custom_code = COALESCE(v.custom_code, p.custom_code)
           FROM unnest($1::int[], $2::text[], $3::text[], $4::float8[], $5::text[], $6::text[]) AS v(id, name, description, price, market_sku, custom_code)
          WHERE p.id = v.id`,
        [part.map((it) => it.id), part.map((it) => val(it, 'name')), part.map((it) => val(it, 'description')),
          part.map((it) => { const v = val(it, 'price'); return v === null ? null : Number(v); }),
          part.map((it) => val(it, 'market_sku')), part.map((it) => val(it, 'custom_code'))]);
    }
    // параметры для Маркета: одним запросом (JSON), в строке только изменённые поля
    const writeYm = async (rows) => {
      for (let i = 0; i < rows.length; i += 500) {
        const part = rows.slice(i, i + 500).map(([id, values]) => ({
          id, c: Object.fromEntries(Object.entries(values).map(([field, v]) => {
            const f = mi.BY_FIELD.get(field); const col = mi.toColumn(f, v);
            return [f.col, typeof col === 'number' ? String(col) : (typeof col === 'boolean' ? String(col) : col)];
          })),
        }));
        const set = mi.FIELDS.map((f) => {
          const raw = `j.c->>'${f.col}'`;
          const expr = f.kind === 'size' || f.kind === 'weight' ? `(${raw})::numeric` : (f.kind === 'bool' ? `(${raw})::boolean` : raw);
          return `${f.col} = CASE WHEN jsonb_exists(j.c, '${f.col}') THEN ${expr} ELSE p.${f.col} END`;
        }).join(', ');
        await c.query(`UPDATE products p SET ${set} FROM jsonb_to_recordset($1::jsonb) AS j(id int, c jsonb) WHERE p.id = j.id`, [JSON.stringify(part)]);
      }
    };
    await writeYm(ymUpd.map((it) => [it.id, Object.fromEntries(it.changes.filter((ch) => isYm(ch.field)).map((ch) => [ch.field, ch.to]))]));
    const linkCodes = async (pairs, n) => {
      if (!pairs.length) return;
      const r = await c.query('INSERT INTO product_barcodes (barcode, product_id) SELECT * FROM unnest($1::text[], $2::int[]) ON CONFLICT (barcode) DO NOTHING RETURNING barcode',
        [pairs.map((x) => x[0]), pairs.map((x) => x[1])]);
      if (r.rowCount !== pairs.length) {
        const got = new Set(r.rows.map((x) => x.barcode));
        for (const [code] of pairs) if (!got.has(code)) problems.push({ n, text: `штрихкод ${code} за это время привязали к другому товару - не привязан` });
      }
    };
    await linkCodes(withCodes.flatMap((i) => i.newCodes.map((code) => [code, i.id])), 0);
    let created = 0;
    for (const it of p.items.filter((i) => i.status === 'create')) {
      const cr = it.create;
      const r = await c.query(
        `INSERT INTO products (sku, name, description, photo_path, price, custom_code, market_sku) VALUES ($1, $2, $3, '', $4, $5, $6)
         ON CONFLICT (sku) DO NOTHING RETURNING id`, [cr.sku, cr.name, cr.description, cr.price, cr.custom_code, cr.market_sku]);
      if (!r.rowCount) { problems.push({ n: it.n, text: `товар «${cr.sku}» за это время уже создали - пропущен` }); continue; }
      created++;
      if (cr.ym && Object.keys(cr.ym).length) await writeYm([[r.rows[0].id, cr.ym]]);
      await linkCodes(it.newCodes.map((code) => [code, r.rows[0].id]), it.n);
    }
    return { stats: { ...p.stats, create: created }, problems, backup, updated: new Set([...upd, ...ymUpd, ...withCodes].map((i) => i.id)).size, created };
  });
}

module.exports = { ROLES, ROLE_LABELS, FIELD, MAX_ROWS, guessRole, guessRoles, guessHeaderRow, parsePriceCell, plan, apply };

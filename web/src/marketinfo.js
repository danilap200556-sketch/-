'use strict';
// Параметры карточки Яндекс Маркета у самого товара (столбцы market_* таблицы products): бренд, страны, габариты и вес
// упаковки, ТН ВЭД, теги, 18+. Категория и характеристики хранятся там же, но выбираются в приложении (им нужен Маркет).
// Правила разбора и тексты - те же, что в приложении (src/productsync.cpp, src/productmarket.cpp).

// Столбцы создаёт настольное приложение при подключении. Пока их нет, сайт работает по-старому.
let ready = false;
let checkedAt = 0;
async function supported(q) {
  if (ready) return true;
  const now = Date.now();
  if (now - checkedAt < 15_000) return false;
  checkedAt = now;
  const r = await q.query(
    `SELECT COUNT(*)::int AS n FROM information_schema.columns
      WHERE table_schema = current_schema() AND table_name = 'products' AND column_name LIKE 'market\\_%' AND column_name <> 'market_sku'`);
  ready = r.rows[0].n >= 12;
  return ready;
}
const resetFeatureCache = () => { ready = false; checkedAt = 0; };

// field - имя в формах и в плане обновления из Excel; col - столбец products; kind - как хранится.
const FIELDS = [
  { field: 'ym_vendor', col: 'market_vendor', label: 'Бренд', kind: 'text' },
  { field: 'ym_countries', col: 'market_countries', label: 'Страна производства', kind: 'list' },
  { field: 'ym_length', col: 'market_length_cm', label: 'Длина, см', kind: 'size' },
  { field: 'ym_width', col: 'market_width_cm', label: 'Ширина, см', kind: 'size' },
  { field: 'ym_height', col: 'market_height_cm', label: 'Высота, см', kind: 'size' },
  { field: 'ym_weight', col: 'market_weight_kg', label: 'Вес, кг', kind: 'weight' },
  { field: 'ym_commodity', col: 'market_commodity_code', label: 'ТН ВЭД', kind: 'code' },
  { field: 'ym_tags', col: 'market_tags', label: 'Теги', kind: 'list' },
  { field: 'ym_adult', col: 'market_adult', label: '18+', kind: 'bool' },
];
const BY_FIELD = new Map(FIELDS.map((f) => [f.field, f]));
const MAX_VENDOR = 100;

// Столбцы для SELECT: числа - как числа JS, остальное - как есть.
const SELECT_SQL = `COALESCE(p.market_vendor, '') AS market_vendor, COALESCE(p.market_countries, '') AS market_countries,
  p.market_length_cm::float8 AS market_length_cm, p.market_width_cm::float8 AS market_width_cm, p.market_height_cm::float8 AS market_height_cm,
  p.market_weight_kg::float8 AS market_weight_kg, COALESCE(p.market_commodity_code, '') AS market_commodity_code,
  COALESCE(p.market_tags, '') AS market_tags, COALESCE(p.market_adult, FALSE) AS market_adult,
  COALESCE(p.market_category_id, 0)::float8 AS market_category_id, COALESCE(p.market_category_name, '') AS market_category_name,
  COALESCE(p.market_params, '{}') AS market_params`;

const splitList = (s) => [...new Set(String(s ?? '').split(/[;\r\n]+/).map((x) => x.trim()).filter(Boolean))];
const listText = (l) => l.join('; ');
const num = (v, dec) => String(Math.round((Number(v) || 0) * 10 ** dec) / 10 ** dec);

// Значение поля товара «как в таблице и в Excel» (строка): «Nike», «Вьетнам; Китай», «30.5», «да».
function shown(f, row) {
  switch (f.kind) {
    case 'text': return row[f.col] ?? '';
    case 'list': return listText(splitList(row[f.col]));
    case 'size': return num(row[f.col], 2);
    case 'weight': return num(row[f.col], 3);
    case 'code': return row[f.col] ?? '';
    default: return row[f.col] ? 'да' : 'нет';
  }
}

// Ячейка или поле формы -> { value } (значение «как в таблице») или { why } (чем не подошло).
function parse(f, raw) {
  const v = String(raw ?? '').replace(/\r\n?/g, '\n').replace(/ /g, ' ').trim();
  if (f.kind === 'text') return v.length > MAX_VENDOR ? { why: `длиннее ${MAX_VENDOR} знаков` } : { value: v };
  if (f.kind === 'list') {
    const t = listText([...new Set(v.split(/[;,\n]+/).map((x) => x.trim()).filter(Boolean))]);
    return t.length > 500 ? { why: 'слишком длинный список' } : { value: t };
  }
  if (f.kind === 'size' || f.kind === 'weight') {
    const t = v.replace(/\s/g, '').replace(',', '.');
    if (!/^\d+(\.\d+)?$/.test(t) || Number(t) > 1000) {
      return { why: f.kind === 'weight' ? 'нужно число от 0 до 1000 (килограммы)' : 'нужно число от 0 до 1000 (сантиметры)' };
    }
    return { value: num(t, f.kind === 'weight' ? 3 : 2) };
  }
  if (f.kind === 'code') {
    const t = v.replace(/[\s.,]/g, '');
    return /^\d{4,14}$/.test(t) ? { value: t } : { why: 'код ТН ВЭД - от 4 до 14 цифр' };
  }
  const low = v.toLowerCase();
  if (['да', 'yes', 'true', '1', '+', 'y'].includes(low)) return { value: 'да' };
  if (['нет', 'no', 'false', '0', '-', 'n'].includes(low)) return { value: 'нет' };
  return { why: 'нужно «да» или «нет»' };
}

// Значение «как в таблице» -> то, что кладётся в столбец.
function toColumn(f, value) {
  switch (f.kind) {
    case 'list': return splitList(value.replace(/,/g, ';')).join(';');
    case 'size': case 'weight': return Number(value);
    case 'bool': return value === 'да';
    default: return value;
  }
}

// Данные формы товара -> { values: {field: «как в таблице»}, error }. Пустые числа - 0, пустое «да/нет» - нет.
function readForm(b) {
  const values = {}; let error = '';
  for (const f of FIELDS) {
    if (f.kind === 'bool') { values[f.field] = b[f.field] === '1' ? 'да' : 'нет'; continue; }
    const raw = String(b[f.field] ?? '').trim();
    if (raw === '') { values[f.field] = f.kind === 'size' || f.kind === 'weight' ? '0' : ''; continue; }
    const r = parse(f, raw);
    if (r.why) { error = error || `«${f.label}»: ${r.why}`; values[f.field] = raw; } else values[f.field] = r.value;
  }
  return { values, error };
}

// Из строки товара (SELECT * или SELECT_SQL) -> значения для формы.
function fromRow(row) {
  const values = {};
  for (const f of FIELDS) values[f.field] = shown(f, row);
  return values;
}

const paramsCount = (row) => { try { return Object.keys(JSON.parse(row.market_params || '{}')).length; } catch { return 0; } };

module.exports = { supported, resetFeatureCache, FIELDS, BY_FIELD, SELECT_SQL, shown, parse, toColumn, readForm, fromRow, splitList, paramsCount };

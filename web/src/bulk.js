'use strict';
const { UserError } = require('./stock');

// Массовое редактирование товаров - те же правила, что BulkEdit в приложении
// (src/bulkeditdialog.cpp): выбранные поля меняются у всех сразу, одной транзакцией.

const num = (v) => {
  const s = String(v ?? '').trim().replace(',', '.');
  return /^-?\d{1,9}(\.\d{1,4})?$/.test(s) ? Number(s) : null;
};

function parseSpec(body) {
  const spec = {};
  if (body.price_on === '1') {
    const mode = ['set', 'percent', 'add'].includes(body.price_mode) ? body.price_mode : 'set';
    const value = num(body.price_value);
    if (value === null) throw new UserError('Цена: введите число');
    if (mode === 'set' && (value < 0 || value > 10_000_000)) throw new UserError('Цена должна быть от 0 до 10 000 000');
    spec.price = { mode, value, round: body.price_round === '1' };
  }
  if (body.name_on === '1') {
    const mode = ['prefix', 'suffix', 'replace'].includes(body.name_mode) ? body.name_mode : 'prefix';
    const a = String(body.name_a ?? '');
    if (!a) throw new UserError(mode === 'replace' ? 'Название: укажите, что нужно найти' : 'Название: введите текст, который нужно добавить');
    spec.name = { mode, a: a.slice(0, 300), b: String(body.name_b ?? '').slice(0, 300) };
  }
  if (body.desc_on === '1') spec.description = String(body.description ?? '').slice(0, 2000);
  if (body.market_on === '1') spec.marketSku = String(body.market_sku ?? '').trim().slice(0, 200);
  if (body.loc_on === '1') {
    const w = Number(body.warehouse);
    const l = body.location === '' || body.location === undefined ? 0 : Number(body.location);
    if (!Number.isInteger(w) || w < 1 || !Number.isInteger(l) || l < 0) throw new UserError('Место хранения: выберите склад');
    spec.location = { warehouseId: w, locationId: l };
  }
  if (!Object.keys(spec).length) throw new UserError('Отметьте хотя бы одно поле, которое нужно изменить');
  return spec;
}

function newPrice(old, p) {
  let v = p.mode === 'set' ? p.value : p.mode === 'percent' ? old * (1 + p.value / 100) : old + p.value;
  v = p.round ? Math.round(v) : Math.round(v * 100) / 100;
  return v < 0 ? 0 : v;
}

function newName(old, n) {
  if (n.mode === 'prefix') return n.a + old;
  if (n.mode === 'suffix') return old + n.a;
  return old.split(n.a).join(n.b);
}

function describe(spec) {
  const parts = [];
  if (spec.price) {
    const { mode, value } = spec.price;
    parts.push(mode === 'set' ? `цена = ${value} ₽` : mode === 'percent' ? `цена ${value >= 0 ? '+' : ''}${value}%` : `цена ${value >= 0 ? '+' : ''}${value} ₽`);
  }
  if (spec.name) parts.push('название');
  if (spec.description !== undefined) parts.push('описание');
  if (spec.marketSku !== undefined) parts.push(spec.marketSku ? `артикул на Маркете = ${spec.marketSku}` : 'артикул на Маркете: очистить');
  if (spec.location) parts.push('место хранения');
  return parts.join(', ');
}

async function apply(db, ids, spec) {
  await db.tx(async (c) => {
    for (let i = 0; i < ids.length; i += 1000) {
      const chunk = ids.slice(i, i + 1000);
      if (spec.price) {
        const d = spec.price.round ? 0 : 2;
        const expr = spec.price.mode === 'set' ? '$2::numeric'
          : spec.price.mode === 'percent' ? 'price::numeric * (1 + $2::numeric / 100)' : 'price::numeric + $2::numeric';
        await c.query(`UPDATE products SET price = ROUND(GREATEST(0, ${expr}), ${d}) WHERE id = ANY($1::int[])`, [chunk, spec.price.value]);
      }
      if (spec.name) {
        const { mode, a, b } = spec.name;
        if (mode === 'prefix') await c.query('UPDATE products SET name = $2 || name WHERE id = ANY($1::int[])', [chunk, a]);
        else if (mode === 'suffix') await c.query('UPDATE products SET name = name || $2 WHERE id = ANY($1::int[])', [chunk, a]);
        else await c.query('UPDATE products SET name = replace(name, $2, $3) WHERE id = ANY($1::int[])', [chunk, a, b]);
      }
      if (spec.description !== undefined) await c.query('UPDATE products SET description = $2 WHERE id = ANY($1::int[])', [chunk, spec.description]);
      if (spec.marketSku !== undefined) await c.query('UPDATE products SET market_sku = $2 WHERE id = ANY($1::int[])', [chunk, spec.marketSku]);
      if (spec.location) {
        const { warehouseId, locationId } = spec.location;
        if (locationId) {
          const ok = await c.query('SELECT 1 FROM locations WHERE id = $1 AND warehouse_id = $2', [locationId, warehouseId]);
          if (!ok.rowCount) throw new UserError('Это место относится к другому складу');
        }
        try {
          await c.query(
            `INSERT INTO stock (product_id, warehouse_id, location_id, quantity)
             SELECT id, $2::int, $3::int, 0 FROM products WHERE id = ANY($1::int[])
             ON CONFLICT (product_id, warehouse_id) DO UPDATE SET location_id = EXCLUDED.location_id`,
            [chunk, warehouseId, locationId || null]);
        } catch (e) {
          if (e.code === '23503') throw new UserError('Склад не найден');
          throw e;
        }
      }
    }
  });
}

module.exports = { parseSpec, newPrice, newName, describe, apply };

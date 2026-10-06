'use strict';
const xlsx = require('./xlsx');
const bc = require('./barcode');
const ops = require('./stock');

// Импорт остатков из Excel/CSV - те же правила, что у вкладки «Импорт» в приложении
// (src/importtab.cpp): режимы, подбор товара по артикулу и штрихкоду, привязка штрихкодов.

const ROLES = { ignore: 0, key: 1, name: 2, stock: 3, warehouse: 4, barcode: 5 };
const ROLE_LABELS = ['— пропустить —', 'Ключ товара (артикул)', 'Название', 'Остаток', 'Склад', 'Штрихкод'];
const MAX_LOG = 400;

function guessRole(header) {
  const h = String(header || '').toLowerCase();
  if (h.includes('штрих') || h.includes('ean') || h.includes('barcode')) return ROLES.barcode;
  if (h.includes('артикул')) return ROLES.key;
  if (h.includes('наимен') || h.includes('назв')) return ROLES.name;
  if (h.includes('остаток')) return ROLES.stock;
  if (h.trim().startsWith('склад')) return ROLES.warehouse;
  return ROLES.ignore;
}

function guessHeaderRow(rows) {
  let best = 0; let bestScore = 0;
  for (let r = 0; r < Math.min(rows.length, 30); r++) {
    const score = rows[r].filter((c) => guessRole(c) !== ROLES.ignore).length;
    if (score > bestScore) { bestScore = score; best = r; }
  }
  return bestScore >= 2 ? best : 0;
}

function parseCsv(buffer) {
  let bytes = buffer;
  if (bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) bytes = bytes.subarray(3);
  let text;
  try {
    text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  } catch {
    text = new TextDecoder('windows-1251').decode(bytes); // выгрузки из старых программ
  }
  const sample = text.slice(0, 4000);
  const count = (ch) => sample.split(ch).length - 1;
  const delim = [[';', count(';')], [',', count(',')], ['\t', count('\t')]].sort((a, b) => b[1] - a[1])[0][0];
  const rows = [];
  let cur = []; let field = ''; let inQuotes = false;
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (inQuotes) {
      if (ch === '"') { if (text[i + 1] === '"') { field += '"'; i++; } else inQuotes = false; } else field += ch;
    } else if (ch === '"') inQuotes = true;
    else if (ch === delim) { cur.push(field); field = ''; }
    else if (ch === '\r') { /* пропускаем */ }
    else if (ch === '\n') { cur.push(field); field = ''; rows.push(cur); cur = []; if (rows.length > 100_000) break; }
    else field += ch;
  }
  if (field !== '' || cur.length) { cur.push(field); rows.push(cur); }
  return rows;
}

// -> { rows } или { error }
function parseFile(buffer, fileName) {
  const ext = (/\.([^.]+)$/.exec(fileName) || [])[1]?.toLowerCase();
  if (ext === 'xls') return { error: 'Файлы .xls (Excel 97-2003) не поддерживаются. Откройте файл в Excel и сохраните как «Книга Excel (.xlsx)».' };
  if (ext !== 'xlsx' && ext !== 'csv') return { error: 'Нужен файл .xlsx или .csv' };
  let rows;
  if (ext === 'xlsx') {
    const t = xlsx.readFirstSheet(buffer);
    if (!t.ok) return { error: `Не удалось прочитать файл: ${t.error}` };
    rows = t.rows;
  } else {
    rows = parseCsv(buffer);
  }
  if (!rows.some((r) => r.some((c) => String(c).trim() !== ''))) return { error: 'В файле не найдено ни одной строки.' };
  return { rows };
}

// opts: { rows, headerIdx, roles: number[] (по колонкам), warehouseId, mode: inventory|receipt|writeoff, createMissing, fileName }
async function run(db, opts) {
  const { rows, headerIdx, roles, mode, fileName } = opts;
  const col = (role) => roles.indexOf(role);
  const keyCol = col(ROLES.key); const stockCol = col(ROLES.stock); const nameCol = col(ROLES.name);
  const whCol = col(ROLES.warehouse); const codeCol = col(ROLES.barcode);
  const barcodesOnly = stockCol < 0 && codeCol >= 0;
  if ((keyCol < 0 && codeCol < 0) || (stockCol < 0 && !barcodesOnly)) {
    throw new ops.ValidationError('Укажите, какая колонка - «Ключ товара (артикул)» (или «Штрихкод»), а какая - «Остаток». ' +
      'Чтобы только привязать штрихкоды, достаточно колонок артикула и штрихкода.');
  }
  if (whCol < 0 && !opts.warehouseId) throw new ops.ValidationError('Выберите склад (или добавьте склад на странице «Склады»).');
  const asInventory = mode === 'inventory';
  const asWriteOff = mode === 'writeoff';
  const createMissing = opts.createMissing && !asWriteOff;
  const comment = `импорт из ${fileName}`;

  const stats = { processed: 0, created: 0, updated: 0, skipped: 0, errors: 0, linked: 0 };
  const log = [];
  const note = (s) => { if (log.length < MAX_LOG) log.push(s); else if (log.length === MAX_LOG) log.push('… (остальные сообщения не показаны)'); };

  await db.tx(async (c) => {
    const warehouseByName = new Map((await c.query('SELECT id, name FROM warehouses')).rows.map((w) => [w.name, w.id]));
    const productBySku = new Map((await c.query('SELECT id, sku FROM products')).rows.map((p) => [p.sku, p.id]));
    const productByCode = new Map((await c.query('SELECT barcode, product_id FROM product_barcodes')).rows.map((b) => [b.barcode, b.product_id]));

    for (let r = headerIdx + 1; r < rows.length; r++) {
      const row = rows[r];
      const rowNo = r + 1;
      const key = keyCol >= 0 ? String(row[keyCol] ?? '').trim() : '';
      const codes = codeCol >= 0
        ? String(row[codeCol] ?? '').split(/[,;\s]+/).filter(Boolean).map(bc.normalize)
        : [];
      if (!key && !codes.length) continue;
      if (key.toLowerCase().startsWith('итого')) continue;
      stats.processed++;

      let productId = key ? (productBySku.get(key) ?? null) : null;
      for (const code of codes) if (productId === null) productId = productByCode.get(code) ?? null;

      await c.query('SAVEPOINT row_sp');
      const local = { created: 0, linked: 0, updated: 0, notes: [], newSku: null, newCodes: [] };
      const linkBarcodes = async (pid) => {
        for (const code of codes) {
          const bad = bc.validate(code);
          if (bad) { local.notes.push(`Строка ${rowNo}: штрихкод «${code}» не привязан - ${bad}`); continue; }
          const owner = productByCode.get(code);
          if (owner === pid) continue;
          if (owner) {
            const sku = (await c.query('SELECT sku FROM products WHERE id = $1', [owner])).rows[0]?.sku ?? '?';
            local.notes.push(`Строка ${rowNo}: штрихкод «${code}» не привязан - штрихкод ${code} уже привязан к товару ${sku}`);
            continue;
          }
          await c.query('INSERT INTO product_barcodes (barcode, product_id) VALUES ($1, $2)', [code, pid]);
          local.newCodes.push([code, pid]);
          local.linked++;
        }
      };
      try {
        if (barcodesOnly) {
          if (productId === null) {
            stats.skipped++; note(`Строка ${rowNo}: товар «${key || codes.join(', ')}» не найден`);
          } else {
            await linkBarcodes(productId);
          }
        } else if (!key && productId === null) {
          stats.skipped++; note(`Строка ${rowNo}: штрихкод ${codes.join(', ')} не привязан ни к одному товару, а артикула нет`);
        } else {
          const raw = String(row[stockCol] ?? '').trim().replace(',', '.');
          const qtyD = raw === '' ? NaN : Number(raw);
          if (!Number.isFinite(qtyD)) {
            stats.errors++; note(`Строка ${rowNo}: не удалось прочитать остаток «${row[stockCol] ?? ''}» для «${key}»`);
          } else {
            const qty = Math.round(qtyD);
            if (!asInventory && qty === 0) { await c.query('RELEASE SAVEPOINT row_sp'); continue; }
            if (!asInventory && qty < 0) {
              stats.errors++;
              note(`Строка ${rowNo}: отрицательное количество ${qty} для «${key}» - для прихода и списания укажите, сколько добавить или убрать`);
            } else if (asInventory && qty < 0) {
              stats.errors++; note(`Строка ${rowNo}: посчитанное количество не может быть отрицательным (${qty}) для «${key}»`);
            } else if (qty > ops.MAX_QTY) {
              stats.errors++; note(`Строка ${rowNo}: слишком большое количество ${qty} для «${key}»`);
            } else {
              let warehouseId = opts.warehouseId;
              let bad = null;
              if (whCol >= 0) {
                const name = String(row[whCol] ?? '').trim();
                if (!warehouseByName.has(name)) bad = `Строка ${rowNo}: склад «${name}» не найден`;
                else warehouseId = warehouseByName.get(name);
              }
              if (!bad && !warehouseId) bad = `Строка ${rowNo}: не выбран склад для «${key}»`;
              if (!bad && productId === null && !createMissing) { stats.skipped++; }
              else if (bad) { stats.errors++; note(bad); }
              else {
                if (productId === null) {
                  const name = (nameCol >= 0 ? String(row[nameCol] ?? '').trim() : '') || key;
                  productId = (await c.query('INSERT INTO products (sku, name) VALUES ($1, $2) RETURNING id', [key, name])).rows[0].id;
                  local.created++; local.newSku = [key, productId];
                  local.notes.push(`Строка ${rowNo}: создан новый товар «${key}»`);
                }
                await linkBarcodes(productId);
                if (asInventory) await ops.inventory(c, productId, warehouseId, qty, comment);
                else if (asWriteOff) await ops.takeOut(c, productId, warehouseId, qty, comment);
                else await ops.receive(c, productId, warehouseId, qty, comment);
                local.updated++;
              }
            }
          }
        }
        await c.query('RELEASE SAVEPOINT row_sp');
        stats.created += local.created; stats.linked += local.linked; stats.updated += local.updated;
        local.notes.forEach(note);
        if (local.newSku) productBySku.set(...local.newSku);
        for (const [code, pid] of local.newCodes) productByCode.set(code, pid);
      } catch (e) {
        await c.query('ROLLBACK TO SAVEPOINT row_sp');
        if (e instanceof ops.UserError || e.code === '23505' || e.code === '22003' || e.code === '23514') {
          stats.errors++;
          note(`Строка ${rowNo}: ошибка обновления остатка для «${key}»: ${e.message}`);
        } else {
          throw e;
        }
      }
    }
  });
  log.push('');
  log.push(`Готово: обработано ${stats.processed}, создано товаров ${stats.created}, обновлено остатков ${stats.updated}, ` +
    `привязано штрихкодов ${stats.linked}, пропущено ${stats.skipped}, ошибок ${stats.errors}`);
  return { stats, log };
}

// Выгрузка остатков в том же виде, что в приложении: Артикул | Название | Склад | Остаток | Место хранения.
// Товары без остатка попадают в файл с нулём, чтобы им можно было проставить количество.
async function exportRows(db, { q = '', warehouseId = null, defaultWarehouseName = '' } = {}) {
  const { searchSql } = require('./routes/products');
  const s = searchSql(q, 3);
  const params = [warehouseId, defaultWarehouseName, ...s.params];
  const sql = `
    SELECT p.sku, p.name, w.name AS wname, s.quantity, l.code AS location
      FROM stock s JOIN products p ON p.id = s.product_id JOIN warehouses w ON w.id = s.warehouse_id
      LEFT JOIN locations l ON l.id = s.location_id
     WHERE ($1::int IS NULL OR s.warehouse_id = $1) AND ${s.sql}
    UNION ALL
    SELECT p.sku, p.name, COALESCE((SELECT name FROM warehouses WHERE id = $1), $2), 0, NULL
      FROM products p
     WHERE ${s.sql}
       AND NOT EXISTS (SELECT 1 FROM stock s WHERE s.product_id = p.id AND ($1::int IS NULL OR s.warehouse_id = $1))
     ORDER BY 1, 3 LIMIT 100000`;
  const { rows } = await db.query(sql, params);
  return [['Артикул', 'Название', 'Склад', 'Остаток', 'Место хранения'],
    ...rows.map((r) => [r.sku, r.name, r.wname ?? '', String(r.quantity), r.location ?? ''])];
}

module.exports = { ROLES, ROLE_LABELS, guessRole, guessHeaderRow, parseFile, parseCsv, run, exportRows };

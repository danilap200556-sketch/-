'use strict';
const crypto = require('crypto');
const sharp = require('sharp');
const { UserError } = require('./stock');

sharp.cache(false);
sharp.concurrency(1);

const MAX_PHOTOS = 12;
const FULL = 1280;
const THUMB = 240;
const FORMATS = new Set(['jpeg', 'png', 'webp', 'gif']);

// Любая картинка -> JPEG до 1280 px и миниатюра до 240 px (как в приложении).
// Обработка на сервере: загруженному файлу нельзя верить, а отдавать мы будем только
// наш собственный JPEG.
async function prepare(buffer) {
  let meta;
  try {
    meta = await sharp(buffer, { limitInputPixels: 60_000_000 }).metadata();
  } catch {
    throw new UserError('не удалось прочитать как картинку');
  }
  if (!FORMATS.has(meta.format)) throw new UserError(`формат ${meta.format || '?'} не поддерживается (нужны jpg, png, webp)`);
  try {
    const data = await sharp(buffer, { limitInputPixels: 60_000_000 })
      .rotate() // поворот по EXIF
      .flatten({ background: '#ffffff' })
      .resize(FULL, FULL, { fit: 'inside', withoutEnlargement: true })
      .jpeg({ quality: 82 })
      .toBuffer();
    const thumb = await sharp(data).resize(THUMB, THUMB, { fit: 'inside' }).jpeg({ quality: 78 }).toBuffer();
    return { data, thumb };
  } catch {
    throw new UserError('не удалось обработать картинку');
  }
}

const md5 = (buf) => crypto.createHash('md5').update(buf).digest('hex');

// 'added' | 'duplicate' | 'limit'. Лимит и позиция проверяются в самом INSERT.
async function addPhoto(db, productId, { data, thumb }) {
  const dup = await db.query('SELECT 1 FROM product_photos WHERE product_id = $1 AND md5(data) = $2 LIMIT 1', [productId, md5(data)]);
  if (dup.rowCount) return 'duplicate';
  const r = await db.query(
    `INSERT INTO product_photos (product_id, position, data, thumb)
     SELECT $1::int, COALESCE((SELECT MAX(position) FROM product_photos WHERE product_id = $1), 0) + 1, $2, $3
      WHERE (SELECT COUNT(*) FROM product_photos WHERE product_id = $1) < ${MAX_PHOTOS}
     RETURNING id`, [productId, data, thumb]);
  return r.rowCount ? 'added' : 'limit';
}

// ---- Подбор товара по имени файла (как PhotoStore::SkuIndex в приложении) ----
async function loadSkuIndex(db) {
  const byKey = new Map();
  const skuById = new Map();
  for (const r of (await db.query('SELECT barcode, product_id FROM product_barcodes')).rows) byKey.set(r.barcode.toLowerCase(), r.product_id);
  const products = (await db.query("SELECT id, sku, COALESCE(NULLIF(TRIM(market_sku), ''), '') AS market FROM products")).rows;
  for (const p of products) {
    skuById.set(p.id, p.sku);
    if (p.market) byKey.set(p.market.toLowerCase(), p.id);
  }
  for (const p of products) byKey.set(p.sku.toLowerCase(), p.id);
  return { byKey, skuById };
}

function matchFile(index, fileName) {
  const base = fileName.replace(/^.*[\\/]/, '').replace(/\.[^.]*$/, '').trim().toLowerCase();
  if (!base) return null;
  const hit = (key, order) => (index.byKey.has(key) ? { productId: index.byKey.get(key), sku: index.skuById.get(index.byKey.get(key)), order } : null);
  const direct = hit(base, 0);
  if (direct) return direct;
  const m = /^(.+?)(?:[\s_\-.]+(\d{1,2})|\s*\((\d{1,2})\))$/.exec(base);
  return m ? hit(m[1].trim(), Number(m[2] ?? m[3])) : null;
}

module.exports = { prepare, addPhoto, loadSkuIndex, matchFile, MAX_PHOTOS };

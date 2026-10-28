'use strict';
const crypto = require('crypto');

// Те же правила, что в настольном приложении (src/barcode.cpp).
function normalize(input) {
  return String(input ?? '').replace(/[\s-]/g, '');
}

function checkDigit(body) {
  let sum = 0;
  let weight = 3;
  for (let i = body.length - 1; i >= 0; i--) {
    sum += (body.charCodeAt(i) - 48) * weight;
    weight = weight === 3 ? 1 : 3;
  }
  return (10 - (sum % 10)) % 10;
}

// null - код годится, иначе текст ошибки.
function validate(code) {
  if (!code) return 'пустой штрихкод';
  if (!/^[0-9]+$/.test(code)) return 'штрихкод должен состоять только из цифр';
  if (![8, 12, 13, 14].includes(code.length)) {
    return `нужно 13 цифр (EAN-13), либо 8, 12 или 14 - а здесь ${code.length}`;
  }
  if (checkDigit(code.slice(0, -1)) !== code.charCodeAt(code.length - 1) - 48) {
    return 'неверная контрольная (последняя) цифра - проверьте, не ошибка ли в коде';
  }
  return null;
}

// Внутренний EAN-13 из диапазона 20..29 (не пересекается с кодами производителей).
function generateInternal() {
  let body = '2';
  for (let i = 0; i < 11; i++) body += crypto.randomInt(10);
  return body + checkDigit(body);
}

// Своя последовательность EAN-13 в базе (функцию next_internal_ean13 создаёт настольное приложение при подключении).
// Пока её нет, новые товары остаются без штрихкода, как раньше. Проверка - раз в 15 секунд, после успеха не повторяется.
let ownReady = false;
let ownCheckedAt = 0;
async function ownSupported(q) {
  if (ownReady) return true;
  const now = Date.now();
  if (now - ownCheckedAt < 15_000) return false;
  ownCheckedAt = now;
  const r = await q.query("SELECT to_regprocedure('next_internal_ean13()') IS NOT NULL AS ok");
  ownReady = r.rows[0].ok;
  return ownReady;
}
const resetOwnCache = () => { ownReady = false; ownCheckedAt = 0; };

// Присваивает свой EAN-13 товарам из ids, у которых нет ни одного штрихкода. -> сколько присвоено (0 - функции в базе ещё нет)
async function ensureOwn(q, ids) {
  const list = [...new Set((ids || []).map(Number).filter((n) => Number.isInteger(n) && n > 0))];
  if (!list.length || !(await ownSupported(q))) return 0;
  const r = await q.query(
    `INSERT INTO product_barcodes (barcode, product_id)
     SELECT next_internal_ean13(), p.id FROM products p WHERE p.id = ANY($1::int[])
        AND NOT EXISTS (SELECT 1 FROM product_barcodes b WHERE b.product_id = p.id) ORDER BY p.id`, [list]);
  return r.rowCount;
}

module.exports = { normalize, validate, generateInternal, checkDigit, ownSupported, resetOwnCache, ensureOwn };

'use strict';
// Операции с остатками. Типы движений и комментарии - те же, что у настольного
// приложения (src/database.cpp), чтобы журнал выглядел одинаково с обеих сторон.
// В отличие от приложения, списание проверяет остаток прямо в UPDATE, поэтому два
// одновременных списания (сайт + приложение) не уведут остаток в минус.

const MAX_QTY = 100_000_000;

class UserError extends Error {}
// Неверный ввод (400), в отличие от конфликта с текущим состоянием (409).
class ValidationError extends UserError {}

function qty(value, { allowZero = false } = {}) {
  const s = String(value ?? '').trim();
  if (!/^\d+$/.test(s)) throw new ValidationError('Количество - целое неотрицательное число');
  const n = Number(s);
  if (n > MAX_QTY) throw new ValidationError('Слишком большое количество');
  if (n === 0 && !allowZero) throw new ValidationError('Количество должно быть больше нуля');
  return n;
}

async function addMovement(c, productId, warehouseId, related, type, delta, comment) {
  await c.query(
    `INSERT INTO stock_movements (product_id, warehouse_id, related_warehouse_id, type, delta, comment)
     VALUES ($1, $2, $3, $4, $5, $6)`,
    [productId, warehouseId, related, type, delta, comment]);
}

async function receive(c, productId, warehouseId, n, comment, type = 'receipt', related = null) {
  await c.query(
    `INSERT INTO stock (product_id, warehouse_id, quantity) VALUES ($1, $2, $3)
     ON CONFLICT (product_id, warehouse_id) DO UPDATE SET quantity = stock.quantity + EXCLUDED.quantity`,
    [productId, warehouseId, n]);
  await addMovement(c, productId, warehouseId, related, type, n, comment);
}

async function takeOut(c, productId, warehouseId, n, comment, type = 'writeoff', related = null) {
  const r = await c.query(
    `UPDATE stock SET quantity = quantity - $3
      WHERE product_id = $1 AND warehouse_id = $2 AND quantity >= $3 RETURNING quantity`,
    [productId, warehouseId, n]);
  if (r.rowCount === 0) {
    const have = await c.query('SELECT quantity FROM stock WHERE product_id = $1 AND warehouse_id = $2',
      [productId, warehouseId]);
    throw new UserError(`Недостаточно остатка: на складе ${have.rows[0]?.quantity ?? 0} шт, требуется списать ${n} шт`);
  }
  await addMovement(c, productId, warehouseId, related, type, -n, comment);
}

async function transfer(c, productId, fromId, toId, n, comment) {
  if (fromId === toId) throw new UserError('Исходный и целевой склад совпадают');
  await takeOut(c, productId, fromId, n, comment, 'transfer_out', toId);
  await receive(c, productId, toId, n, comment, 'transfer_in', fromId);
}

// counted - посчитанное фактическое количество; пишется разница с текущим.
async function inventory(c, productId, warehouseId, counted, comment) {
  await c.query(
    `INSERT INTO stock (product_id, warehouse_id, quantity) VALUES ($1, $2, 0)
     ON CONFLICT (product_id, warehouse_id) DO NOTHING`, [productId, warehouseId]);
  const cur = (await c.query(
    'SELECT quantity FROM stock WHERE product_id = $1 AND warehouse_id = $2 FOR UPDATE',
    [productId, warehouseId])).rows[0].quantity;
  const delta = counted - cur;
  if (delta === 0) return;
  const full = `${comment ? comment + ' ' : ''}(инвентаризация: было ${cur}, стало ${counted})`;
  await c.query('UPDATE stock SET quantity = $3 WHERE product_id = $1 AND warehouse_id = $2',
    [productId, warehouseId, counted]);
  await addMovement(c, productId, warehouseId, null, 'inventory_adjust', delta, full);
}

const TYPE_LABELS = {
  receipt: 'Приход',
  writeoff: 'Списание',
  transfer_out: 'Перемещение (расход)',
  transfer_in: 'Перемещение (приход)',
  inventory_adjust: 'Инвентаризация',
};

module.exports = { UserError, ValidationError, qty, receive, takeOut, transfer, inventory, TYPE_LABELS, MAX_QTY };

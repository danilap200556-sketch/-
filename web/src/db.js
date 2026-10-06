'use strict';
const fs = require('fs');
const { Pool } = require('pg');

// Сеть до облачной БД может обрывать простаивающие соединения. Чтение
// безопасно повторить один раз; запись не повторяем, чтобы не выполнить дважды.
function isConnectionError(e) {
  return !e.code || /^(08|57P0|53300)/.test(e.code) || /ECONN|EPIPE|ETIMEDOUT|terminated|timeout/i.test(e.message || '');
}

function createDb(cfg) {
  let ssl = false;
  if (cfg.ssl === 'require') ssl = { rejectUnauthorized: false }; // как sslmode=require в приложении
  if (cfg.ssl === 'verify') {
    ssl = { rejectUnauthorized: true };
    if (cfg.sslCaFile) ssl.ca = fs.readFileSync(cfg.sslCaFile, 'utf8');
  }
  const pool = new Pool({
    host: cfg.host,
    port: cfg.port,
    database: cfg.database,
    user: cfg.user,
    password: cfg.password,
    ssl,
    max: cfg.poolSize,
    connectionTimeoutMillis: 20000,
    idleTimeoutMillis: 30000,
    keepAlive: true,
    statement_timeout: 30000,
    application_name: 'inventory-web',
  });
  pool.on('error', (e) => console.error('[db] ошибка простаивающего соединения:', e.message));

  async function query(text, params) {
    try {
      return await pool.query(text, params);
    } catch (e) {
      if (/^\s*select/i.test(text) && isConnectionError(e)) return pool.query(text, params);
      throw e;
    }
  }

  async function tx(fn) {
    const client = await pool.connect();
    let broken = false;
    try {
      await client.query('BEGIN');
      const result = await fn(client);
      await client.query('COMMIT');
      return result;
    } catch (e) {
      try {
        await client.query('ROLLBACK');
      } catch {
        broken = true;
      }
      throw e;
    } finally {
      client.release(broken);
    }
  }

  // Таблицы создаёт и обновляет настольное приложение - сайт их не меняет,
  // только проверяет, что приложение уже подготовило эту базу.
  const required = [
    ['products', 'sku'], ['products', 'market_sku'], ['warehouses', 'name'], ['locations', 'code'],
    ['stock', 'quantity'], ['stock_movements', 'delta'], ['users', 'is_admin'], ['users', 'read_only'], ['product_barcodes', 'barcode'], ['product_photos', 'data'],
  ];
  async function checkSchema() {
    const { rows } = await pool.query(
      `SELECT table_name, column_name FROM information_schema.columns WHERE table_schema = current_schema()`);
    const have = new Set(rows.map((r) => `${r.table_name}.${r.column_name}`));
    const missing = required.map(([t, c]) => `${t}.${c}`).filter((k) => !have.has(k));
    if (missing.length) {
      throw new Error('В базе нет: ' + missing.join(', ') +
        '. Запустите последнюю версию настольного приложения один раз с этой базой - оно создаст и обновит таблицы.');
    }
  }

  return { pool, query, tx, checkSchema, end: () => pool.end() };
}

module.exports = { createDb };

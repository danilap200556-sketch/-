'use strict';
const xlsx = require('../xlsx');
const { productTable, toCsv } = require('../export');
const { parseId, str } = require('../util');

const MAX_SELECTION = 20000;

// Выгрузка полных данных товаров: всё найденное по поиску (GET) или отмеченное в списке (POST из формы списка).
module.exports = (app, { db, requireLogin }) => {
  const stamp = () => new Date().toISOString().slice(0, 10);

  async function pick(req) {
    const b = req.method === 'POST' ? req.body : req.query;
    if (req.method !== 'POST' || b.all === '1') return { q: str(b.q, 100) };
    const raw = [].concat(b.ids ?? [], String(b.idlist ?? '').split(','));
    const ids = [...new Set(raw.map((v) => parseId(String(v).trim())).filter(Boolean))].slice(0, MAX_SELECTION);
    return { ids };
  }

  const handler = (format) => async (req, res) => {
    const sel = await pick(req);
    if (sel.ids && !sel.ids.length) {
      res.flash('err', 'Отметьте хотя бы один товар в списке');
      return res.redirect('/products');
    }
    const table = await productTable(db, sel);
    if (format === 'xlsx') {
      const buf = xlsx.write([{ name: 'Товары', rows: table.rows, numericColumns: table.numericColumns, columnWidths: table.columnWidths }]);
      res.set({
        'Content-Type': 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
        'Content-Disposition': `attachment; filename="products_${stamp()}.xlsx"; filename*=UTF-8''${encodeURIComponent(`Товары_${stamp()}.xlsx`)}`,
      });
      return res.send(buf);
    }
    res.set({
      'Content-Type': 'text/csv; charset=utf-8',
      'Content-Disposition': `attachment; filename="products_${stamp()}.csv"; filename*=UTF-8''${encodeURIComponent(`Товары_${stamp()}.csv`)}`,
    });
    return res.send(toCsv(table));
  };

  app.get('/export/products.xlsx', requireLogin, handler('xlsx'));
  app.get('/export/products.csv', requireLogin, handler('csv'));
  app.post('/export/products.xlsx', requireLogin, handler('xlsx'));
  app.post('/export/products.csv', requireLogin, handler('csv'));
};

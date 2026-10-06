'use strict';
const { unzipSync, zipSync, strToU8, strFromU8 } = require('fflate');

// Чтение и запись .xlsx без тяжёлых зависимостей (тот же формат и те же правила, что
// в приложении - src/xlsx.cpp): читается первый лист, значения превращаются в текст.

const MAX_PART_BYTES = 80 * 1024 * 1024;
const MAX_ROWS = 100_000;
const MAX_COLS = 500;

const ENTITIES = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'" };
function decode(s) {
  return s
    .replace(/&(#x[0-9a-fA-F]+|#\d+|[a-z]+);/g, (m, e) => {
      if (e[0] === '#') {
        const cp = e[1] === 'x' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
        return cp > 0 && cp <= 0x10ffff ? String.fromCodePoint(cp) : '';
      }
      return ENTITIES[e] ?? m;
    })
    .replace(/_x([0-9A-Fa-f]{4})_/g, (m, h) => String.fromCharCode(parseInt(h, 16)));
}

function attrsOf(s) {
  const out = {};
  for (const m of s.matchAll(/([\w:.-]+)\s*=\s*"([^"]*)"/g)) out[m[1]] = decode(m[2]);
  return out;
}

// Все элементы <name ...>...</name> (и самозакрывающиеся) - без разбора вложенности того же имени
// (в нашем формате <row>, <c>, <si> не вкладываются).
function* elements(xml, name) {
  const re = new RegExp(`<${name}\\b([^>]*?)(\\/>|>)`, 'g');
  let m;
  while ((m = re.exec(xml))) {
    if (m[2] === '/>') { yield { attrs: attrsOf(m[1]), inner: '' }; continue; }
    const end = xml.indexOf(`</${name}>`, re.lastIndex);
    if (end < 0) return;
    yield { attrs: attrsOf(m[1]), inner: xml.slice(re.lastIndex, end) };
    re.lastIndex = end + name.length + 3;
  }
}

// Текст всех <t> внутри элемента, без фонетических подсказок <rPh>.
function richText(inner) {
  const clean = inner.replace(/<rPh\b[\s\S]*?<\/rPh>/g, '');
  let text = '';
  for (const m of clean.matchAll(/<t\b[^>]*?(?:\/>|>([\s\S]*?)<\/t>)/g)) text += decode(m[1] ?? '');
  return text;
}

function columnFromRef(ref) {
  let col = 0; let letters = 0;
  for (const ch of ref || '') {
    if (ch >= 'A' && ch <= 'Z') { col = col * 26 + (ch.charCodeAt(0) - 64); letters++; } else break;
  }
  return letters ? col - 1 : -1;
}

function columnLetters(col) {
  let s = '';
  for (let n = col + 1; n > 0; n = Math.floor((n - 1) / 26)) s = String.fromCharCode(65 + ((n - 1) % 26)) + s;
  return s;
}

// Excel хранит 3 как 3 или 2.9999999999999996 - приводим к тому, что человек видит в ячейке.
function normalizeNumber(raw) {
  const d = Number(raw);
  if (raw.trim() === '' || !Number.isFinite(d)) return raw;
  const r = Math.round(d);
  if (Math.abs(d - r) < 1e-9 && Math.abs(r) < 1e15) return String(r);
  return String(Number(d.toPrecision(15)));
}

// -> { ok, rows: string[][], error }
function readFirstSheet(buffer) {
  let files;
  try {
    let total = 0;
    files = unzipSync(new Uint8Array(buffer), {
      filter: (f) => {
        total += f.originalSize;
        if (f.originalSize > MAX_PART_BYTES || total > MAX_PART_BYTES * 2) throw new Error('слишком большой файл');
        return f.name === 'xl/workbook.xml' || f.name === 'xl/_rels/workbook.xml.rels' ||
          f.name === 'xl/sharedStrings.xml' || /^xl\/worksheets\/[^/]+\.xml$/.test(f.name);
      },
    });
  } catch (e) {
    return { ok: false, rows: [], error: e.message === 'слишком большой файл' ? e.message : 'файл не открывается как .xlsx' };
  }
  const text = (name) => (files[name] ? strFromU8(files[name]) : '');

  const shared = [];
  for (const si of elements(text('xl/sharedStrings.xml'), 'si')) shared.push(richText(si.inner));

  // Первый лист по порядку в книге -> файл по связям; запасной вариант - sheet1.xml
  let sheetPath = 'xl/worksheets/sheet1.xml';
  const first = elements(text('xl/workbook.xml'), 'sheet').next().value;
  const relId = first && (first.attrs['r:id'] ?? Object.entries(first.attrs).find(([k]) => k.endsWith(':id'))?.[1]);
  if (relId) {
    for (const rel of elements(text('xl/_rels/workbook.xml.rels'), 'Relationship')) {
      if (rel.attrs.Id === relId && rel.attrs.Target) {
        sheetPath = rel.attrs.Target.startsWith('/') ? rel.attrs.Target.slice(1) : `xl/${rel.attrs.Target}`;
        break;
      }
    }
  }
  const sheetXml = text(sheetPath);
  if (!sheetXml) return { ok: false, rows: [], error: 'в файле не найден лист с данными' };

  const rows = [];
  for (const row of elements(sheetXml, 'row')) {
    const rowIndex = row.attrs.r ? parseInt(row.attrs.r, 10) - 1 : rows.length;
    if (!(rowIndex >= 0) || rowIndex >= MAX_ROWS) continue;
    const cells = [];
    let col = -1;
    for (const c of elements(row.inner, 'c')) {
      const refCol = columnFromRef(c.attrs.r);
      col = refCol >= 0 ? refCol : col + 1;
      if (col >= MAX_COLS) continue;
      const type = c.attrs.t || '';
      let value;
      const v = /<v>([\s\S]*?)<\/v>/.exec(c.inner);
      const inline = /<is\b[^>]*>([\s\S]*?)<\/is>/.exec(c.inner);
      if (inline) value = richText(inline[1]);
      else if (v) value = decode(v[1]);
      else continue;
      let out;
      if (type === 's') out = shared[parseInt(value, 10)] ?? '';
      else if (type === 'str' || type === 'inlineStr' || type === 'e') out = value;
      else if (type === 'b') out = value === '1' ? 'TRUE' : 'FALSE';
      else out = normalizeNumber(value);
      while (cells.length <= col) cells.push('');
      cells[col] = out;
    }
    while (cells.length && cells[cells.length - 1] === '') cells.pop();
    while (rows.length < rowIndex) rows.push([]);
    rows[rowIndex] = cells;
  }
  return { ok: true, rows, error: '' };
}

const xmlText = (s) =>
  String(s ?? '').replace(/[^\u0009\u000a\u000d -퟿-�\u{10000}-\u{10ffff}]/gu, '')
    .replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

function sheetName(name, i) {
  const n = String(name).replace(/[[\]:*?/\\]/g, '_').slice(0, 31).trim();
  return n || `Лист${i + 1}`;
}

// sheets: [{ name, rows: string[][], numericColumns?: number[], columnWidths?: number[] }] -> Buffer
function write(sheets) {
  const parts = {};
  let types = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">' +
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>' +
    '<Default Extension="xml" ContentType="application/xml"/>' +
    '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>' +
    '<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>';
  let wbSheets = '';
  let rels = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">';
  sheets.forEach((sheet, i) => {
    const n = i + 1;
    types += `<Override PartName="/xl/worksheets/sheet${n}.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>`;
    wbSheets += `<sheet name="${xmlText(sheetName(sheet.name, i))}" sheetId="${n}" r:id="rId${n}"/>`;
    rels += `<Relationship Id="rId${n}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet${n}.xml"/>`;
    const numeric = new Set(sheet.numericColumns || []);
    const maxCols = sheet.rows.reduce((m, r) => Math.max(m, r.length), 0);
    let xml = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">' +
      '<sheetViews><sheetView workbookViewId="0"><pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/></sheetView></sheetViews>';
    if (sheet.columnWidths?.length) {
      xml += '<cols>' + sheet.columnWidths.map((w, c) => `<col min="${c + 1}" max="${c + 1}" width="${w}" customWidth="1"/>`).join('') + '</cols>';
    }
    xml += '<sheetData>';
    sheet.rows.forEach((cells, r) => {
      xml += `<row r="${r + 1}">`;
      cells.forEach((cell, c) => {
        const v = cell === null || cell === undefined ? '' : String(cell);
        if (v === '') return;
        const ref = columnLetters(c) + (r + 1);
        const style = r === 0 ? ' s="1"' : '';
        if (r > 0 && numeric.has(c) && v.trim() !== '' && Number.isFinite(Number(v))) xml += `<c r="${ref}"${style}><v>${v}</v></c>`;
        else xml += `<c r="${ref}"${style} t="inlineStr"><is><t xml:space="preserve">${xmlText(v)}</t></is></c>`;
      });
      xml += '</row>';
    });
    xml += '</sheetData>';
    if (sheet.rows.length > 1 && maxCols > 0) xml += `<autoFilter ref="A1:${columnLetters(maxCols - 1)}${sheet.rows.length}"/>`;
    xml += '</worksheet>';
    parts[`xl/worksheets/sheet${n}.xml`] = strToU8(xml);
  });
  types += '</Types>';
  rels += `<Relationship Id="rId${sheets.length + 1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>`;
  parts['[Content_Types].xml'] = strToU8(types);
  parts['_rels/.rels'] = strToU8('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' +
    '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>');
  parts['xl/workbook.xml'] = strToU8('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" ' +
    `xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>${wbSheets}</sheets></workbook>`);
  parts['xl/_rels/workbook.xml.rels'] = strToU8(rels);
  parts['xl/styles.xml'] = strToU8('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">' +
    '<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts>' +
    '<fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills>' +
    '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>' +
    '<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>' +
    '<cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0" applyFont="1"/></cellXfs>' +
    '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>');
  return Buffer.from(zipSync(parts, { level: 6 }));
}

module.exports = { readFirstSheet, write };

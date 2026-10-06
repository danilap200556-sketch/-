'use strict';
const busboy = require('busboy');
const { safeEqual } = require('./security');

// expose: текст ошибки можно показать пользователю.
const fail = (status, message) => Object.assign(new Error(message), { status, expose: true });

// Разбор multipart-формы (загрузка файлов) с жёсткими ограничениями. Тело читается
// в память, поэтому лимиты небольшие; фото при необходимости грузятся несколькими запросами.
// CSRF: токен приходит заголовком X-CSRF-Token (проверен в requireLogin) или полем _csrf -
// тогда он проверяется здесь, и без него результат не возвращается.
function readMultipart(req, { maxFiles = 12, maxFileBytes = 12 * 1024 * 1024, maxTotalBytes = 48 * 1024 * 1024 } = {}) {
  return new Promise((resolve, reject) => {
    const declared = Number(req.get('content-length') || 0);
    if (declared > maxTotalBytes + 64 * 1024) return reject(fail(413, 'Слишком большой запрос'));
    let bb;
    try {
      bb = busboy({
        headers: req.headers,
        defParamCharset: 'utf8',
        limits: { files: maxFiles, fileSize: maxFileBytes, fields: 200, fieldSize: 256 * 1024, parts: maxFiles + 220 },
      });
    } catch {
      return reject(fail(400, 'Некорректный запрос'));
    }
    const fields = {};
    const files = [];
    const rejected = [];
    let total = 0;
    let aborted = false;

    const abort = (err) => {
      if (aborted) return;
      aborted = true;
      req.unpipe(bb);
      req.resume();
      reject(err);
    };
    bb.on('field', (name, value) => {
      fields[name] = name in fields ? [].concat(fields[name], value) : value;
    });
    bb.on('file', (name, stream, info) => {
      const chunks = [];
      let truncated = false;
      stream.on('data', (d) => {
        total += d.length;
        if (total > maxTotalBytes) return abort(fail(413, 'Слишком большой запрос'));
        chunks.push(d);
      });
      stream.on('limit', () => { truncated = true; });
      stream.on('end', () => {
        if (!info.filename) return; // пустое поле файла
        if (truncated) rejected.push({ filename: info.filename, reason: `файл больше ${Math.round(maxFileBytes / 1048576)} МБ` });
        else files.push({ field: name, filename: info.filename, buffer: Buffer.concat(chunks) });
      });
    });
    bb.on('filesLimit', () => rejected.push({ filename: '', reason: `за один раз можно не больше ${maxFiles} файлов` }));
    bb.on('error', (e) => abort(fail(400, e.message)));
    bb.on('close', () => {
      if (aborted) return;
      if (req.csrfPending && !safeEqual(fields._csrf ?? '', req.ctx.csrf)) return reject(fail(403, 'Страница устарела. Обновите её и повторите.'));
      resolve({ fields, files, rejected });
    });
    req.pipe(bb);
  });
}

module.exports = { readMultipart };

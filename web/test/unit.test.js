'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const pw = require('../src/passwords');
const bc = require('../src/barcode');
const { html } = require('../src/html');
const { csvCell, parsePrice, parseId, likeEscape } = require('../src/util');
const sec = require('../src/security');

test('пароль: формат хеша = SHA-256(соль + пароль), как в приложении', () => {
  // Вектор посчитан независимо: sha256(bytes.fromhex(salt) + "пароль123".encode())
  const salt = '00112233445566778899aabbccddeeff';
  assert.equal(pw.hashPassword('пароль123', salt), require('crypto').createHash('sha256')
    .update(Buffer.concat([Buffer.from(salt, 'hex'), Buffer.from('пароль123', 'utf8')])).digest('hex'));
  const h = pw.hashPassword('abc', salt);
  assert.ok(pw.verifyPassword('abc', salt, h));
  assert.ok(!pw.verifyPassword('abd', salt, h));
  assert.ok(!pw.verifyPassword('abc', salt, ''));
  assert.match(pw.newSalt(), /^[0-9a-f]{32}$/);
});

test('штрихкоды: контрольная цифра и длина', () => {
  assert.equal(bc.validate('4006381333931'), null);       // EAN-13
  assert.equal(bc.validate('036000291452'), null);        // UPC-A
  assert.equal(bc.validate('96385074'), null);            // EAN-8
  assert.match(bc.validate('4006381333932'), /контрольная/);
  assert.match(bc.validate('12345'), /нужно 13 цифр/);
  assert.match(bc.validate('40063813339a1'), /только из цифр/);
  assert.equal(bc.normalize(' 4006-381 333931 '), '4006381333931');
  for (let i = 0; i < 50; i++) { const g = bc.generateInternal(); assert.equal(g.length, 13); assert.ok(g.startsWith('2')); assert.equal(bc.validate(g), null); }
});

test('html: всё экранируется, вложенные шаблоны - нет', () => {
  const evil = '<script>alert(1)</script>"\'&';
  assert.equal(html`<b>${evil}</b>`.toString(), '<b>&lt;script&gt;alert(1)&lt;/script&gt;&quot;&#39;&amp;</b>');
  assert.equal(html`<ul>${[1, 2].map((n) => html`<li>${n}</li>`)}</ul>`.toString(), '<ul><li>1</li><li>2</li></ul>');
  assert.equal(html`${null}${undefined}${false}`.toString(), '');
});

test('util: цены, id, CSV, LIKE', () => {
  assert.equal(parsePrice('1 299'.replace(' ', '')), 1299);
  assert.equal(parsePrice('12,5'), 12.5);
  assert.equal(parsePrice(''), 0);
  assert.equal(parsePrice('-1'), null);
  assert.equal(parsePrice('1.234'), null);
  assert.equal(parsePrice('99999999'), null);
  assert.equal(parseId('12'), 12);
  assert.equal(parseId('0'), null);
  assert.equal(parseId('99999999999'), null);
  assert.equal(parseId('1; DROP'), null);
  assert.equal(csvCell('=HYPERLINK("x")'), `"'=HYPERLINK(""x"")"`);
  assert.equal(csvCell('a;b'), '"a;b"');
  assert.equal(csvCell('-5'), "'-5");
  assert.equal(likeEscape('50%_\\'), '50\\%\\_\\\\');
});

test('подпись cookie: подделка и порча отвергаются', () => {
  const t = sec.sign('s'.repeat(32), { uid: 1 });
  assert.deepEqual(sec.unsign('s'.repeat(32), t), { uid: 1 });
  assert.equal(sec.unsign('t'.repeat(32), t), null);
  assert.equal(sec.unsign('s'.repeat(32), t.replace(/.$/, (c) => (c === 'A' ? 'B' : 'A'))), null);
  assert.equal(sec.unsign('s'.repeat(32), 'garbage'), null);
  assert.equal(sec.unsign('s'.repeat(32), undefined), null);
});

test('ограничитель попыток: блокирует и отпускает по окну', () => {
  const rl = new sec.RateLimiter(3, 1000);
  for (let i = 0; i < 3; i++) { assert.ok(!rl.blocked('k', 0)); rl.fail('k', 0); }
  assert.ok(rl.blocked('k', 500));
  assert.ok(!rl.blocked('k', 1500));
  rl.fail('x', 0); rl.reset('x'); assert.ok(!rl.blocked('x', 0));
});

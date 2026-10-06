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

module.exports = { normalize, validate, generateInternal, checkDigit };

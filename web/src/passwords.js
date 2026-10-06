'use strict';
const crypto = require('crypto');

// Формат хранения совместим с настольным приложением: SHA-256 от (соль + пароль),
// соль - 16 случайных байт в hex. Менять нельзя: иначе пароли перестанут
// подходить в приложении (и наоборот).
function hashPassword(password, saltHex) {
  return crypto
    .createHash('sha256')
    .update(Buffer.concat([Buffer.from(saltHex, 'hex'), Buffer.from(password, 'utf8')]))
    .digest('hex');
}

function newSalt() {
  return crypto.randomBytes(16).toString('hex');
}

function verifyPassword(password, saltHex, expectedHex) {
  const a = Buffer.from(hashPassword(password, saltHex), 'utf8');
  const b = Buffer.from(String(expectedHex || ''), 'utf8');
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

// На сайте требования строже, чем в приложении (там от 4 символов): сайт виден из интернета.
function validateNewPassword(password) {
  if (typeof password !== 'string' || password.length < 8) return 'Пароль должен быть не короче 8 символов';
  if (password.length > 200) return 'Пароль слишком длинный';
  return null;
}

module.exports = { hashPassword, newSalt, verifyPassword, validateNewPassword };

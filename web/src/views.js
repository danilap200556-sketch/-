'use strict';
const { html } = require('./html');

// Миниатюра товара для списков; клик открывает крупный просмотр (public/app.js).
function thumbImg(productId, coverId, big = false) {
  const cls = big ? 'thumb big' : 'thumb';
  if (!coverId) return html`<span class="${cls} none">нет фото</span>`;
  return html`<img class="${cls}" src="/photos/${coverId}/thumb" data-product="${productId}" alt="" loading="lazy">`;
}

const COVER_SQL = '(SELECT id FROM product_photos ph WHERE ph.product_id = p.id ORDER BY ph.position, ph.id LIMIT 1)';

module.exports = { thumbImg, COVER_SQL };

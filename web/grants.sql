-- Права для отдельного пользователя БД, под которым работает сайт.
-- Раздел «Заказы Маркета» читает ключи кабинетов (market_accounts) - только чтение: сайт
-- не может ни менять кабинеты, ни добавлять новые. Настраиваются кабинеты в приложении.
-- Если раздел «Заказы Маркета» на сайте не нужен, строку с market_accounts можно не выдавать.
-- Пользователя (web_user) создайте в консоли Яндекс Облака (Managed Service for PostgreSQL ->
-- Пользователи), затем выполните этот файл под владельцем базы (тем же, под кем работает приложение).
GRANT SELECT, INSERT, UPDATE, DELETE ON
  products, warehouses, locations, stock, stock_movements, users, product_barcodes, product_photos
TO web_user;
GRANT SELECT ON market_accounts TO web_user;

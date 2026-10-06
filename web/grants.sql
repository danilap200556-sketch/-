-- Права для отдельного пользователя БД, под которым работает сайт.
-- Сайту НЕ нужны таблицы market_accounts / market_account_warehouses - там API-ключи Маркета.
-- Пользователя (web_user) создайте в консоли Яндекс Облака (Managed Service for PostgreSQL ->
-- Пользователи), затем выполните этот файл под владельцем базы (тем же, под кем работает приложение).
GRANT SELECT, INSERT, UPDATE, DELETE ON
  products, warehouses, locations, stock, stock_movements, users, product_barcodes
TO web_user;

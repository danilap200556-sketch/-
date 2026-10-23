-- Копия schemaDdl() из src/database.cpp (для тестов сайта). Обновляется: python3 web/test/extract-schema.py
CREATE TABLE IF NOT EXISTS products (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            sku TEXT NOT NULL UNIQUE,
            name TEXT NOT NULL,
            description TEXT,
            photo_path TEXT,
            price REAL NOT NULL DEFAULT 0,
            custom_code TEXT,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
CREATE TABLE IF NOT EXISTS warehouses (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            name TEXT NOT NULL UNIQUE,
            address TEXT
        );
CREATE TABLE IF NOT EXISTS locations (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            warehouse_id INTEGER NOT NULL REFERENCES warehouses(id) ON DELETE CASCADE,
            code TEXT NOT NULL,
            description TEXT,
            UNIQUE(warehouse_id, code)
        );
CREATE TABLE IF NOT EXISTS stock (
            product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
            warehouse_id INTEGER NOT NULL REFERENCES warehouses(id) ON DELETE CASCADE,
            location_id INTEGER REFERENCES locations(id) ON DELETE SET NULL,
            quantity INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (product_id, warehouse_id)
        );
CREATE TABLE IF NOT EXISTS users (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            username TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            salt TEXT NOT NULL,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
CREATE TABLE IF NOT EXISTS stock_movements (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
            warehouse_id INTEGER NOT NULL REFERENCES warehouses(id) ON DELETE CASCADE,
            related_warehouse_id INTEGER REFERENCES warehouses(id),
            type TEXT NOT NULL,
            delta INTEGER NOT NULL,
            comment TEXT,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
ALTER TABLE users ADD COLUMN IF NOT EXISTS is_admin BOOLEAN NOT NULL DEFAULT FALSE;
UPDATE users SET is_admin = TRUE WHERE id = (SELECT MIN(id) FROM users) AND NOT EXISTS (SELECT 1 FROM users WHERE is_admin);
ALTER TABLE products ADD COLUMN IF NOT EXISTS market_sku TEXT;
CREATE TABLE IF NOT EXISTS market_accounts (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            name TEXT NOT NULL,
            api_key TEXT NOT NULL,
            business_id BIGINT NOT NULL,
            campaign_id BIGINT NOT NULL,
            warehouse_groups BOOLEAN NOT NULL DEFAULT FALSE,
            market_warehouse_id BIGINT,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
CREATE TABLE IF NOT EXISTS product_barcodes (
            barcode TEXT PRIMARY KEY,
            product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
CREATE INDEX IF NOT EXISTS product_barcodes_product_idx ON product_barcodes (product_id);
CREATE TABLE IF NOT EXISTS market_account_warehouses (
            account_id INTEGER NOT NULL REFERENCES market_accounts(id) ON DELETE CASCADE,
            warehouse_id INTEGER NOT NULL REFERENCES warehouses(id) ON DELETE CASCADE,
            PRIMARY KEY (account_id, warehouse_id)
        );
ALTER TABLE users ADD COLUMN IF NOT EXISTS read_only BOOLEAN NOT NULL DEFAULT FALSE;
CREATE TABLE IF NOT EXISTS product_photos (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
            position INTEGER NOT NULL DEFAULT 0,
            data BYTEA NOT NULL,
            thumb BYTEA NOT NULL,
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
CREATE INDEX IF NOT EXISTS product_photos_product_idx ON product_photos (product_id, position, id);
ALTER TABLE stock_movements ADD COLUMN IF NOT EXISTS doc_id TEXT;
ALTER TABLE stock_movements ADD COLUMN IF NOT EXISTS reverts_id INTEGER REFERENCES stock_movements(id);
CREATE INDEX IF NOT EXISTS stock_movements_doc_idx ON stock_movements (doc_id) WHERE doc_id IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS stock_movements_reverts_uq ON stock_movements (reverts_id) WHERE reverts_id IS NOT NULL;
ALTER TABLE product_photos ADD COLUMN IF NOT EXISTS public_token TEXT;
CREATE UNIQUE INDEX IF NOT EXISTS product_photos_token_uq ON product_photos (public_token) WHERE public_token IS NOT NULL;
CREATE TABLE IF NOT EXISTS market_cards (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            account_id INTEGER NOT NULL REFERENCES market_accounts(id) ON DELETE CASCADE,
            product_id INTEGER REFERENCES products(id) ON DELETE SET NULL,
            offer_id TEXT NOT NULL,
            name TEXT NOT NULL DEFAULT '',
            description TEXT NOT NULL DEFAULT '',
            vendor TEXT NOT NULL DEFAULT '',
            vendor_code TEXT NOT NULL DEFAULT '',
            category_id BIGINT,
            category_name TEXT NOT NULL DEFAULT '',
            barcodes TEXT NOT NULL DEFAULT '',
            pictures TEXT NOT NULL DEFAULT '',
            videos TEXT NOT NULL DEFAULT '',
            countries TEXT NOT NULL DEFAULT '',
            length_cm NUMERIC(10,2) NOT NULL DEFAULT 0,
            width_cm NUMERIC(10,2) NOT NULL DEFAULT 0,
            height_cm NUMERIC(10,2) NOT NULL DEFAULT 0,
            weight_kg NUMERIC(10,3) NOT NULL DEFAULT 0,
            commodity_code TEXT NOT NULL DEFAULT '',
            tags TEXT NOT NULL DEFAULT '',
            price NUMERIC(12,2) NOT NULL DEFAULT 0,
            adult BOOLEAN NOT NULL DEFAULT FALSE,
            params TEXT NOT NULL DEFAULT '{}',
            status TEXT NOT NULL DEFAULT 'draft',
            market_status TEXT NOT NULL DEFAULT '',
            market_sku BIGINT,
            last_result TEXT NOT NULL DEFAULT '',
            sent_at TIMESTAMPTZ,
            updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            UNIQUE (account_id, offer_id)
        );
CREATE INDEX IF NOT EXISTS market_cards_product_idx ON market_cards (product_id);

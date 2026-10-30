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
ALTER TABLE products ADD COLUMN IF NOT EXISTS market_vendor TEXT NOT NULL DEFAULT '', ADD COLUMN IF NOT EXISTS market_countries TEXT NOT NULL DEFAULT '', ADD COLUMN IF NOT EXISTS market_length_cm NUMERIC(10,2) NOT NULL DEFAULT 0, ADD COLUMN IF NOT EXISTS market_width_cm NUMERIC(10,2) NOT NULL DEFAULT 0, ADD COLUMN IF NOT EXISTS market_height_cm NUMERIC(10,2) NOT NULL DEFAULT 0, ADD COLUMN IF NOT EXISTS market_weight_kg NUMERIC(10,3) NOT NULL DEFAULT 0, ADD COLUMN IF NOT EXISTS market_commodity_code TEXT NOT NULL DEFAULT '', ADD COLUMN IF NOT EXISTS market_tags TEXT NOT NULL DEFAULT '', ADD COLUMN IF NOT EXISTS market_adult BOOLEAN NOT NULL DEFAULT FALSE, ADD COLUMN IF NOT EXISTS market_category_id BIGINT, ADD COLUMN IF NOT EXISTS market_category_name TEXT NOT NULL DEFAULT '', ADD COLUMN IF NOT EXISTS market_params TEXT NOT NULL DEFAULT '{}';
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
CREATE TABLE IF NOT EXISTS app_settings (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL DEFAULT ''
        );
CREATE TABLE IF NOT EXISTS marking_codes (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            code TEXT NOT NULL UNIQUE,
            gtin TEXT NOT NULL,
            serial TEXT NOT NULL DEFAULT '',
            product_id INTEGER REFERENCES products(id) ON DELETE SET NULL,
            status TEXT NOT NULL DEFAULT 'new',
            print_count INTEGER NOT NULL DEFAULT 0,
            printed_at TIMESTAMPTZ,
            source TEXT NOT NULL DEFAULT 'import',
            order_id TEXT NOT NULL DEFAULT '',
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
CREATE INDEX IF NOT EXISTS marking_codes_product_idx ON marking_codes (product_id);
CREATE INDEX IF NOT EXISTS marking_codes_gtin_idx ON marking_codes (gtin);
CREATE INDEX IF NOT EXISTS marking_codes_status_idx ON marking_codes (status);
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
CREATE TABLE IF NOT EXISTS stock_documents (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            doc_key TEXT NOT NULL UNIQUE,
            kind TEXT NOT NULL DEFAULT 'other',
            comment TEXT NOT NULL DEFAULT '',
            username TEXT NOT NULL DEFAULT '',
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
INSERT INTO stock_documents (doc_key, kind, comment, created_at)
           SELECT g.doc_id,
                  CASE WHEN g.types = ARRAY['receipt']::text[] THEN 'receipt'
                       WHEN g.types = ARRAY['writeoff']::text[] THEN 'writeoff'
                       WHEN g.types <@ ARRAY['transfer_out', 'transfer_in']::text[] THEN 'transfer'
                       WHEN g.types = ARRAY['inventory_adjust']::text[] THEN 'inventory'
                       WHEN g.types = ARRAY['cancel']::text[] THEN 'cancel'
                       ELSE 'other' END,
                  COALESCE(g.first_comment, ''), g.first_at
             FROM (SELECT m.doc_id, array_agg(DISTINCT m.type ORDER BY m.type) AS types,
                          (array_agg(m.comment ORDER BY m.id))[1] AS first_comment, MIN(m.created_at) AS first_at
                     FROM stock_movements m LEFT JOIN stock_documents d ON d.doc_key = m.doc_id
                    WHERE m.doc_id IS NOT NULL AND d.id IS NULL
                    GROUP BY m.doc_id) g
            ORDER BY g.first_at
           ON CONFLICT (doc_key) DO NOTHING;
CREATE INDEX IF NOT EXISTS stock_documents_created_idx ON stock_documents (created_at DESC);
CREATE SEQUENCE IF NOT EXISTS internal_barcode_seq MINVALUE 1 MAXVALUE 99999999999 START 1;
CREATE OR REPLACE FUNCTION next_internal_ean13() RETURNS text
        LANGUAGE plpgsql SECURITY DEFINER SET search_path = public, pg_temp AS $$
        DECLARE n bigint; body text; s int; i int; code text;
        BEGIN
          LOOP
            n := nextval('internal_barcode_seq');
            body := '2' || lpad(n::text, 11, '0');
            s := 0;
            FOR i IN 1..12 LOOP
              s := s + (substr(body, i, 1))::int * (CASE WHEN i % 2 = 1 THEN 1 ELSE 3 END);
            END LOOP;
            code := body || ((10 - s % 10) % 10)::text;
            EXIT WHEN NOT EXISTS (SELECT 1 FROM product_barcodes WHERE barcode = code);
          END LOOP;
          RETURN code;
        END
        $$;
CREATE TABLE IF NOT EXISTS stocktakes (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            warehouse_id INTEGER NOT NULL REFERENCES warehouses(id) ON DELETE CASCADE,
            status TEXT NOT NULL DEFAULT 'open',
            started_by TEXT NOT NULL DEFAULT '',
            started_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            finished_at TIMESTAMPTZ,
            finished_by TEXT NOT NULL DEFAULT '',
            document_id INTEGER,
            zero_missing BOOLEAN NOT NULL DEFAULT TRUE
        );
CREATE UNIQUE INDEX IF NOT EXISTS stocktakes_one_open ON stocktakes (warehouse_id) WHERE status = 'open';
CREATE TABLE IF NOT EXISTS stocktake_items (
            stocktake_id INTEGER NOT NULL REFERENCES stocktakes(id) ON DELETE CASCADE,
            product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,
            counted INTEGER NOT NULL CHECK (counted >= 0),
            updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            updated_by TEXT NOT NULL DEFAULT '',
            PRIMARY KEY (stocktake_id, product_id)
        );
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
ALTER TABLE market_accounts ADD COLUMN IF NOT EXISTS writeoff_warehouse_id INTEGER REFERENCES warehouses(id) ON DELETE SET NULL;
CREATE TABLE IF NOT EXISTS order_writeoffs (
            id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            source TEXT NOT NULL,
            account_id INTEGER NOT NULL DEFAULT 0,
            order_ref TEXT NOT NULL,
            state TEXT NOT NULL DEFAULT 'pending',
            lines JSONB NOT NULL DEFAULT '[]'::jsonb,
            written JSONB NOT NULL DEFAULT '[]'::jsonb,
            warehouse_id INTEGER,
            doc_id INTEGER,
            return_doc_id INTEGER,
            note TEXT NOT NULL DEFAULT '',
            created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            written_at TIMESTAMPTZ,
            returned_at TIMESTAMPTZ,
            UNIQUE (source, account_id, order_ref)
        );
CREATE TABLE IF NOT EXISTS site_prices (
            model_key TEXT PRIMARY KEY,
            price NUMERIC(12,2) NOT NULL CHECK (price >= 0),
            sale_price NUMERIC(12,2) CHECK (sale_price IS NULL OR sale_price >= 0),
            sale_from DATE,
            sale_to DATE,
            updated_by TEXT NOT NULL DEFAULT '',
            updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );

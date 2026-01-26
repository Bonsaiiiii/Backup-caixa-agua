-- ============================================
-- EXTENSÕES
-- ============================================
CREATE EXTENSION IF NOT EXISTS "pgcrypto";
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- ============================================
-- TABELAS REGULARES
-- ============================================
CREATE TABLE user_maintable (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name VARCHAR(100) NOT NULL,
    number VARCHAR(20) UNIQUE NOT NULL CHECK (
        number ~ '^\+55[1-9][0-9][9][0-9]{8}$'
    ),
    apartamento VARCHAR(100) NOT NULL
);

CREATE TABLE esp_info (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    owner_id UUID REFERENCES user_maintable(id) ON DELETE SET NULL,
    mac TEXT UNIQUE NOT NULL CHECK(
        mac ~* '^([0-9A-F]{2}:){5}[0-9A-F]{2}$'
    ),
    password VARCHAR(255) NOT NULL,
    firmware_version VARCHAR(10) NOT NULL DEFAULT '1.0.0',
    notifications BOOLEAN DEFAULT true,
    premium BOOLEAN DEFAULT false,
    config_exemplo VARCHAR(255)
);

CREATE TABLE user_devices (
    user_id UUID NOT NULL REFERENCES user_maintable(id) ON DELETE CASCADE,
    esp_id UUID NOT NULL REFERENCES esp_info(id) ON DELETE CASCADE,
    PRIMARY KEY (user_id, esp_id)
);

-- ============================================
-- TABELA DE SÉRIE TEMPORAL
-- ============================================
CREATE TABLE esp_medicoes (
    id UUID DEFAULT gen_random_uuid(),
    esp_id UUID NOT NULL REFERENCES esp_info(id) ON DELETE CASCADE,
    data_hora TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    agua_nivel REAL NOT NULL,
    pressao REAL NOT NULL,
    fluxo REAL NOT NULL,
    fluxo_total REAL NOT NULL,
    alerta SMALLINT NOT NULL
);

-- ============================================
-- CONVERTER EM HYPERTABLE (TimescaleDB)
-- ============================================
SELECT create_hypertable(
    'esp_medicoes',
    'data_hora',
    chunk_time_interval => INTERVAL '1 day',
    if_not_exists => TRUE
);

-- ============================================
-- ÍNDICES OTIMIZADOS
-- ============================================
CREATE INDEX idx_esp_medicoes_esp_time ON esp_medicoes (esp_id, data_hora DESC);
CREATE INDEX idx_esp_medicoes_alerta ON esp_medicoes (alerta, data_hora DESC) 
    WHERE alerta > 0;
CREATE INDEX idx_esp_info_mac ON esp_info(mac);
CREATE INDEX idx_esp_info_owner ON esp_info(owner_id) WHERE owner_id IS NOT NULL;
CREATE INDEX idx_user_maintable_number ON user_maintable(number);
CREATE INDEX idx_user_devices_esp ON user_devices(esp_id);

-- ============================================
-- COMPRESSÃO AUTOMÁTICA (TimescaleDB)
-- ============================================
ALTER TABLE esp_medicoes SET (
    timescaledb.compress,
    timescaledb.compress_segmentby = 'esp_id',
    timescaledb.compress_orderby = 'data_hora DESC'
);

SELECT add_compression_policy('esp_medicoes', INTERVAL '7 days');

-- ============================================
-- RETENÇÃO AUTOMÁTICA (TimescaleDB)
-- ============================================
SELECT add_retention_policy('esp_medicoes', INTERVAL '1 year');

-- ============================================
-- AGREGAÇÕES CONTÍNUAS (TimescaleDB)
-- ============================================
CREATE MATERIALIZED VIEW esp_medicoes_hourly
WITH (timescaledb.continuous) AS
SELECT 
    time_bucket('1 hour', data_hora) AS hora,
    esp_id,
    AVG(agua_nivel) AS agua_nivel_avg,
    MIN(agua_nivel) AS agua_nivel_min,
    MAX(agua_nivel) AS agua_nivel_max,
    AVG(pressao) AS pressao_avg,
    AVG(fluxo) AS fluxo_avg,
    SUM(fluxo_total) AS fluxo_total_sum,
    COUNT(*) FILTER (WHERE alerta > 0) AS alertas_count,
    COUNT(*) AS medicoes_count
FROM esp_medicoes
GROUP BY hora, esp_id
WITH NO DATA;

-- Refresh automático (substitui pg_cron)
SELECT add_continuous_aggregate_policy(
    'esp_medicoes_hourly',
    start_offset => INTERVAL '3 hours',
    end_offset => INTERVAL '1 hour',
    schedule_interval => INTERVAL '1 hour'
);

-- ============================================
-- AGREGAÇÕES DIÁRIAS
-- ============================================
CREATE MATERIALIZED VIEW esp_medicoes_daily
WITH (timescaledb.continuous) AS
SELECT 
    time_bucket('1 day', hora) AS dia,
    esp_id,
    AVG(agua_nivel_avg) AS agua_nivel_avg,
    MIN(agua_nivel_min) AS agua_nivel_min,
    MAX(agua_nivel_max) AS agua_nivel_max,
    SUM(fluxo_total_sum) AS consumo_dia,
    SUM(alertas_count) AS alertas_dia
FROM esp_medicoes_hourly
GROUP BY dia, esp_id
WITH NO DATA;

SELECT add_continuous_aggregate_policy(
    'esp_medicoes_daily',
    start_offset => INTERVAL '3 days',
    end_offset => INTERVAL '1 day',
    schedule_interval => INTERVAL '1 day'
);

-- ============================================
-- LIMPEZA DE USUÁRIOS ÓRFÃOS (Procedimento)
-- ============================================
-- Opção 1: Criar função e usar TimescaleDB job
CREATE OR REPLACE FUNCTION cleanup_orphan_users()
RETURNS INTEGER AS $$
DECLARE
    deleted_count INTEGER;
BEGIN
    DELETE FROM user_maintable 
    WHERE id NOT IN (
        SELECT DISTINCT user_id FROM user_devices
    );
    GET DIAGNOSTICS deleted_count = ROW_COUNT;
    RETURN deleted_count;
END;
$$ LANGUAGE plpgsql;

-- Agendar com TimescaleDB (roda semanalmente às 3 AM domingo)
SELECT add_job('cleanup_orphan_users', '7 days', initial_start => '2025-12-07 03:00:00');

-- ============================================
-- COMENTÁRIOS
-- ============================================
COMMENT ON TABLE esp_info IS 'Tabela com informações dos dispositivos (ESP)';
COMMENT ON TABLE user_maintable IS 'Tabela principal de usuários';
COMMENT ON TABLE esp_medicoes IS 'Medições em série temporal gerenciadas por TimescaleDB';

'''
Nova call de metabase

SELECT
  "source"."Data/Hora" AS "Data/hora",
  "source"."Nível de água" AS "Nível de água",
  "source"."Pressão (PA)" AS "Pressão (PA)",
  "source"."Fluxo (Litro por minuto)" AS "Fluxo (Litro por minuto)",
  "source"."Quantidade de medições" AS "Quantidade de medições"
FROM
  (
    SELECT
      -- Use time_bucket for efficient TimescaleDB aggregation
      CASE
        WHEN LOWER({{intervalo_de_tempo}}) = 'última hora' THEN 
          time_bucket('1 minute', m.data_hora)
        WHEN LOWER({{intervalo_de_tempo}}) = 'último dia' THEN 
          time_bucket('5 minutes', m.data_hora)
        WHEN LOWER({{intervalo_de_tempo}}) = 'última semana' THEN 
          time_bucket('15 minutes', m.data_hora)
        WHEN LOWER({{intervalo_de_tempo}}) = 'último mês' THEN 
          time_bucket('6 hours', m.data_hora)
        WHEN LOWER({{intervalo_de_tempo}}) = 'último ano' THEN 
          time_bucket('1 day', m.data_hora)
        ELSE 
          time_bucket('1 minute', m.data_hora)
      END AS "Data/Hora",
      AVG(-1 * m.agua_nivel) AS "Nível de água",
      AVG(m.pressao) AS "Pressão (PA)",
      AVG(m.fluxo) AS "Fluxo (Litro por minuto)",
      COUNT(*) AS "Quantidade de medições"
    FROM
      "public"."esp_medicoes" AS m
      JOIN "public"."esp_info" AS info ON m.esp_id = info.id
    WHERE
      info.mac = {{mac}}
      AND info.password = {{password}}
      AND m.data_hora >= CASE
        WHEN LOWER({{intervalo_de_tempo}}) = 'última hora' THEN NOW() - INTERVAL '4 hour'
        WHEN LOWER({{intervalo_de_tempo}}) = 'último dia' THEN NOW() - INTERVAL '1 day 3 hour'
        WHEN LOWER({{intervalo_de_tempo}}) = 'última semana' THEN NOW() - INTERVAL '1 week 3 hour'
        WHEN LOWER({{intervalo_de_tempo}}) = 'último mês' THEN NOW() - INTERVAL '1 month 3 hour'
        WHEN LOWER({{intervalo_de_tempo}}) = 'último ano' THEN NOW() - INTERVAL '1 year 3 hour'
        ELSE NOW() - INTERVAL '1 hour'
      END
    GROUP BY
      1
    ORDER BY
      1 ASC
  ) AS "source"
LIMIT
  10000;
'''
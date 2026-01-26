import psycopg2
from psycopg2.extras import RealDictCursor
from datetime import datetime

def database_conn():
    return psycopg2.connect(
        database="mydatabase",
        user="myuser",
        password="mypassword",
        host="localhost",
        port=5431
    )

def get_esp_info(mac_addr):
    conn = database_conn()
    cur = conn.cursor()
    
    cur.execute("SELECT mac, password, id FROM esp_info WHERE mac = %s", (mac_addr,))
    result = cur.fetchone()

    cur.close()
    conn.close()

    return result  # returns tuple (mac, password, id) or None

def insert_measurement(esp_id, data_hora, distancia, pressao, fluxo, fluxo_total):
    conn = database_conn()
    cur = conn.cursor()

    cur.execute("""
        INSERT INTO esp_medicoes (esp_id, data_hora, agua_nivel, pressao, fluxo, fluxo_total, alerta)
        VALUES (%s, %s, %s, %s, %s, %s, %s)
    """, (
        esp_id,
        data_hora,
        distancia,
        pressao,
        fluxo,
        fluxo_total,
        0
    ))

    conn.commit()
    cur.close()
    conn.close()
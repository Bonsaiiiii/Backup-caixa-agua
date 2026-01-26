import requests
import psycopg2
from datetime import datetime
import logging

# Configuração
WORDPRESS_URL = "https://hugenplus.wpcomstaging.com"
API_TOKEN = "seu-token-secreto-aqui"  # Mesmo do WordPress

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

def get_db():
    return psycopg2.connect(
        host="localhost",
        database="mydatabase",
        user="myuser",
        password="mypassword",
        port="5431"
    )

def sync_all_devices_premium_status():
    """Sincroniza status premium de todos os devices com WordPress"""
    
    conn = get_db()
    cursor = conn.cursor()
    
    try:
        # Busca todos os MACs cadastrados
        cursor.execute("SELECT mac FROM esp_info WHERE mac IS NOT NULL")
        devices = cursor.fetchall()
        
        if not devices:
            logger.info("Nenhum device encontrado")
            return
        
        mac_list = [device[0] for device in devices]
        logger.info(f"Sincronizando {len(mac_list)} devices...")
        
        # Consulta WordPress em lote (mais eficiente)
        response = requests.post(
            f"{WORDPRESS_URL}/wp-json/hugenplus/v1/devices/status",
            headers={
                "Authorization": f"Bearer {API_TOKEN}",
                "Content-Type": "application/json",
            },
            json={"macs": mac_list},
            timeout=30,
        )
        
        if response.status_code != 200:
            logger.error(f"Erro ao consultar WordPress: {response.status_code}")
            return
        
        data = response.json()
        
        # Atualiza cada device no banco
        updated_count = 0
        premium_count = 0
        
        for device_data in data['devices']:
            mac = device_data['mac']
            premium = device_data['premium']
            found = device_data.get('found', False)
            
            if not found:
                logger.warning(f"Device {mac} não encontrado no WordPress")
                continue
            
            cursor.execute("""
                UPDATE esp_info 
                SET premium = %s
                WHERE UPPER(mac) = UPPER(%s)
            """, (premium, mac))
            
            updated_count += 1
            if premium:
                premium_count += 1
            
            logger.info(f"✅ {mac} - Premium: {premium}")
        
        conn.commit()
        
        logger.info(f"""
        Sincronização concluída:
        - Total de devices: {len(mac_list)}
        - Atualizados: {updated_count}
        - Premium ativos: {premium_count}
        - Timestamp: {datetime.now()}
        """)
        
    except requests.exceptions.RequestException as e:
        logger.error(f"Erro de conexão com WordPress: {e}")
    except Exception as e:
        logger.error(f"Erro inesperado: {e}")
        conn.rollback()
    finally:
        cursor.close()
        conn.close()

def sync_single_device(mac):
    """Sincroniza um device específico (útil para testes)"""
    
    try:
        response = requests.get(
            f"{WORDPRESS_URL}/wp-json/hugenplus/v1/device/{mac}/status",
            headers={"Authorization": f"Bearer {API_TOKEN}"},
            timeout=15
        )
        
        if response.status_code == 404:
            logger.warning(f"Device {mac} não encontrado no WordPress")
            return False
        
        if response.status_code != 200:
            logger.error(f"Erro HTTP {response.status_code}")
            return False
        
        data = response.json()
        
        conn = get_db()
        cursor = conn.cursor()
        
        cursor.execute("""
            UPDATE esp_info 
            SET premium = %s
            WHERE UPPER(mac) = UPPER(%s)
        """, (data['premium'], mac))
        
        conn.commit()
        cursor.close()
        conn.close()
        
        logger.info(f"✅ Device {mac} sincronizado - Premium: {data['premium']}")
        return True
        
    except Exception as e:
        logger.error(f"Erro ao sincronizar {mac}: {e}")
        return False

if __name__ == "__main__":
    # Roda sincronização completa
    sync_all_devices_premium_status()
    
    # Ou testa um device específico:
    # sync_single_device("AA:BB:CC:DD:EE:FF")
""" Código responsável por escutar mensagens enviadas por dispositivos ESP e registrar
no banco de dados """

import os
import json
import threading
import requests
import paho.mqtt.client as mqtt
from database_conn import get_esp_info, insert_measurement

API_KEY = os.getenv("API_KEY", "O?7cIxjWaXdL*j-I?N!j:6:ShG}EqIjA}=}8-6LB0o7*Wwy@pAH9O]pmJ*wF2)-d")

estados_alerta = {}

# ----- MQTT CALLBACKS -----

def on_connect(client, userdata, flags, rc):
    """ Função responsável por conectar esse código com o BROKER MQTT """
    if rc == 0:
        print("✅ Conectado com sucesso ao broker MQTT.")
        client.subscribe("esp/+/data")  # resubscribe on reconnect
    else:
        print(f"❌ Falha na conexão. Código de retorno: {rc}")

def on_disconnect(client, userdata, rc):
    """ Função responsável por avisar a desconexão com o broker e tentar reconectar-se """
    print(f"⚠️ Desconectado do broker. Código de retorno: {rc}")
    try:
        client.reconnect()
    except Exception as e:
        print(f"Erro ao tentar reconectar: {e}")

def on_message(client, userdata, msg):
    """ Função responsável por tratar mensagens recebidas (JSONDecode) """
    try:
        message = msg.payload.decode('utf-8')
        data = json.loads(message)
        print(f"📩 Mensagem recebida no tópico {msg.topic}: {data}")

        # Run handler in separate thread
        threading.Thread(target=handle_message, args=(data,), daemon=True).start()

    except json.JSONDecodeError as e:
        print(f"❌ Erro ao decodificar JSON: {e}")
        print(f"Mensagem bruta: {msg.payload}")

# ----- HANDLER FOR PROCESSING DATA -----

def handle_message(data):
    """ Função responsável por lidar com as mensagens recebidas nos tópicos dos dispositivos """
    try:
        if 'mac_addr' not in data:
            print("⚠️ Payload incompleto: 'mac_addr' ausente.")
            return

        mac_addr = data.get("mac_addr")
        pasw = data.get("pasw")
        esp_nivel = data.get("distancia")
        esp_fluxo = data.get("fluxo_agua")
        esp_fluxo_total = data.get("fluxo_total")
        esp_pressao = data.get("pressao")
        data_hora = data.get("data_hora")
        aviso = data.get("aviso")

        ultimo_aviso = estados_alerta.get(mac_addr)

        print(f"➡️ Processando dados do ESP: {mac_addr}")

        esp_info = get_esp_info(mac_addr)
        if esp_info:
            esp_info_mac, esp_info_pasw, esp_info_id = esp_info

            if esp_info_mac == mac_addr and pasw == esp_info_pasw:
                print("✅ ESP autenticado. Inserindo dados no banco.")
                insert_measurement(esp_info_id, data_hora, esp_nivel, esp_pressao, esp_fluxo,
                esp_fluxo_total)
            else:
                print("❌ Senha inválida para o ESP.")
        else:
            print("❌ ESP não encontrado no banco de dados.")

        print(aviso, ultimo_aviso)
        if aviso != ultimo_aviso and aviso is not None:
            print(f"Estado de aviso mudado do {mac_addr}: {ultimo_aviso} -> {aviso}")
            try:
                response = requests.post(
                    "http://" + BROKER_ADDRESS + ":5000/send-warning",
                    json = {
                        "mac": mac_addr,
                        "aviso": aviso,
                        "ultimo_aviso": ultimo_aviso
                    },
                    headers = {"x-api-key": API_KEY},
                    timeout = 5
                )

                if response.status_code == 200:
                    print("✅ Warning notification sent")
                    estados_alerta[mac_addr] = aviso
                else:
                    print(f"❌ Failed to send warning: {response.text}")

            except requests.exceptions.RequestException as e:
                print(f"❌ Error calling API: {e}")

    except Exception as e:
        print(f"Erro ao processar mensagem: {e}")

# ----- MQTT SETUP -----

BROKER_ADDRESS = "testeagua.ddns.net"

client = mqtt.Client()
client.on_connect = on_connect
client.on_disconnect = on_disconnect
client.on_message = on_message

# Optional: helps with unstable networks
client.reconnect_delay_set(min_delay=1, max_delay=120)

print("🔌 Conectando ao broker...")
client.connect(BROKER_ADDRESS, 1883, keepalive=60)

# Start the loop in a background thread (non-blocking)
client.loop_start()

# Keep the main thread alive to avoid script exiting
try:
    while True:
        pass
except KeyboardInterrupt:
    print("🛑 Encerrando conexão MQTT...")
    client.loop_stop()
    client.disconnect()

# netstat -an | findstr 1883netstat -an | findstr 1883 ouvir a porta 1883

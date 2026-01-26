import json
import os
from fastapi import FastAPI, HTTPException, Header, Depends, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from pydantic import BaseModel
import psycopg2
from psycopg2.extras import RealDictCursor
from typing import Optional, Dict
import paho.mqtt.client as mqtt
import threading
import time
import requests

#& C:/Users/Hugen/AppData/Local/Microsoft/WindowsApps/python3.13.exe -m uvicorn python_press_comm:app --reload --host 0.0.0.0 --port 5000

#from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials 
#import jwt
#from datetime import datetime, timedelta

from slowapi import Limiter, _rate_limit_exceeded_handler
from slowapi.util import get_remote_address
from slowapi.errors import RateLimitExceeded

app = FastAPI()

limiter = Limiter(key_func=get_remote_address)
app.state.limiter = limiter
app.add_exception_handler(RateLimitExceeded, _rate_limit_exceeded_handler)

API_KEY = os.getenv("API_KEY", "O?7cIxjWaXdL*j-I?N!j:6:ShG}EqIjA}=}8-6LB0o7*Wwy@pAH9O]pmJ*wF2)-d")

async def verify_api_key(x_api_key: str = Header(...)):
    if x_api_key != API_KEY:
        raise HTTPException(status_code=401, detail="Invalid API key")
    return x_api_key

app.add_middleware(
    CORSMiddleware,
    allow_origins=["https://hugenplus.wpcomstaging.com", "https://www.hugenplus.wpcomstaging.com"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Store warning states to prevent duplicate messages
device_warning_states: Dict[str, int] = {}

class ConfigRequest(BaseModel):
    mac: str
    password: str

class ProgramRequest(BaseModel):
    mac: str
    senha: str = None
    config: str = None

class UserRequest(BaseModel):
    name: str
    number: str
    apartamento: str
    mac: Optional[str] = None

class ListUsersRequest(BaseModel):
    mac: Optional[str] = None

class VerifyDeviceRequest(BaseModel):
    mac: str
    password: str

class DeleteRequest(BaseModel):
    mac: str
    number: str

class WarningRequest(BaseModel):
    mac: str
    aviso: int
    ultimo_aviso: Optional[int] = None

def database_conn():
    return psycopg2.connect(
        database=os.getenv("POSTGRES_DB", "mydatabase"),
        user=os.getenv("POSTGRES_USER", "myuser"),
        password=os.getenv("POSTGRES_PASSWORD", "mypassword"),
        host=os.getenv("POSTGRES_HOST", "localhost"),
        port=os.getenv("POSTGRES_PORT", "5431")
    )

# MQTT Handler for sending messages directly
class ESPMessageHandler:
    def __init__(self):
        self.broker = os.getenv("MQTT_BROKER", "localhost")
        self.port = int(os.getenv("MQTT_PORT", "1883"))
        self.confirmation_received = False
        self.status_data = {}
        
    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            client.subscribe("esp/+/confirm")
        
    def on_message(self, client, userdata, msg):
        payload = msg.payload.decode()
        if msg.topic:
            self.confirmation_received = True
            self.status_data = {
                "type": "confirmation",
                "message": payload,
                "topic": msg.topic,
                "timestamp": time.time()
            }
    
    def send_message_and_wait(self, mac, message, timeout=10):
        """Send message and wait for confirmation"""
        try:
            client = mqtt.Client()
            client.on_connect = self.on_connect
            client.on_message = self.on_message
            
            client.connect(self.broker, self.port, 60)
            client.loop_start()
            
            time.sleep(0.5)
            
            topic = f"esp/{mac}/config"
            result = client.publish(topic, message)
            
            if result.rc != mqtt.MQTT_ERR_SUCCESS:
                return {"status": "error", "message": "Failed to publish message"}
            
            start_time = time.time()
            while not self.confirmation_received and (time.time() - start_time) < timeout:
                time.sleep(0.1)
            
            client.loop_stop()
            client.disconnect()
            
            if self.confirmation_received:
                return {"status": "success", "data": self.status_data}
            else:
                return {"status": "timeout", "message": "ESP32 did not respond within timeout period"}
                
        except ConnectionRefusedError:
            return {"status": "error", "message": "MQTT broker connection refused. Is the broker running?"}
        except Exception as e:
            return {"status": "error", "message": f"Exception: {str(e)}"}

    def check_status(self, mac, timeout=5):
        """Check ESP32 status"""
        try:
            client = mqtt.Client()
            client.on_connect = self.on_connect
            client.on_message = self.on_message
            
            client.connect(self.broker, self.port, 60)
            client.loop_start()
            
            time.sleep(0.5)
            
            topic = f"esp/{mac}/config"
            client.publish(topic, "Ping")
            
            start_time = time.time()
            while not self.status_data and (time.time() - start_time) < timeout:
                time.sleep(0.1)
            
            client.loop_stop()
            client.disconnect()
            
            if self.status_data:
                return {"status": "online", "data": self.status_data}
            else:
                return {"status": "offline", "message": "No response from the device"}
                
        except ConnectionRefusedError:
            return {"status": "error", "message": "MQTT broker connection refused"}
        except Exception as e:
            return {"status": "error", "message": str(e)}

    def update_firmware(self, mac, timeout=5):
        """Send firmware update command to ESP32"""
        try:
            client = mqtt.Client()  # ← Fixed: was client.mqtt.Client()
            client.on_connect = self.on_connect
            client.on_message = self.on_message
            
            client.connect(self.broker, self.port, 60)
            client.loop_start()

            time.sleep(0.5)

            topic = f"esp/{mac}/config"
            result = client.publish(topic, "Firmware Update")
            
            if result.rc != mqtt.MQTT_ERR_SUCCESS:
                return {"status": "error", "message": "Failed to publish message"}

            start_time = time.time()

            while not self.confirmation_received and (time.time() - start_time) < timeout:
                time.sleep(0.1)
            
            client.loop_stop()
            client.disconnect()

            if self.confirmation_received:
                return {"status": "success", "data": self.status_data}
            else:
                return {"status": "timeout", "message": "No response from the device"}
                
        except ConnectionRefusedError:
            return {"status": "error", "message": "MQTT broker connection refused"}
        except Exception as e:
            return {"status": "error", "message": str(e)}


@app.post("/programar")
@limiter.limit("5/minute")
def programar(request: Request, data: ProgramRequest, api_key: str = Depends(verify_api_key)):
    print(f"Received MAC Address: {data.mac}")
    print(f"Sending config: {data.config}")
    
    message = f"Update:{data.config}" if data.config else "Update:default"
    handler = ESPMessageHandler()
    result = handler.send_message_and_wait(data.mac, message)
    
    return {
        "status": result.get("status", "unknown"),
        "confirmation": result.get("data"),
        "message": result.get("message", ""),
        "mac": data.mac
    }

@app.post("/check-status")
@limiter.limit("5/minute")
def check_status(request: Request, data: ProgramRequest, api_key: str = Depends(verify_api_key)):
    """Check if ESP32 is online and responsive"""
    print(f"Checking status for MAC: {data.mac}")
    
    handler = ESPMessageHandler()
    result = handler.check_status(data.mac)

    print(f'result.type: {data.mac}')
    
    print(f"Status check result: {result}")
    msg = result.get("data", {}).get("message", "")
    
    return {
        "online": result.get("status") == "online",
        "status": result.get("status"),
        "details": result.get("data"),
        "message": result.get("message", ""),
        "firmware_version": msg.split(":")[-1][-5:]
    }

@app.post("/get-config")
@limiter.limit("5/minute")
def get_config(request: Request, data: ConfigRequest, api_key: str = Depends(verify_api_key)):
    try:
        conn = database_conn()
        cur = conn.cursor(cursor_factory=RealDictCursor)
        mac = data.mac.upper()

        cur.execute("""
            SELECT config_exemplo, password
            FROM esp_info
            WHERE mac = %s
        """, (mac,))

        device = cur.fetchone()
        cur.close()
        conn.close()

        if device and device["password"] == data.password:
            print ({"config": device["config_exemplo"]})
            return {"config": device["config_exemplo"]}

        return {"config": "unauthorized"}

    except Exception as e:
        print(f"Error: {e}")
        return {"config": "error"}

@app.post("/list-users")
@limiter.limit("5/minute")
def list_users(request: Request, data: ListUsersRequest = None, api_key: str = Depends(verify_api_key)):
    """Get users filtered by MAC address"""
    try:
        conn = database_conn()
        cur = conn.cursor(cursor_factory=RealDictCursor)
        
        mac = data.mac if data else None
        
        if mac:
            cur.execute("""
                SELECT 
                    u.id, 
                    u.name, 
                    u.number, 
                    u.apartamento
                FROM user_maintable u
                INNER JOIN user_devices ud ON u.id = ud.user_id
                INNER JOIN esp_info e ON ud.esp_id = e.id
                WHERE e.mac = %s
                ORDER BY u.name
            """, (mac.upper(),))
        else:
            cur.execute("""
                SELECT id, name, number, apartamento
                FROM user_maintable
                ORDER BY name
            """)
        
        users = cur.fetchall()
        cur.close()
        conn.close()
        
        return {"success": True, "users": [dict(user) for user in users]}
    except Exception as e:
        print(f"Error listing users: {e}")
        return {"success": False, "error": str(e)}

@app.post("/add-user")
@limiter.limit("15/minute")
def add_user(request: Request, data: UserRequest, api_key: str = Depends(verify_api_key)):
    """Add a new user and optionally link to a device"""
    try:
        conn = database_conn()
        cur = conn.cursor(cursor_factory=RealDictCursor)
        
        cur.execute("""
            INSERT INTO user_maintable (name, number, apartamento)
            VALUES (%s, %s, %s)
            ON CONFLICT (number) DO UPDATE
                SET name = EXCLUDED.name,
                    apartamento = EXCLUDED.apartamento
            RETURNING id, name, number, apartamento
        """, (data.name, data.number, data.apartamento))
        
        new_user = cur.fetchone()
        user_id = new_user['id']
        
        if data.mac:
            mac_upper = data.mac.upper()
            
            cur.execute("SELECT id FROM esp_info WHERE mac = %s", (mac_upper,))
            esp_device = cur.fetchone()
            
            if esp_device:
                cur.execute("""
                    INSERT INTO user_devices (user_id, esp_id)
                    VALUES (%s, %s)
                    ON CONFLICT DO NOTHING
                """, (user_id, esp_device['id']))
                
                conn.commit()
                cur.close()
                conn.close()
                return {
                    "success": True, 
                    "user": dict(new_user), 
                    "message": f"Usuário adicionado e vinculado ao dispositivo {mac_upper}"
                }
            else:
                conn.commit()
                cur.close()
                conn.close()
                return {
                    "success": True, 
                    "user": dict(new_user), 
                    "message": "Usuário adicionado, mas dispositivo não encontrado"
                }
        
        conn.commit()
        cur.close()
        conn.close()
        
        return {"success": True, "user": dict(new_user), "message": "Usuário adicionado com sucesso"}
        
    except psycopg2.IntegrityError as e:
        if 'number' in str(e):
            return {"success": False, "error": "Este número de telefone já está cadastrado"}
        return {"success": False, "error": "Erro de integridade nos dados"}
    except Exception as e:
        print(f"Error adding user: {e}")
        return {"success": False, "error": str(e)}

@app.post("/delete-user")
@limiter.limit("15/minute")
def delete_user(request: Request, data: DeleteRequest, api_key: str = Depends(verify_api_key)):
    """Delete a user from the database"""
    try:
        conn = database_conn()
        cur = conn.cursor(cursor_factory=RealDictCursor)
        
        cur.execute("SELECT id FROM esp_info WHERE mac = %s", (data.mac,))
        esp_id = cur.fetchone()

        cur.execute("SELECT id, name FROM user_maintable WHERE number = %s", (data.number,))
        user = cur.fetchone()

        user_id = user["id"]
        user_name = user["name"]
        
        print(esp_id["id"], user_id)

        if not esp_id:
            cur.close()
            conn.close()
            return {"success": False, "error": "Dispositivo não encontrado"}

        if not user_id:
            cur.close()
            conn.close()
            return {"success": False, "error": "Usuário não encontrado"}
        
        cur.execute("DELETE FROM user_devices WHERE user_id = %s AND esp_id = %s", (user_id, esp_id["id"]))
        conn.commit()
        cur.close()
        conn.close()
        
        return {"success": True, "message": f"Usuário {user_name} removido com sucesso"}
    except Exception as e:
        print(f"Error deleting user: {e}")
        return {"success": False, "error": str(e)}

@app.post("/verify-device")
@limiter.limit("5/minute")
def verify_device(request: Request, data: VerifyDeviceRequest, api_key: str = Depends(verify_api_key)):
    try:
        conn = database_conn()
        cur = conn.cursor(cursor_factory=RealDictCursor)
        mac = data.mac.upper()

        cur.execute("""
            SELECT config_exemplo, password
            FROM esp_info
            WHERE mac = %s
        """, (mac,))

        device = cur.fetchone()
        cur.close()
        conn.close()

        if device and device["password"] == data.password:
            # ✅ Success response for WordPress
            return {
                "valid": True,
                "device_name": f"Device {mac}",
                "config": device["config_exemplo"]
            }

        # ❌ Invalid credentials
        return {
            "valid": False,
            "message": "Invalid credentials"
        }

    except Exception as e:
        print(f"Error: {e}")
        return {
            "valid": False,
            "message": "Server error"
        }

@app.post("/try-firmware-update")
@limiter.limit("3/minute")
def try_firmware_update(request: Request, data: ProgramRequest, api_key: str = Depends(verify_api_key)):
    """Send firmware update command to ESP32 and wait for confirmation"""
    try:
        print(f"Firmware update request for MAC: {data.mac}")
        
        # Verify device credentials first
        conn = database_conn()
        cur = conn.cursor(cursor_factory=RealDictCursor)
        mac = data.mac.upper()

        cur.execute("""
            SELECT password
            FROM esp_info
            WHERE mac = %s
        """, (mac,))

        device = cur.fetchone()
        cur.close()
        conn.close()

        # Check if device exists and password matches
        if not device or device["password"] != data.senha:
            return {
                "status": "error",
                "message": "Invalid device credentials"
            }

        # Send firmware update command via MQTT
        message = "Firmware Update"
        handler = ESPMessageHandler()
        result = handler.send_message_and_wait(data.mac, message)
        
        print(f"Firmware update result: {result}")
        
        return {
            "status": result.get("status", "unknown"),
            "confirmation": result.get("data"),
            "message": result.get("message", ""),
            "mac": data.mac
        }

    except Exception as e:
        print(f"Error in firmware update: {e}")
        return {
            "status": "error",
            "message": f"Server error: {str(e)}"
        }

@app.get("/firmware-update")
@limiter.limit("3/minute")
def firmware_update(request: Request):
    firmware_path = r"C:\Users\Hugen\Documents\firmware_v1.1.0.bin"
    
    if not os.path.exists(firmware_path):
        raise HTTPException(status_code=404, detail="Firmware file not found")
    
    print(f"ESP32 requesting firmware update from: {request.client.host}")
    
    # Get file size
    file_size = os.path.getsize(firmware_path)
    print(f"Firmware size: {file_size} bytes ({file_size/1024:.2f} KB)")
    
    # Create response with explicit headers
    response = FileResponse(
        path=firmware_path,
        media_type="application/octet-stream",
        filename="firmware_v1.1.0.bin"
    )
    
    # CRITICAL: Explicitly set Content-Length header
    response.headers["Content-Length"] = str(file_size)
    response.headers["Accept-Ranges"] = "bytes"
    response.headers["Cache-Control"] = "no-cache"
    
    return response

@app.get("/teste")
async def get_message(request: Request):
    try:
        # Read raw bytes from the request
        body_bytes = await request.body()
        print("Raw body:", body_bytes)

        # Try to decode JSON (optional)
        message = json.loads(body_bytes)
        print("Decoded JSON:", message)

        return {"status": "received", "message": message}

    except json.JSONDecodeError as e:
        print(f"Erro ao decodificar mensagem: {e}")
        print(f"Mensagem bruta: {body_bytes}")

        return {"error": "invalid JSON", "raw": body_bytes.decode("utf-8", errors="ignore")}

@app.post("/send-warning")
@limiter.limit("10/minute")
async def send_warning(
    request: Request, 
    data: WarningRequest, 
    api_key: str = Depends(verify_api_key)
):
    """Send WhatsApp warning message when device warning state changes"""
    try:
        conn = database_conn()
        cur = conn.cursor(cursor_factory=RealDictCursor)
        
        # Get device info and associated users
        cur.execute("""
            SELECT 
                e.mac,
                e.id as esp_id,
                u.name,
                u.number
            FROM esp_info e
            INNER JOIN user_devices ud ON e.id = ud.esp_id
            INNER JOIN user_maintable u ON ud.user_id = u.id
            WHERE e.mac = %s
        """, (data.mac.upper(),)) 

        cur.execute("""
            SELECT id FROM esp_info WHERE mac = %s
        """, (data.mac.upper(),))

        device = cur.fetchone()
        device_id = device["id"]

        print(f"Device ID: {device_id}")

        cur.execute("""
            SELECT user_id FROM user_devices WHERE esp_id = %s
        """, (device_id,))

        user_ids_rows = cur.fetchall()
        user_ids = [row["user_id"] for row in user_ids_rows]  # ✅ Extract user_ids into a list

        print(f"User IDs: {user_ids}")

        # ✅ Use IN operator with tuple of IDs
        cur.execute("""
            SELECT number, name FROM user_maintable WHERE id = ANY(%s::uuid[])
        """, (user_ids,))

        users = cur.fetchall()

        print(f"Numbers: {[user['number'] for user in users]}")

        cur.close()
        conn.close()
        
        """if not users:
            return {"success": False, "error": "No users found for this device"}
        
        # Determine warning message
        ultimo_aviso = {
            0: "✅ Sistema normalizado",
            1: "⚠️ ALERTA: Nível de água baixo!",
            2: "🚨 CRÍTICO: Vazamento detectado!",
            3: "⚠️ ATENÇÃO: Pressão anormal!"
        }
        
        message = warning_messages.get(data.aviso, f"⚠️ Alerta {data.aviso}")
        
        # Send WhatsApp message to all users
        sent_count = 0
        for user in users:
            try:
                # Replace with your WhatsApp API call
                whatsapp_response = send_whatsapp_message(
                    phone=user['number'],
                    message=f"{message}\n\nDispositivo: {data.mac}\nUsuário: {user['name']}"
                )
                
                if whatsapp_response:
                    sent_count += 1
                    print(f"✅ Message sent to {user['name']} ({user['number']})")
                    
            except Exception as e:
                print(f"❌ Failed to send to {user['name']}: {e}")
        
        # Update warning state
        device_warning_states[data.mac] = data.aviso
        
        return {
            "success": True,
            "message": f"Warning sent to {sent_count} user(s)",
            "warning_type": data.aviso,
            "users_notified": sent_count
        } """
        
    except Exception as e:
        print(f"Error sending warning: {e}")
        return {"success": False, "error": str(e)}

def send_whatsapp_message(phone: str, message: str):
    """
    Send WhatsApp message using your preferred service
    Examples: Twilio, WhatsApp Business API, Evolution API, etc.
    """
    # Example with Twilio
    # from twilio.rest import Client
    # client = Client(account_sid, auth_token)
    # message = client.messages.create(
    #     from_='whatsapp:+14155238886',
    #     body=message,
    #     to=f'whatsapp:+{phone}'
    # )
    # return message.sid
    
    # Example with Evolution API (popular in Brazil)
    # response = requests.post(
    #     "https://your-evolution-api.com/message/sendText/instance",
    #     json={
    #         "number": phone,
    #         "text": message
    #     },
    #     headers={"apikey": "your-key"}
    # )
    # return response.status_code == 200
    
    print(f"📱 WhatsApp message: {phone} -> {message}")
    return True  # Replace with actual implementation
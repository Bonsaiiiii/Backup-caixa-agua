import paho.mqtt.client as mqtt
import time
import json
import sys

class ESPMessageHandler:
    def __init__(self, broker="localhost", port=1883):
        self.broker = broker
        self.port = port
        self.client = mqtt.Client()
        self.confirmation_received = False
        self.status_data = {}
        
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        
    def on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            # Subscribe to all ESP32 confirmation topics
            client.subscribe("esp/+/confirm")
        else:
            print(json.dumps({"status": "error", "message": f"MQTT connection failed with code {rc}"}))
        
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
            self.client.connect(self.broker, self.port, 60)
            self.client.loop_start()
            
            # Wait a bit for connection to establish
            time.sleep(0.5)
            
            # Publish message
            topic = f"esp/{mac}/config"
            result = self.client.publish(topic, message)
            
            if result.rc != mqtt.MQTT_ERR_SUCCESS:
                return {"status": "error", "message": "Failed to publish message"}
            
            # Wait for confirmation
            start_time = time.time()
            while not self.confirmation_received and (time.time() - start_time) < timeout:
                time.sleep(0.1)
            
            self.client.loop_stop()
            self.client.disconnect()
            
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
            self.client.connect(self.broker, self.port, 60)
            self.client.loop_start()
            
            # Wait for connection
            time.sleep(0.5)
            
            # Send ping
            topic = f"esp/{mac}/config"
            self.client.publish(topic, "Ping")
            
            # Wait for status response
            start_time = time.time()
            while not self.status_data and (time.time() - start_time) < timeout:
                time.sleep(0.1)
            
            self.client.loop_stop()
            self.client.disconnect()
            
            if self.status_data:
                return {"status": "online", "data": self.status_data}
            else:
                return {"status": "offline", "message": "No response from ESP32"}
                
        except ConnectionRefusedError:
            return {"status": "error", "message": "MQTT broker connection refused"}
        except Exception as e:
            return {"status": "error", "message": str(e)}

if __name__ == "__main__":
    # Ensure we always output valid JSON
    try:
        if len(sys.argv) < 3:
            result = {"status": "error", "message": "Usage: python message_logger.py <mac> <message>"}
            print(json.dumps(result))
            sys.exit(1)
        
        mac = sys.argv[1]
        message = sys.argv[2]
        
        handler = ESPMessageHandler()
        
        # Check if this is a status check (Ping) or a message
        if message.lower() == "ping":
            result = handler.check_status(mac)
        else:
            result = handler.send_message_and_wait(mac, message)
        
        # Always output valid JSON
        print(json.dumps(result))
        
    except Exception as e:
        # Even if there's an error, output valid JSON
        error_result = {"status": "error", "message": str(e)}
        print(json.dumps(error_result))
        sys.exit(1)
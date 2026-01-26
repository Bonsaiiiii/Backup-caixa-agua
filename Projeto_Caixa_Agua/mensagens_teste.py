from datetime import datetime, timezone
import time as t
import requests

def enviar_mensagem(number, text, tipo = 0):
    url = "http://localhost:8080/message/sendText/torbi"
    payload = {
        "number": number,
        "text": text
    }
    if tipo == 1:
        url = "http://localhost:8080/message/sendMedia/torbi"
        payload = {
            "number": number,
            "mediatype": "image",
            "mimetype": "image/png",
            "caption": text,
            "media": "https://i.redd.it/osvoh8vuumdf1.jpeg",
            "fileName": "Codorna.png"
            #https://upload.wikimedia.org/wikipedia/commons/d/d8/Taoniscus.jpg
            #https://i.redd.it/osvoh8vuumdf1.jpeg
        }

    headers = {
        "Content-Type": "application/json",
        "apikey": "3DCD9D66910C-4131-87C7-F97E9D53D897",
        "User-Agent": "PostmanRuntime/7.44.1",
        "Accept": "*/*"
    }

    try:
        response = requests.post(url, headers=headers, json=payload)
        return response.status_code, response.text
    except requests.RequestException as e:
        return 500, str(e)

# 🔁 Loop forever
while True:
    now = datetime.now()
    minute = now.minute
    second = now.second

    print(f"Current time: {now.strftime('%H:%M:%S')}")

    if (str(minute).endswith("0")) and second == 00:
        print("Sending message...")
        status, response = enviar_mensagem("5547997691399", "*Mambo!*", 1)
        print(f"Status: {status}, Response: {response}")
    else:
        t.sleep(1)  # check again in 1 second

#5548999509969
#5548999651619
#5547997691399
#120363021389717365@g.us
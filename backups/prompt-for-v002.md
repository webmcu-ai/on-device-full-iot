Continue the webMCU-AI "v002" build. Check your memory notes on the WiFi transport / vision trainer project first.

PROJECT
A single-file teaching platform for the Seeed XIAO ESP32S3 Sense: collect images, train a small CNN in the browser or on the board, and run inference. Students get three single files: firmware-vNNN.ino, index-vNNN.html, server-vNNN.js. The base is the code from github.com/webmcu-ai/on-device-vision-and-web, which trains larger resolutions and more classes successfully. I am retiring in June 2027 and this is for classrooms.

ATTACHED (as uploads)
- firmware-v001.ino, index-v001.html, server-v001.js: the finished first batch, the new base
- firmware-lora-v009.ino and index-lora-v010.html: the older LoRa versions, to port from (the monitor, history, CSV and graph)
- the Nano 33 BLE Sense firmware and index.html: only as a pattern for Web Bluetooth (custom service UUID, chunked writes, notify characteristics)

WHAT v001 ALREADY DOES
- Page section 9 and firmware: WiFi (joins the router or makes its own "XIAO-Vision" network, mDNS xiao-vision.local) with a WebSocket server on port 81.
- One text protocol on every link. The page sends single characters D/d as a debug heartbeat, and "@model <bytes> <sum>" then weights. On USB the weights go as "@h<offset>,<hex>,<sum>" lines in 96-byte chunks; on WiFi as raw binary frames of 4096 bytes. The board replies A,<bytes so far> / Y,<message> / E,<error>. The board sends "@F ..." debug frames to USB and WiFi at once (myLkEmit).
- Firmware hooks: myLkHandleChar swallows any line starting with @ inside myHandleDebugChar. myLkPoll() is called in loop, the collect loop, the infer loop and the training exit check. MY_USE_WIFI 0 removes WiFi.
- server-v001.js serves the page over http and bridges ws://localhost:8080/dev/<board>[:port] to a board (private addresses and .local only).
- Not yet done: LoRa, Web Bluetooth, menu keys over WiFi. The firmware has not been compiled or tested on hardware.

v002 GOALS, IN THIS ORDER
1. Port LoRa in as an option (MY_USE_LORA) from the LoRa v009/v010 files: the radio, summaries, chat, @commands, plus the page's monitor, history, graph and CSV. Make the page section "Nearby devices" with a transport column (LoRa, WiFi, BLE) using the shared summary line S,<name>,<seq>,<period>,<frames>,<c0>,.... A WiFi-connected board should also forward the LoRa lines it hears. The LoRa firmware has its own @command system, so route @model and @h to the push code and the rest to the LoRa commands. The touch pad conflicts with LoRa DIO1 on D0 and the SPI bus is shared with the SD card.
2. Web Bluetooth transport (NimBLE-Arduino on the board) with the same text protocol and chunked writes, in the page and firmware.
3. Menu keys (t, l, digits) over WiFi and BLE.
4. Update the visuals and code comments to be student-friendly, and fix the old stray classDir(-1 + 0, true) line in the LoRa page's addClass().

HARD RULES
- Arduino IDE: every variable and function pre-declared near the top (forward declarations).
- Do not remove any existing ability: SD folder and zip, review, burst capture, any input size, parity test, debug frames.
- Do not bring over the LoRa firmware's older eager training-buffer allocation or its uninitialized camera_config_t. Keep v001's lazy allocation and zero-initialized config.
- Each deliverable is one complete file. Keep a MY_USE_* switch for each optional transport so students can leave things out.

HOW TO WORK
Look in /mnt/user-data/uploads first. Edit the real files with patch scripts (each text replacement must match exactly once), syntax-check the page's JavaScript with node, test server-v001.js with an echo board, then give me the complete files named *-v002. Keep replies short. Tell me plainly what you could not test.




possible helpful code

#include <WiFi.h>
#include <ESPAsyncWebServer.h>

// Replace with your network credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);
// Create a WebSocket object on the path /ws
AsyncWebSocket ws("/ws");

// Simple HTML web page served directly from the ESP32 flash memory
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 WebSocket Server</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; text-align: center; margin-top: 50px; }
        .btn { padding: 15px 30px; font-size: 24px; cursor: pointer; }
    </style>
</head>
<body>
    <h2>ESP32 WebSocket Demo</h2>
    <button class="btn" onclick="sendMessage()">Send Hello</button>
    <p>Server Response: <span id="response">None</span></p>

<script>
    // Automatically detect host IP and target the /ws route
    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;
    
    window.addEventListener('load', onload);

    function onload(event) {
        initWebSocket();
    }

    function initWebSocket() {
        console.log('Trying to open a WebSocket connection...');
        websocket = new WebSocket(gateway);
        websocket.onopen    = onOpen;
        websocket.onclose   = onClose;
        websocket.onmessage = onMessage;
    }

    function onOpen(event) {
        console.log('Connection opened');
    }

    function onClose(event) {
        console.log('Connection closed');
        setTimeout(initWebSocket, 2000); // Try to reconnect every 2 seconds
    }

    function onMessage(event) {
        console.log("Data received: " + event.data);
        document.getElementById('response').innerHTML = event.data;
    }

    function sendMessage() {
        websocket.send('Hello from Browser!');
    }
</script>
</body>
</html>
)rawliteral";

// Handle incoming WebSocket events
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, uint8_t *data, size_t len) {
    
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
            client->text("Connected to ESP32!");
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("WebSocket client #%u disconnected\n", client->id());
            break;
        case WS_EVT_DATA:
            // Handle incoming data packet
            AwsFrameInfo *info = (AwsFrameInfo*)arg;
            if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
                data[len] = 0; // Null-terminate string
                String message = (char*)data;
                Serial.printf("Received message: %s\n", message.c_str());
                
                // Echo back message or perform an action (like changing a GPIO state)
                client->text("ESP32 received your: " + message);
            }
            break;
        case WS_EVT_PONG:
        case WS_EVT_ERROR:
            break;
    }
}

void setup() {
    Serial.begin(115200);

    // Connect to Wi-Fi network
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");
    Serial.print("Connected! IP Address: ");
    Serial.println(WiFi.localIP());

    // Attach WebSocket handler to the server path
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // Serve the root index.html page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    // Start server
    server.begin();
}

void loop() {
    // Periodically clean up disconnected clients to free up heap memory
    ws.cleanupClients();
}

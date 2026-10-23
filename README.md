# on-device-full-iot
on-device machine learning with wifi, ble, lora and webSerial



# XIAO Vision: teach a camera board to recognise things

You will teach a small board (Seeed XIAO ESP32S3 Sense) to tell objects apart, for example "pen" and "cup".
You collect pictures, train a small neural network (CNN) in your browser, then put the result on the board.

You have three files:

| File | What it is |
|---|---|
| `firmware.ino` | The program for the board (Arduino IDE) |
| `index.html` | The web page where you collect pictures and train |
| `server.js` | A tiny helper program, only needed for the WiFi ways (Way 3 and 4) |

## One-time setup

1. Open `firmware-v002.ino` in the Arduino IDE.
2. Choose the board **XIAO_ESP32S3**, turn **PSRAM: OPSRAM** on, and upload.
3. Put a FAT32-formatted SD card in the board (needed for Way 1, and used by the others as a backup).
4. Open `index-v002.html` in Chrome or Edge (double-click it).

## The big picture

1. **Collect** pictures of each thing (the board takes them).
2. **Train** in the web page. This makes a file of "weights" (the learned numbers).
3. **Get the weights onto the board**, using one of the four ways below.
4. The board now recognises things by itself, with no computer.

The four ways only differ in step 3. Pick the easiest one that works for you.

---

## Way 1: SD card (easiest)

No cables or WiFi needed. Best for a classroom.

1. In the page, choose your SD card folder (section 1). The page reads your pictures from it.
2. Train (sections 2 to 5).
3. Press **Save** (section 6). The weights are written into the SD card folder.
4. Put the SD card back into the board and reset it. The board loads the weights at start-up.

## Way 2: USB cable with WebSerial (slow, but simple)

Good when you have a cable and no card reader.

1. Plug the board into the computer with USB.
2. In section 8, press **Connect** and pick the board's port.
3. Press **Send weights to device** (section 9).
4. Wait. This is slow (a few minutes) because the model is sent in small pieces. Do not unplug until the page says it is done.

Close the Arduino Serial Monitor first, because only one program can use the port.

## Way 3: WiFi through your router or phone hotspot

Faster than USB. The board and the computer must be on the **same WiFi**.

1. Type your WiFi name and password at the top of the firmware (`myWifiSsid`, `myWifiPass`) and upload.
2. Start the helper on your computer: open a terminal in the folder and run `node server-v001.js`.
3. In section 9 change the address to `ws://localhost:8080/dev/xiao-vision.local`
   (if that name does not work, use the board's number, like `ws://localhost:8080/dev/192.168.1.57`; the board prints it in the Serial Monitor at start-up).
4. Press **Connect WiFi**, then **Send weights to device**.

## Way 4: the board makes its own WiFi (hotspot)

Use this when there is no router. If the board cannot join your WiFi, it makes its own network automatically.

1. Power the board. Look for the WiFi network **XIAO-Vision** (password `xiaovision`) and join it with your computer.
   (Your computer has no internet while joined. That is fine.)
2. # Initialize a new Node project (press Enter to accept defaults)
```npm init -y```
3. # Install the required websocket library specified in your file comments
```npm install ws```
3. Start the helper: `node server.js`
4. In section 9 leave the address as it is: `ws://localhost:8080/dev/192.168.4.1`
5. Press **Connect WiFi**, then **Send weights to device**.

---

## What is the "bridge" (`server-v001.js`)?

Browsers are careful: a page may not open a plain connection to a device on your network when the page itself comes from a secure (https) place, and some browsers are strict about local files too.
The helper is a small go-between that runs on **your own computer**:

```
Web page  <-->  helper (your computer, port 8080)  <-->  board (WiFi, port 81)
```

The page always talks to `localhost` (your own computer, which is allowed). The helper passes the messages on to the board.
It only talks to boards on your own network (private addresses and `.local` names), so it cannot be used to reach the internet.

The address has this shape: `ws://localhost:8080/dev/<board address>`. Change only the last part.

## If something goes wrong

| Problem | Try this |
|---|---|
| Page says it cannot connect | Is the helper running? Is your computer on the board's WiFi? |
| `xiao-vision.local` is not found | Use the number address instead (192.168.4.1 for the board's own WiFi) |
| Board says the size is wrong | The page settings (image size, classes) must match the firmware |
| USB port will not open | Close the Serial Monitor and any other tab using it |
| Page is open from an `https://` link | Open it from the file, or from `http://localhost:8080` (the helper serves it) |

## Not in this version yet

LoRa (many boards in a room), Web Bluetooth, and menu keys over WiFi are planned for the next steps.

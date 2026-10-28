# BLE reliability: index-v010.html + firmware-v010.ino

Needs NimBLE-Arduino 2.1 or newer (API checked against the 2.3.6 source). Same UUIDs, same line protocol, same command names, same file formats, same SD layout. New: one command `@ble` and one reply prefix `@B`.

## How each finding was established

- **Confirmed by running code**: reproduced with the original v009 code, or with the v010 code in a test harness.
- **Confirmed by reading**: the fault is visible in the v009 source (and, for NimBLE behaviour, in the NimBLE 2.3.6 source), but I did not run it on a board.
- **Suspected**: plausible, not proven. Needs a board.

## Root causes

### Firmware (firmware-v009.ino)

| # | Function | Fault | Status |
|---|----------|-------|--------|
| F1 | `myBleSend` | Each piece was sent with `setValue()` then `notify()` with no connection handle. In NimBLE 2.x that form only marks the characteristic "changed" (`ble_gatts_chr_updated`), returns true at once, and the host task reads the stored value later. Pieces sent 2 ms apart can be merged or overwritten, and the newline (sent as its own 1-byte piece) can replace the line it follows. The `while (!notify() ...)` retry loop could never run. | Confirmed by reading NimBLE 2.3.6. How often it bites on hardware is untested. |
| F2 | `myBleSend` | A failure was never reported, the loop kept going after a failed piece (a cut line reached the page), nothing checked that the link was still up, and a stalled link could hold the main loop for 200 x 3 ms per piece. | Confirmed by reading. |
| F3 | `MyBleServerCb::onConnect`, `MyBleRxCb::onWrite`, `myBleService` | `onConnect` reset the ring head and tail from the BLE task while the main loop was consuming. `volatile uint16_t` indexes have no ordering against the array writes. A full ring dropped bytes silently. | Confirmed by reading. Race frequency unknown. |
| F4 | `myBleFeed` | Lines longer than 2100 characters were silently cut (`@cfg` can be 4005). The half-collected line was never cleared on a new connection, so a half line from a dropped link prefixed the next link's first command. | Confirmed by reading; harness reproduces the fixed behaviour. |
| F5 | callbacks | One `myBleConnected` bool, no connection handle: a second central would corrupt state. Advertising was only restarted from `onDisconnect`; NimBLE 2.x does not restart it itself (`advertiseOnDisconnect` defaults to false). A connected page that never talks keeps the board from advertising for ever. | Confirmed by reading. |
| F6 | `myLkEmit` | Serial was written before Bluetooth. A USB port with nobody reading can make `Serial.write` slow, delaying the BLE reply. | Suspected. v010 sends Bluetooth first and measures the worst Serial write (`sermaxms` in `@ble`). |

### Page (index-v009.html)

| # | Function | Fault | Status |
|---|----------|-------|--------|
| P1 | `serFeed` | After every notification, any partial line that did not start with `@` and did not look like a short `A,` / `E,` / `Y,` reply was flushed to the monitor and the buffer emptied. Because v009 firmware sends the newline as a separate notification, every `Y,` reply with more than 30 characters after the comma was lost: `Y,info\|...` (the `@info` answer) and the final `Y,...` confirmation of a weights or image transfer never reached `lkSettle`. A 300-character reply split by the MTU was lost entirely. | **Confirmed by running the original function** in Node with the v009 firmware's send pattern: only `A,450` and a short `E,` reached the transfer code; both long `Y,` lines went to the monitor. |
| P2 | `serFeed`, `myBle.dec`, `myBleGone` | USB and Bluetooth shared one `serPend` buffer. `myBleGone` never cleared it or the streaming `TextDecoder`, so a leftover `@...` fragment or half UTF-8 character from the old link prefixed the first reply of the next one. | Confirmed by reading. |
| P3 | `myBleConnect` | When setup failed after `requestDevice` (the third retry, `getCharacteristic`, `startNotifications`), the `catch` called `myBleGone()` without `gatt.disconnect()`. The browser can keep the GATT link open, the board stays connected, stops advertising, and is missing from the next chooser. | Mechanism confirmed by reading. That it happens on your boards is unproven. |
| P4 | `myBleWrite` | Queued writes used `myBle.rx` when they ran, so a write queued on an old link could run on the new one. No timeout. Errors from heartbeats were thrown away (`.catch(() => {})`). No guard against a second connect while one was in progress. | Confirmed by reading. |
| P5 | `serFeed`, `lkLine` | The board answers on every open link (`myLkEmit`), and the page accepted replies from any link, so with BLE plus USB or WiFi open a reply could settle the wrong waiter twice. | Confirmed by reading. |
| P6 | `lkWait`, `myReadRemote`, `myDataLine` | An old timeout timer cleared a newer waiter; a late or repeated `@D` piece failed the current piece. The protocol recovered by retrying, at the cost of time. | Confirmed by reading. |

## The two-device observation

Both boards advertise `XIAO-Vision` with identical service and characteristic UUIDs, and the page filters by name prefix only, so the Chrome chooser shows two indistinguishable entries. That is confirmed. **Nothing here proves that a second board improves reliability, and v010 does not rely on it.** Hypotheses, none tested:

1. A board stuck in a half-open connection (P3, F5) does not advertise, so only the other board appears and works.
2. The user picks a different board (nearer, or the one that was not stuck) when two entries are shown.
3. Radio placement, or coincidence.

What v010 does: the board advertises `XIAO-Vision-XXXX` (last two bytes of its MAC; `MY_BLE_UNIQUE_NAME 0` restores the old name, `myBleIdOverride` sets your own id such as `A1`). The page still matches the prefix `XIAO-Vision`, so old boards still show. An optional name box narrows the chooser (`XIAO-Vision-A3F2`). Multi-device connections were not added.

## What changed

**Firmware**
- `notify(data, length, connHandle)`: one piece, one notification, a real success flag. Piece size = MTU − 3, at most 240, read from the live connection. The newline rides in the last piece (fewer packets).
- A failed piece is retried for at most `MY_BLE_TX_WAIT_MS` (400 ms), then the line is abandoned. After `MY_BLE_STALL_LINES` (3) abandoned lines in a row the link counts as stalled and lines are dropped instantly until the page writes again. The next line after an abandoned one starts with a newline so the page drops the cut line. Nothing is sent before the page switches notifications on (`onSubscribe`).
- SPSC receive ring with `std::atomic` indexes; only the main loop moves the tail or resets state. Overflow writes a cancel byte (0x18) into the ring exactly at the damage and drops the rest of that line, so a cut or merged line never runs. Over-long lines (> 4100) and half lines older than 5 s are discarded and counted.
- Connect, disconnect, MTU and subscribe events are noted by the BLE task and printed by the main loop (the BLE task never prints). A second connection is refused. A connected page silent for 30 s (`MY_BLE_IDLE_MS`) is disconnected. Advertising is checked every 3 s and restarted if off.
- `@ble` answers `@B ble|name=..|mtu=..|pay=..|rxhi=..|rxovf=..|txfail=..|...`. The `@B` prefix cannot be mistaken for an `A,` / `E,` / `Y,` transfer reply. Debug frames stay off by default over BLE.

**Page**
- Bluetooth bytes are decoded with a streaming decoder and cut into lines only at `\n`. USB keeps its prompt flushing; Bluetooth never flushes.
- A session number invalidates callbacks, queued writes and receive buffers of an old link. Every failure and disconnect goes through `myBleDrop`, which also disconnects GATT.
- Writes: one at a time, piece size = the board's reported payload (20 bytes when the board does not answer `@ble`), 6 s timeout then the link is dropped. After a failed write the next write starts with a newline.
- Only the active link (WiFi, then USB, then Bluetooth) feeds the transfer code. A disconnect rejects the waiting transfer at once instead of after its timeout.
- Fixed stale timers; `@D` pieces for a different offset are ignored instead of failing the piece; the `@model` start is retried (it is idempotent).
- Diagnostics box (redrawn once a second, `@ble` polled every 10 s, never while a transfer is waiting): device name, MTU, payload, round trip (last, worst, average), writes ok/failed, notifications, partial lines, board ring high-water, overflows, notify failures, link drops and the last disconnect reason.

**Left as it was on purpose**: transfer chunk sizes (BLE: 450 bytes upload, 600 bytes download, 96 bytes weights), checksums, offsets, acknowledgements, retry counts, USB and WiFi paths.

## Known limits

- `@cfg` has no checksum in the protocol. A line cut by a failed write could be saved as a bad `config.json`; v010 makes that much less likely but did not change the protocol.
- An overflowed line is lost and the page recovers by its normal timeout and resend.
- `@B` lines are broadcast on all links like every reply, so they appear in the Arduino serial monitor every 10 s while the page is connected over BLE (the page ignores them on USB and WiFi).
- A BLE-only session with a modest connection interval is slow for models and many images, as before.

## What was and was not tested here

**Run here, all passing**
- Page Bluetooth code (Node, fake characteristic): several lines in one notification; a line split at every byte position; UTF-8 split inside a character; CRLF and empty lines; session drop clears the buffer; late bytes after a drop ignored; writes never overlap and use payload-sized pieces in order; a failed write marks the cut line; a drop cancels a write in progress; `@B` sets the payload.
- Firmware BLE logic (native build of the real ring, framing, send and callback code against stubs whose signatures match NimBLE 2.3.6): 400 and 3000 lines intact and in order with random write sizes and a concurrent writer thread; flood of 18 KB into the 4 KB ring and an unthrottled writer against a slow reader (no damaged line ever executed); half line of an old link dropped on reconnect; stale handle ignored; second connection refused; 4005-character `@cfg` accepted whole, over-long line dropped whole; piece sizes at MTU 247 and MTU 23; failed notifications give up after about 400 ms, stall after three, recover, resync with a newline; nothing sent before subscribe; a send stops when the link disconnects. ThreadSanitizer reported nothing.

**Not run here (needs hardware or the Arduino toolchain)**
- `firmware-v010.ino` was not compiled with the Arduino-ESP32 toolchain. Only the BLE block was syntax-checked against stubs. Compile it once and tell me the first error if there is one.
- Every real-radio behaviour: Chrome/OS MTU negotiation, notification timing, connection interval, mbuf exhaustion, and whether F1 and P1 account for the failures you see.

## Test checklist (hardware)

Open the Serial monitor at 115200 (the `[ble]` lines) and keep the page's diagnostics box visible. Capture the box and the `[ble]` lines on any failure.

**A. One board**
1. Flash `firmware-v010.ino`, open `index-v010.html` in Chrome. Serial shows `Bluetooth ready ... "XIAO-Vision-XXXX"`.
2. Connect Bluetooth. Expect `[ble] connect`, `mtu=` (up to 247), `notifications on`. The box shows MTU, payload (up to 240) and a round trip.
3. `@info` answers (the Y reply that v009 lost). Press Ask the board several times.
4. Leave connected 2 minutes: round trip stays sane, `txfail`, `rxovf`, `bad` stay 0, no half lines dropped.

**B. Two boards with identical names**
1. Flash one board with `MY_BLE_UNIQUE_NAME 0`, the other with the default. Expected chooser: `XIAO-Vision` and `XIAO-Vision-XXXX`. Then flash both with the default: two different names.
2. Flash both with `MY_BLE_UNIQUE_NAME 0` to reproduce your original setup. Connect to each in turn, 10 times each; note which entry you picked, whether it connected, and whether `@info` worked. This is the test that shows whether duplicate names matter.
3. With two boards on, do the same with only one powered. Compare success rates. Only this comparison supports or refutes the "better with two" observation.
4. Type a full name in the name box and check only that board is offered.

**C. Repeated connect / disconnect**
1. 20 cycles: Connect, wait for `@info`, Disconnect. Every cycle must reconnect first time, and Serial must show a matching `[ble] disconnect` and advertising again.
2. 10 cycles closing the tab or reloading while connected: the board must be findable again within about 3 s (or, for a silent half-open link, 30 s).
3. Press Connect twice quickly: the second press is ignored ("still connecting").
4. Power-cycle the board while connected: the page shows "Bluetooth link closed" and Connect works again.
5. Move out of range and back.

**D. Short commands**
1. 100 x `@info`, `@ls /images`, `@stat <file>`: no timeouts, no half lines, round trip steady.
2. Heartbeat: with debug frames off, `txok` grows only with replies; with the box ticked, expect slower replies (leave it off by default).

**E. Large transfers (one change at a time)**
1. Download `config.json`, then `weights`, then 20 images over BLE. Every piece must pass its check; watch `stale pieces ignored`, `txlinefail`, `rxovf`.
2. Upload 20 images, then a weights push, then `config.json`. Confirm file sizes on the SD card and that the final `Y,` message arrives (the "no confirmation from the device" case in v009).
3. Disconnect during a transfer: the page should stop at once with "Bluetooth disconnected".
4. If a transfer fails, compare: SD errors show in the board's `E,` text; `txlinefail` / `rxovf` / `bad` point at Bluetooth; a checksum mismatch with all counters 0 points at the application.

**F. Switching links**
1. Connect BLE, then USB: USB becomes the active link (commands go over USB), BLE keeps only its diagnostics, replies are not counted twice.
2. WiFi then BLE, then disconnect the first one: the next command uses the remaining link.
3. USB transfer, disconnect USB, connect BLE, repeat the transfer.
4. Old behaviour check: the USB monitor still shows prompts and menu text.

**G. Stress, optional**
1. A 12 KB `@J` snapshot over BLE with a phone or laptop moved to the edge of range: expect abandoned lines and a stall message, never a frozen camera loop. A failed line costs at most about 0.4 s and three in a row stall the link, so `txmaxms` should only be large for a long line that really was sent slowly.
2. Leave the page connected overnight with debug frames off: no growth in `bad`, no unexplained drops.

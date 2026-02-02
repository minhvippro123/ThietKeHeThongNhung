# ESP32 + Pico (LoRa) — Firebase HMI Pack

Mục tiêu:
1) **Web UI** (HMI) ghi lệnh vào Firebase `/control`
2) **ESP32** đọc `/control` → gửi LoRa xuống Pico (CMD:...)
3) **ESP32** ghi `/state` + **push `/history`** để web vẽ chart (Lux / Brightness)

---

## 0) Cấu trúc Firebase đề xuất

```
/nodes
  /NODE_01
    /control
      mode:   "AUTO" | "MANUAL"
      power:  true/false
      bri:    0..255
      r,g,b:  0..255
      preset: "NONE" | "STATIC" | "NIGHT" | "MUSIC"
      ts:     1730000000000  (ms)
    /state
      online: true
      mode: ...
      preset: ...
      power: ...
      bri: ...
      r,g,b: ...
      lux: ...
      wifi_rssi: ...
      lora_rssi: ...
      ts: ...
    /history
      -pushId1-
        ts:  ...
        lux: ...
        bri: ...
      -pushId2-
        ...
```

Web sẽ **listen realtime** `/state` và lấy **last 120** mẫu từ `/history` để vẽ chart.

---

## 1) Chạy Web HMI (VSCode)

1. Giải nén thư mục.
2. Mở bằng VSCode.
3. Cài **Live Server** (extension).
4. Right click `index.html` → **Open with Live Server**.

> Nếu bạn muốn đổi `NODE_ID`, ở UI góc phải sẽ có ô **NODE ID** (đã “tận dụng” ô ESP32 IP cũ).

---

## 2) Firebase Rules (DEV nhanh)

Vào **Realtime Database → Rules**, tạm để dev nhanh:

```json
{
  "rules": {
    ".read": true,
    ".write": true
  }
}
```

Khi demo xong thì siết lại theo auth (để an toàn).

---

## 3) File trong pack

- `index.html`, `style.css`, `app.js` : Web HMI + Chart
- `firebase_config.js` : Firebase config (đúng project bh1750-d79df)
- `esp32_firebase_control.ino` : ESP32 đọc `/control`, ghi `/state` + push `/history`

---

## 4) ESP32 đọc /control (ý chính)

Trong `esp32_firebase_control.ino`:

- Bắt stream: `/nodes/NODE_01/control`
- Khi có thay đổi: đọc full object `/control`
- Nếu khác cache → **throttle** → gửi lệnh xuống Pico:
  - `CMD:POWER:ON|OFF`
  - `CMD:MODE:AUTO|MANUAL`
  - `CMD:BRI:120`
  - `CMD:RGB:255,0,0`
  - `CMD:PRESET:NIGHT` (nếu preset != NONE)

👉 Bạn chỉ cần map hàm `sendToPico(cmd)` sang LoRa send thật của bạn.

---

## 5) Lưu lịch sử (History) hợp lý lưu gì?

**Tối thiểu để vẽ chart:**
- `ts` (ms), `lux`, `bri`

**Nên thêm nếu muốn debug “kẹt/lệnh chậm”:**
- `wifi_rssi`, `lora_rssi`, `mode`, `power`, `preset`

Trong pack, mình để history **lux + bri** cho nhẹ.

---

## 6) “3 history” được không?

Có 2 kiểu:
- **3 dòng chart**: Lux / Brightness / RSSI → chỉ cần push thêm field vào mỗi sample.
- **giữ đúng 3 mẫu gần nhất** trong DB: làm được nhưng RTDB không tự auto-trim; thường dùng:
  - Cloud Function để dọn,
  - hoặc ESP32 định kỳ đọc `limitToFirst()` rồi xóa mẫu cũ.

Hiện web đã vẽ **last 120** mẫu (dễ dùng, không phình quá nhanh).

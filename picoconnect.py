from machine import SPI, Pin, I2C, PWM
import time
import math

# =========================
# LoRa (SX127x) SPI0
# Pico W SPI0 default pins can be remapped:
#   SCK=GP2, MOSI=GP3, MISO=GP4
# CS=GP5, RST=GP6
# =========================
spi = SPI(0, baudrate=1_000_000, polarity=0, phase=0,
          sck=Pin(2), mosi=Pin(3), miso=Pin(4))
cs  = Pin(5, Pin.OUT, value=1)
rst = Pin(6, Pin.OUT, value=1)

REG_FIFO               = 0x00
REG_OP_MODE            = 0x01
REG_FRF_MSB            = 0x06
REG_FRF_MID            = 0x07
REG_FRF_LSB            = 0x08
REG_PA_CONFIG          = 0x09
REG_FIFO_ADDR_PTR      = 0x0D
REG_FIFO_TX_BASE_ADDR  = 0x0E
REG_FIFO_RX_BASE_ADDR  = 0x0F
REG_FIFO_RX_CURRENT    = 0x10
REG_IRQ_FLAGS          = 0x12
REG_RX_NB_BYTES        = 0x13
REG_PKT_RSSI_VALUE     = 0x1A
REG_MODEM_CONFIG_1     = 0x1D
REG_MODEM_CONFIG_2     = 0x1E
REG_MODEM_CONFIG_3     = 0x26
REG_PREAMBLE_MSB       = 0x20
REG_PREAMBLE_LSB       = 0x21
REG_PAYLOAD_LENGTH     = 0x22
REG_SYNC_WORD          = 0x39
REG_VERSION            = 0x42

IRQ_RX_DONE = 0x40
IRQ_TX_DONE = 0x08
IRQ_CRC_ERR = 0x20

MODE_LONG_RANGE    = 0x80
MODE_SLEEP         = 0x00
MODE_STDBY         = 0x01
MODE_TX            = 0x03
MODE_RX_CONT       = 0x05

def wreg(a, v):
    cs.value(0); spi.write(bytearray([a | 0x80, v & 0xFF])); cs.value(1)

def rreg(a):
    cs.value(0); spi.write(bytearray([a & 0x7F])); v = spi.read(1)[0]; cs.value(1); return v

def burst_write_fifo(data: bytes):
    cs.value(0); spi.write(bytearray([REG_FIFO | 0x80])); spi.write(data); cs.value(1)

def burst_read_fifo(n: int) -> bytes:
    cs.value(0); spi.write(bytearray([REG_FIFO & 0x7F])); d = spi.read(n); cs.value(1); return d

def reset_radio():
    rst.value(0); time.sleep(0.05)
    rst.value(1); time.sleep(0.05)

def clear_irq():
    wreg(REG_IRQ_FLAGS, 0xFF)

def set_freq(freq_hz: int):
    frf = int((freq_hz << 19) / 32_000_000)
    wreg(REG_FRF_MSB, (frf >> 16) & 0xFF)
    wreg(REG_FRF_MID, (frf >> 8) & 0xFF)
    wreg(REG_FRF_LSB, frf & 0xFF)

def rssi_dbm():
    return -157 + rreg(REG_PKT_RSSI_VALUE)

def enter_rx():
    clear_irq()
    wreg(REG_OP_MODE, MODE_LONG_RANGE | MODE_RX_CONT)

def radio_init():
    wreg(REG_OP_MODE, MODE_LONG_RANGE | MODE_SLEEP); time.sleep_ms(5)
    wreg(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY); time.sleep_ms(5)

    set_freq(433_000_000)
    wreg(REG_MODEM_CONFIG_1, 0x70 | 0x02)         # BW125 + CR4/5
    wreg(REG_MODEM_CONFIG_2, (7 << 4) | 0x04)     # SF7 + CRC ON
    wreg(REG_MODEM_CONFIG_3, 0x04)                # AGC
    wreg(REG_PREAMBLE_MSB, 0x00)
    wreg(REG_PREAMBLE_LSB, 0x08)
    wreg(REG_SYNC_WORD, 0x12)
    wreg(REG_PA_CONFIG, 0x80 | (14 - 2))          # 14 dBm (ổn định)

    wreg(REG_FIFO_RX_BASE_ADDR, 0x00)
    wreg(REG_FIFO_ADDR_PTR, 0x00)
    enter_rx()

def send_packet(text: str, timeout_ms=2000) -> bool:
    b = text.encode()

    wreg(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY)
    time.sleep_ms(2)
    clear_irq()

    wreg(REG_FIFO_TX_BASE_ADDR, 0x00)
    wreg(REG_FIFO_ADDR_PTR, 0x00)
    burst_write_fifo(b)
    wreg(REG_PAYLOAD_LENGTH, len(b))

    wreg(REG_OP_MODE, MODE_LONG_RANGE | MODE_TX)
    time.sleep_ms(2)

    t0 = time.ticks_ms()
    while True:
        irq = rreg(REG_IRQ_FLAGS)
        if irq & IRQ_TX_DONE:
            clear_irq()
            enter_rx()
            return True
        if time.ticks_diff(time.ticks_ms(), t0) > timeout_ms:
            clear_irq()
            enter_rx()
            return False

def recv_packet():
    irq = rreg(REG_IRQ_FLAGS)
    if not (irq & IRQ_RX_DONE):
        return None

    n = rreg(REG_RX_NB_BYTES)
    addr = rreg(REG_FIFO_RX_CURRENT)
    crc_err = bool(irq & IRQ_CRC_ERR)
    clear_irq()

    if crc_err or n == 0 or n > 255:
        return None

    wreg(REG_FIFO_ADDR_PTR, addr)
    return burst_read_fifo(n)

# =========================
# BH1750 (I2C0 GP0/GP1)
# SDA=GP0, SCL=GP1
# =========================
i2c = I2C(0, scl=Pin(1), sda=Pin(0), freq=100000)
BH1750_ADDR = 0x23

def bh1750_init():
    try:
        i2c.writeto(BH1750_ADDR, b'\x01')  # power on
        time.sleep_ms(10)
        i2c.writeto(BH1750_ADDR, b'\x10')  # cont high-res
        print("BH1750 OK")
    except Exception as e:
        print("BH1750 init FAIL:", e)

def bh1750_read_lux():
    try:
        d = i2c.readfrom(BH1750_ADDR, 2)
        raw = (d[0] << 8) | d[1]
        return raw / 1.2
    except:
        return None

# =========================
# KY-016 RGB PWM (GP16/17/18)
# =========================
PWM_FREQ = 1000
COMMON_ANODE = False   # nếu màu/sáng bị ngược -> True
GAMMA = 2.2

pwm_r = PWM(Pin(16)); pwm_r.freq(PWM_FREQ)
pwm_g = PWM(Pin(17)); pwm_g.freq(PWM_FREQ)
pwm_b = PWM(Pin(18)); pwm_b.freq(PWM_FREQ)

def _clamp8(x):
    if x < 0: return 0
    if x > 255: return 255
    return x

def _gamma_u16(v8):
    v8 = _clamp8(v8)
    x = v8 / 255.0
    y = pow(x, GAMMA)
    return int(y * 65535 + 0.5)

def set_rgb(r8, g8, b8, bri8, power_on):
    if (not power_on) or bri8 <= 0:
        pwm_r.duty_u16(0); pwm_g.duty_u16(0); pwm_b.duty_u16(0)
        return

    bri8 = _clamp8(bri8)
    r8 = _clamp8(r8); g8 = _clamp8(g8); b8 = _clamp8(b8)

    # scale by brightness
    r = (r8 * bri8) // 255
    g = (g8 * bri8) // 255
    b = (b8 * bri8) // 255

    if COMMON_ANODE:
        r = 255 - r; g = 255 - g; b = 255 - b

    pwm_r.duty_u16(_gamma_u16(r))
    pwm_g.duty_u16(_gamma_u16(g))
    pwm_b.duty_u16(_gamma_u16(b))

# =========================
# App State
# =========================
MODE = "MANUAL"       # AUTO / MANUAL
POWER = True
BRI = 120             # 0..255
R, G, B = 255, 0, 0
PRESET = "STATIC"     # STATIC / NIGHT / MUSIC

# AUTO thresholds (hysteresis)
LUX_ON  = 30.0        # < 30 -> bật trắng
LUX_OFF = 60.0        # > 60 -> tắt
auto_white_on = False
last_lux = None

# MUSIC effect
music_h = 0.0
music_speed = 0.010   # tăng -> đổi màu nhanh hơn

# ACK dedupe
_last_ack_payload = ""
_last_ack_ms = 0
ACK_DEDUP_MS = 250    # cùng 1 lệnh trong 250ms thì không ACK lại

def hsv_to_rgb(h, s, v):
    i = int(h * 6)
    f = h * 6 - i
    p = v * (1 - s)
    q = v * (1 - f * s)
    t = v * (1 - (1 - f) * s)
    i = i % 6
    if i == 0: r, g, b = v, t, p
    elif i == 1: r, g, b = q, v, p
    elif i == 2: r, g, b = p, v, t
    elif i == 3: r, g, b = p, q, v
    elif i == 4: r, g, b = t, p, v
    else: r, g, b = v, p, q
    return int(r * 255), int(g * 255), int(b * 255)

def apply_preset():
    global R, G, B, BRI
    if PRESET == "NIGHT":
        R, G, B = 255, 120, 20
        BRI = 50
    elif PRESET == "STATIC":
        pass
    # MUSIC: effect trong loop

def status_string():
    lx = "NA" if last_lux is None else ("%.1f" % last_lux)
    return "MODE=%s POWER=%s BRI=%d RGB=%d,%d,%d PRESET=%s LUX=%s" % (
        MODE, "ON" if POWER else "OFF", BRI, R, G, B, PRESET, lx
    )

def _send_ack_for_cmd(cmd_line):
    # cmd_line: "CMD:...."
    global _last_ack_payload, _last_ack_ms
    payload = cmd_line[4:]  # bỏ "CMD:"
    now = time.ticks_ms()
    if payload == _last_ack_payload and time.ticks_diff(now, _last_ack_ms) < ACK_DEDUP_MS:
        return
    _last_ack_payload = payload
    _last_ack_ms = now
    send_packet("ACK:" + payload)

def handle_cmd(s):
    global MODE, POWER, BRI, R, G, B, PRESET

    s = s.strip()
    if not s.startswith("CMD:"):
        return False

    # Special: status query
    if s == "CMD:STATUS?":
        send_packet("STAT:" + status_string())
        _send_ack_for_cmd(s)
        return True

    parts = s.split(":")
    if len(parts) < 3:
        _send_ack_for_cmd(s)
        return True

    key = parts[1]
    val = ":".join(parts[2:]).strip()

    if key == "MODE":
        v = val.upper()
        if v in ("AUTO", "MANUAL"):
            MODE = v
        _send_ack_for_cmd(s)
        return True

    if key == "POWER":
        v = val.upper()
        if v in ("ON", "OFF"):
            POWER = (v == "ON")
        _send_ack_for_cmd(s)
        return True

    if key == "BRI":
        try:
            BRI = int(val)
            if BRI < 0: BRI = 0
            if BRI > 255: BRI = 255
        except:
            pass
        _send_ack_for_cmd(s)
        return True

    if key == "RGB":
        try:
            rr, gg, bb = val.split(",")
            R = _clamp8(int(rr)); G = _clamp8(int(gg)); B = _clamp8(int(bb))
        except:
            pass
        _send_ack_for_cmd(s)
        return True

    if key == "PRESET":
        v = val.upper()
        if v in ("STATIC", "NIGHT", "MUSIC"):
            PRESET = v
            if PRESET != "MUSIC":
                apply_preset()
        _send_ack_for_cmd(s)
        return True

    # Unknown CMD vẫn ACK để ESP biết đã nhận
    _send_ack_for_cmd(s)
    return True

# =========================
# Boot
# =========================
reset_radio()
ver = rreg(REG_VERSION)
print("SX127x version =", hex(ver))
radio_init()
bh1750_init()
print("Pico FINAL READY.")
print("Expect CMD from ESP: MODE/POWER/BRI/RGB/PRESET/STATUS?")

t_last_lux = time.ticks_ms()
t_last_stat = time.ticks_ms()

while True:
    # -------- RX LoRa --------
    d = recv_packet()
    if d:
        s = d.decode("utf-8", "ignore").strip()
        if s:
            print("RX", s, "RSSI", rssi_dbm())
            handle_cmd(s)

    # -------- AUTO mode --------
    if MODE == "AUTO":
        if time.ticks_diff(time.ticks_ms(), t_last_lux) > 500:
            t_last_lux = time.ticks_ms()
            lux = bh1750_read_lux()
            if lux is not None:
                last_lux = lux
                # hysteresis
                if (not auto_white_on) and lux < LUX_ON:
                    auto_white_on = True
                elif auto_white_on and lux > LUX_OFF:
                    auto_white_on = False

        if auto_white_on:
            set_rgb(255, 255, 255, 140, True)
        else:
            set_rgb(0, 0, 0, 0, False)

    # -------- MANUAL mode --------
    else:
        if PRESET == "MUSIC":
            music_h += music_speed
            if music_h >= 1.0:
                music_h -= 1.0
            rr, gg, bb = hsv_to_rgb(music_h, 1.0, 1.0)
            set_rgb(rr, gg, bb, BRI, POWER)
        else:
            set_rgb(R, G, B, BRI, POWER)

    # (optional) print lux every 2s for debug
    if time.ticks_diff(time.ticks_ms(), t_last_stat) > 2000:
        t_last_stat = time.ticks_ms()
        lx = bh1750_read_lux()
        if lx is not None:
            last_lux = lx
        # comment dòng dưới nếu spam log
        # print("STAT", status_string())

    time.sleep(0.02)

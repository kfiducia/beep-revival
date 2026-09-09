#!/usr/bin/env python3
"""Render the Beep 24-LED ring UI states to labeled GIFs for docs.

Every animation below is a faithful port of the on-device algorithm:
  - beepd.c: led_render_{connecting,ap,breathe,sleep,muted,volume,party,button_pulse,arming}
  - led-stage / led-boot-anim: smiley, wings (boot), fill (OTA), failsafe
Geometry matches the device: 24 LEDs, index 0 = 12 o'clock, increasing clockwise;
6 o'clock straddles LEDs 11 & 12 (the idle breathe/sleep pair).
Frame rate is the device's ~24 Hz (POLL_MS=42).

Usage:  pip install Pillow;  python3 scripts/led-ui-gifs.py
Writes docs/ui/*.gif (override the target with OUTDIR=/some/dir).
"""
import math, os, random
from PIL import Image, ImageDraw, ImageChops, ImageFont

NLED   = 24
LED_BOT = 12
EYE_L, EYE_R = 20, 3
SMILE_LO, SMILE_HI = 8, 15
POLL_MS = 42                      # device loop period → GIF frame duration

# --- canvas / look ----------------------------------------------------------
SIZE   = 240                      # ring canvas (square)
CAPH   = 34                       # caption strip height
CX = CY = SIZE // 2
RING_R = 92                       # LED ring radius
GLOWR  = 22                       # glow sprite radius
WARM   = (255, 236, 202)          # warm-white LED tint
OUTDIR = os.environ.get("OUTDIR") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "ui")
os.makedirs(OUTDIR, exist_ok=True)

def _font(sz):
    for p in ("/System/Library/Fonts/SFNSDisplay.ttf",
              "/System/Library/Fonts/Helvetica.ttc",
              "/Library/Fonts/Arial.ttf",
              "/System/Library/Fonts/Supplemental/Arial.ttf"):
        try: return ImageFont.truetype(p, sz)
        except Exception: pass
    return ImageFont.load_default()
FONT = _font(16)

# Pre-render a soft radial glow (grayscale), brightest at center.
_glow = Image.new("L", (2*GLOWR, 2*GLOWR), 0)
for yy in range(2*GLOWR):
    for xx in range(2*GLOWR):
        d = math.hypot(xx-GLOWR, yy-GLOWR) / GLOWR
        v = max(0.0, 1.0 - d)
        v = v*v*(3-2*v)                     # smoothstep falloff
        _glow.putpixel((xx, yy), int(255*v))

def _pos(i):
    # +half-a-step so NO LED sits on a cardinal, matching the real ring: 12 o'clock
    # straddles LEDs 23/0 (wire 0 lands ~12:15) and 6 o'clock straddles 11/12. Without
    # this offset, patterns symmetric about the 11/12 axis (the wings) render tilted.
    a = math.radians(i * (360.0/NLED) + (360.0/NLED)/2)   # 0 just CW of top, clockwise
    return CX + RING_R*math.sin(a), CY - RING_R*math.cos(a)

def render(bright, label):
    """bright: list[24] of 0..255 → one RGB frame (ring + caption)."""
    img = Image.new("RGB", (SIZE, SIZE), (10, 11, 13))
    # faint ring guide so unlit LEDs still read as a dial
    d = ImageDraw.Draw(img)
    for i in range(NLED):
        x, y = _pos(i)
        d.ellipse([x-2, y-2, x+2, y+2], fill=(26, 28, 32))
    for i in range(NLED):
        b = max(0, min(255, int(bright[i])))
        if b <= 0: continue
        x, y = _pos(i)
        s = _glow.point(lambda p: p*b//255)
        colored = Image.merge("RGB", (s.point(lambda p: p*WARM[0]//255),
                                      s.point(lambda p: p*WARM[1]//255),
                                      s.point(lambda p: p*WARM[2]//255)))
        ox, oy = int(x)-GLOWR, int(y)-GLOWR
        box = (ox, oy, ox+2*GLOWR, oy+2*GLOWR)
        region = img.crop(box)
        img.paste(ImageChops.add(region, colored), box)
    # caption strip
    out = Image.new("RGB", (SIZE, SIZE+CAPH), (10, 11, 13))
    out.paste(img, (0, 0))
    dd = ImageDraw.Draw(out)
    w = dd.textlength(label, font=FONT)
    dd.text(((SIZE-w)/2, SIZE+CAPH/2-9), label, font=FONT, fill=(196, 200, 208))
    return out

def save(frames, name, durations=None, loop=0):
    dur = durations if durations is not None else [POLL_MS]*len(frames)
    path = os.path.join(OUTDIR, name)
    # quantize to a shared 64-colour palette (the warm-white glow is ~1D) — keeps the
    # files small without visible banding, and a fixed palette avoids inter-frame flicker.
    pal = frames[0].quantize(colors=64, method=Image.FASTOCTREE, dither=0)
    q = [f.quantize(colors=64, palette=pal, dither=0) for f in frames]
    q[0].save(path, save_all=True, append_images=q[1:],
              duration=dur, loop=loop, disposal=2, optimize=True)
    print(f"  {name:28s} {len(frames):3d} frames  {os.path.getsize(path)//1024} KB")

def tri(ph, period):                         # integer triangle wave 0..period/2..0
    return ph if ph < period//2 else period - ph

# ---------------------------------------------------------------------------
# state renderers → list of brightness arrays (one per frame)
# ---------------------------------------------------------------------------
def f_smiley():
    b = [0]*NLED
    b[EYE_L] = b[EYE_R] = 255
    for i in range(SMILE_LO, SMILE_HI+1): b[i] = 255
    return b

def wings(k, v=180):
    b = [0]*NLED
    for i in range(NLED):
        d = (i-LED_BOT) if i >= LED_BOT else (LED_BOT-1-i)
        if d <= k: b[i] = v
    return b

def fill(n, v=180):
    return [v if i < n else 14 for i in range(NLED)]

def connecting(f):
    b = [10]*NLED
    b[f % NLED] = 255
    b[(f-1) % NLED] = 70
    return b

def comet(f):
    tail = [255, 130, 55, 22, 8]
    b = [0]*NLED
    head = f % NLED
    for d, t in enumerate(tail): b[(head-d) % NLED] = t
    return b

def breathe(f):
    lvl = 4 + tri(f % 84, 84)
    b = [0]*NLED; b[LED_BOT] = lvl; b[(LED_BOT-1) % NLED] = lvl
    return b

def sleep(f):
    lvl = 3 + tri(f % 120, 120)//2
    b = [0]*NLED; b[LED_BOT] = lvl; b[(LED_BOT-1) % NLED] = lvl
    return b

def muted(f):
    lvl = 8 + tri(f % 84, 84)
    return [lvl]*NLED

def volume(vol):
    lit = (vol*NLED + 50)//100
    b = [10]*NLED
    for j in range(lit): b[(LED_BOT+j) % NLED] = 255
    return b

def button_pulse(elapsed):                   # elapsed ms into a 350ms pulse
    half = 350//2
    t = elapsed*255//half if elapsed < half else (350-elapsed)*255//half
    t = max(0, min(255, t))
    return [t]*NLED

def arming(held):                            # held ms; 10s→wifi, 30s→reset
    HOLD_WIFI, HOLD_RESET = 10000, 30000
    if held < HOLD_WIFI:
        lit = held*NLED//HOLD_WIFI
        return [255 if i < lit else 6 for i in range(NLED)]
    elif held < HOLD_RESET:
        lit = (held-HOLD_WIFI)*NLED//(HOLD_RESET-HOLD_WIFI)
        return [255 if i < lit else 90 for i in range(NLED)]
    return [255]*NLED

# party (twinkle) — port with a seeded RNG for reproducibility
def party_frames(nframes):
    random.seed(1234)
    CAP, LMIN, LRAND = 250, 6, 19
    life = [0]*NLED; age = [0]*NLED; tick = 0; out = []
    for _ in range(nframes):
        tick += 1; b = [0]*NLED
        for i in range(NLED):
            if life[i] <= 0: b[i] = 0; continue
            a = age[i]; age[i] += 1
            if a >= life[i]: life[i] = 0; b[i] = 0; continue
            on = max(1, life[i]//4)
            v = (CAP*a//on) if a < on else (CAP*(life[i]-a)//(life[i]-on))
            b[i] = max(0, min(CAP, v))
        if tick % 2 == 0:
            led = random.randrange(NLED)
            if life[led] <= 0: life[led] = LMIN + random.randrange(LRAND); age[led] = 0
        out.append(b)
    return out

# ---------------------------------------------------------------------------
# build each GIF
# ---------------------------------------------------------------------------
def hold(frame_arr, n): return [frame_arr]*n

# 1. BOOT: smiley → rising wings (bottom→top) → brief full ring
boot = []
boot += hold(f_smiley(), 26)                     # ~1.1s smiley
for k in range(0, 12):                            # wings rise, ~4 frames/level
    boot += hold(wings(k), 4)
boot += hold(wings(11), 10)                       # full-ring "wings complete" hold

# 2. OTA update: arc clockwise from 12 o'clock
ota = []
for n in range(0, NLED+1): ota += hold(fill(n), 2)
ota += hold(fill(NLED), 12)

# 3. Wi-Fi setup comet / 4. connecting spinner — one clean revolution each
comet_f = [comet(f) for f in range(NLED)]
conn_f  = [connecting(f) for f in range(NLED)]

# 5. idle breathe (step 2 → half the frames, double duration = same real speed)
breathe_f = [breathe(f) for f in range(0, 84, 2)]
# 6. sleep
sleep_f   = [sleep(f) for f in range(0, 120, 2)]
# 7. muted
muted_f   = [muted(f) for f in range(0, 84, 2)]

# 8. volume: a knob turn up to ~70% then hold
vol = []
for v in range(0, 71, 3): vol += hold(volume(v), 2)
vol += hold(volume(70), 14)

# 9. button pulse: the 350ms flash + a rest, looped
btn = [button_pulse(e) for e in range(0, 350, POLL_MS)] + hold([0]*NLED, 12)

# 10. holding the knob (arming): wifi fill, then reset fill, compressed ~1.6s each
arm = []
for held in range(0, 10000, 320):  arm += [arming(held)]     # wifi-setup fill
for held in range(10000, 30000, 640): arm += [arming(held)]  # reset fill (brighter bg)
arm += hold(arming(30000), 8)

# 11. playing twinkle
party = party_frames(84)

# 12. failsafe: alternating warning ring, ~3 Hz blink
fs_even = [120 if i % 2 == 0 else 0 for i in range(NLED)]
fs_odd  = [0 if i % 2 == 0 else 120 for i in range(NLED)]

jobs = [
    (boot,     "boot.gif",            "Boot: smiley -> wings rise -> ready"),
    (comet_f,  "wifi-setup.gif",      "Wi-Fi setup mode (reconfigure me)"),
    (conn_f,   "wifi-connecting.gif", "Wi-Fi connecting"),
    (breathe_f,"idle.gif",            "Idle (on, nothing playing)"),
    (sleep_f,  "sleep.gif",           "Sleep (deep idle)"),
    (vol,      "volume.gif",          "Volume (turning the knob)"),
    (party,    "playing.gif",         "Playing (AirPlay)"),
    (muted_f,  "muted.gif",           "Muted"),
    (btn,      "button-press.gif",    "Button press"),
    (arm,      "holding-knob.gif",    "Holding knob (10s Wi-Fi / 30s reset)"),
    (ota,      "ota-update.gif",      "Firmware update (OTA)"),
]
print("rendering GIFs →", OUTDIR)
for arrs, name, label in jobs:
    frames = [render(a, label) for a in arrs]
    dur = POLL_MS*2 if name in ("idle.gif", "sleep.gif", "muted.gif") else POLL_MS
    save(frames, name, durations=[dur]*len(frames))

# failsafe: two-phase blink
fs_frames = [render(fs_even, "Failsafe / warning") for _ in range(3)] + \
            [render(fs_odd,  "Failsafe / warning") for _ in range(3)]
save(fs_frames, "failsafe.gif", durations=[150]*len(fs_frames))
print("done.")

"""Render a 400x480 dark weather card for the Whale-Maid AMOLED screen.

Reads structured weather JSON from stdin, writes a JPEG to argv[1].
Key-free: Pillow + bundled Windows Chinese fonts (Microsoft YaHei).
"""
import sys
import json
from datetime import datetime
from PIL import Image, ImageDraw, ImageFont

W, H = 400, 480
BOLD = "C:/Windows/Fonts/msyhbd.ttc"
REG = "C:/Windows/Fonts/msyh.ttc"

TOP = (11, 18, 32)
BOT = (20, 29, 51)
WHITE = (240, 245, 255)
GRAY = (148, 163, 184)
ACCENT = (56, 189, 248)
CARD = (30, 41, 59)
SUN = (250, 204, 21)
RAIN = (96, 165, 250)
SNOW = (226, 232, 240)


def font(path, size):
    return ImageFont.truetype(path, size, index=0)


def gradient():
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / (H - 1)
        c = tuple(int(TOP[i] + (BOT[i] - TOP[i]) * t) for i in range(3))
        d.line([(0, y), (W, y)], fill=c)
    return img


def kind(code):
    if code in (0, 1):
        return "sun"
    if code in (2, 3):
        return "cloud"
    if code in (45, 48):
        return "fog"
    if code in (71, 73, 75, 77, 85, 86):
        return "snow"
    if code in (95, 96, 99):
        return "thunder"
    return "rain"


def cloud(d, cx, cy, s, color):
    r = s * 0.28
    d.ellipse([cx - s * 0.45, cy - r * 0.2, cx - s * 0.05, cy + r * 1.1], fill=color)
    d.ellipse([cx - s * 0.2, cy - r * 0.9, cx + s * 0.25, cy + r * 0.5], fill=color)
    d.ellipse([cx + s * 0.02, cy - r * 0.3, cx + s * 0.45, cy + r * 1.0], fill=color)
    d.rounded_rectangle([cx - s * 0.45, cy + r * 0.2, cx + s * 0.45, cy + r * 1.1], radius=r * 0.5, fill=color)


def icon(d, cx, cy, s, code):
    k = kind(code)
    if k == "sun":
        r = s * 0.26
        for a in range(0, 360, 45):
            import math
            rad = math.radians(a)
            x1 = cx + math.cos(rad) * r * 1.35
            y1 = cy + math.sin(rad) * r * 1.35
            x2 = cx + math.cos(rad) * r * 1.75
            y2 = cy + math.sin(rad) * r * 1.75
            d.line([(x1, y1), (x2, y2)], fill=SUN, width=max(3, int(s * 0.06)))
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=SUN)
        return
    base = (148, 163, 184) if k in ("cloud", "fog") else (100, 116, 139)
    cloud(d, cx, cy - s * 0.12, s, base)
    if k == "fog":
        for i in range(3):
            y = cy + s * (0.28 + i * 0.14)
            d.line([(cx - s * 0.4, y), (cx + s * 0.4, y)], fill=GRAY, width=max(2, int(s * 0.04)))
    elif k == "rain":
        for i in range(3):
            x = cx - s * 0.26 + i * s * 0.26
            d.line([(x, cy + s * 0.26), (x - s * 0.08, cy + s * 0.46)], fill=RAIN, width=max(3, int(s * 0.06)))
    elif k == "snow":
        for i in range(3):
            x = cx - s * 0.26 + i * s * 0.26
            y = cy + s * 0.36
            rr = s * 0.05
            d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=SNOW)
    elif k == "thunder":
        d.polygon([(cx + s * 0.02, cy + s * 0.2), (cx - s * 0.16, cy + s * 0.46),
                   (cx - s * 0.0, cy + s * 0.46), (cx - s * 0.08, cy + s * 0.66),
                   (cx + s * 0.16, cy + s * 0.36), (cx + s * 0.0, cy + s * 0.36)], fill=SUN)


def advice(temp):
    if temp >= 28:
        return "炎热，短袖短裤，注意防晒补水"
    if temp >= 22:
        return "温暖舒适，短袖或薄长袖即可"
    if temp >= 16:
        return "微凉，长袖加一件薄外套"
    if temp >= 10:
        return "较凉，建议夹克或风衣"
    if temp >= 4:
        return "冷，毛衣加保暖外套"
    return "寒冷，羽绒服出行，注意保暖"


def center(d, text, fnt, cx, y, fill):
    d.text((cx, y), text, font=fnt, fill=fill, anchor="mm")


def render(data):
    img = gradient()
    d = ImageDraw.Draw(img)
    # accent glow top-right
    d.ellipse([W - 150, -90, W + 90, 150], fill=(23, 37, 64))

    city = str(data.get("city") or data.get("label") or "天气")[:10]
    d.text((28, 34), city, font=font(BOLD, 36), fill=WHITE)
    now = datetime.now()
    wd = "一二三四五六日"[now.weekday()]
    d.text((28, 80), f"实时天气 · {now.month}月{now.day}日 周{wd}", font=font(REG, 18), fill=GRAY)
    d.line([(28, 112), (W - 28, 112)], fill=(51, 65, 85), width=2)

    temp = data.get("temp", 0)
    d.text((28, 190), f"{temp}", font=font(BOLD, 116), fill=WHITE, anchor="lm")
    tw = d.textlength(f"{temp}", font=font(BOLD, 116))
    d.text((28 + tw + 6, 150), "°C", font=font(BOLD, 40), fill=ACCENT)
    d.text((30, 244), str(data.get("text", "")), font=font(BOLD, 30), fill=ACCENT)
    icon(d, 305, 185, 120, data.get("code", 0))

    feels = data.get("feels", temp)
    hum = data.get("humidity", 0)
    wind = data.get("wind", 0)
    d.text((28, 292), f"体感 {feels}°C   湿度 {hum}%   风速 {wind} km/h", font=font(REG, 20), fill=GRAY)

    # clothing advice card
    d.rounded_rectangle([28, 326, W - 28, 392], radius=14, fill=CARD)
    d.rectangle([28, 326, 34, 392], fill=ACCENT)
    d.text((48, 340), "穿衣建议", font=font(BOLD, 18), fill=ACCENT)
    d.text((48, 366), advice(temp), font=font(REG, 20), fill=WHITE)

    # 3-day forecast
    days = data.get("daily") or []
    if days:
        colw = (W - 56) / 3
        for i, day in enumerate(days[:3]):
            cx = 28 + colw * i + colw / 2
            label = day.get("date", "")[5:].replace("-", "/")
            center(d, label, font(REG, 18), cx, 416, GRAY)
            center(d, day.get("text", "")[:4], font(REG, 18), cx, 440, WHITE)
            center(d, f"{day.get('min',0)}~{day.get('max',0)}°", font(BOLD, 18), cx, 464, ACCENT)
    return img


def main():
    out = sys.argv[1]
    # stdin bytes are UTF-8; Windows console codepage (GBK) would mojibake Chinese.
    data = json.loads(sys.stdin.buffer.read().decode("utf-8"))
    img = render(data)
    img.save(out, "JPEG", quality=80)


if __name__ == "__main__":
    main()
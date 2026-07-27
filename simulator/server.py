#!/usr/bin/env python3
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
from math import atan2, cos, radians, sin, sqrt
from time import time
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlencode, urlparse
from urllib.request import Request, urlopen
import json
import re

from PIL import Image, ImageDraw, ImageFilter, ImageFont


OPEN_SKY_URL = "https://opensky-network.org/api/states/all"
PLANESPOTTERS_URL = "https://api.planespotters.net/pub/photos/hex/{hex_code}"
ADSBDB_CALLSIGN_URL = "https://api.adsbdb.com/v0/callsign/{callsign}"
OSM_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
TRAFFIC_CACHE_SECONDS = 12
AIRCRAFT_CACHE_SECONDS = 86400
ROUTE_CACHE_SECONDS = 21600
TILE_CACHE_SECONDS = 86400
traffic_cache = {}
aircraft_cache = {}
route_cache = {}
tile_cache = {}


def clamp_float(value, default, low, high):
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return default
    return max(low, min(high, parsed))


def bounding_box(lat, lon, range_km):
    lat_delta = range_km / 111.0
    lon_delta = range_km / max(10.0, 111.0 * cos(radians(lat)))
    return {
        "lamin": lat - lat_delta,
        "lamax": lat + lat_delta,
        "lomin": lon - lon_delta,
        "lomax": lon + lon_delta,
    }


def opensky_states(lat, lon, range_km):
    box = bounding_box(lat, lon, range_km)
    params = urlencode({
        "lamin": f"{box['lamin']:.5f}",
        "lomin": f"{box['lomin']:.5f}",
        "lamax": f"{box['lamax']:.5f}",
        "lomax": f"{box['lomax']:.5f}",
    })
    url = f"{OPEN_SKY_URL}?{params}"
    request = Request(url, headers={
        "Accept": "application/json",
        "User-Agent": "FlightDeskSimulator/0.1 (+https://github.com/jeanne0r/FlightDesk)",
    })
    with urlopen(request, timeout=8) as response:
        payload = json.loads(response.read().decode("utf-8"))
        return {
            "source": "opensky",
            "time": payload.get("time"),
            "states": payload.get("states") or [],
            "rate_limit_remaining": response.headers.get("x-rate-limit-remaining"),
        }


def aircraft_details_from_photo(photo):
    if not photo:
        return {"photo": None, "type": None, "registration": None, "credit": None, "link": None}

    link = photo.get("link") or ""
    aircraft_type = type_from_planespotters_link(link)
    thumbnail = photo.get("thumbnail_large") or photo.get("thumbnail") or {}
    return {
        "photo": (thumbnail.get("src") or "").replace("\\/", "/") or None,
        "type": aircraft_type,
        "registration": registration_from_planespotters_link(link),
        "credit": photo.get("photographer"),
        "link": link,
    }


def registration_from_planespotters_link(link):
    match = re.search(r"/photo/\d+/([^/?]+)", link or "")
    if not match:
        return None
    slug = match.group(1)
    parts = slug.split("-")
    if len(parts) > 1 and re.fullmatch(r"[a-z0-9]{1,2}", parts[0]):
        return "-".join(parts[:2]).upper()
    return "-".join(parts[:2]).upper() if len(parts) > 1 else (parts[0].upper() if parts else None)


def type_from_planespotters_link(link):
    match = re.search(r"/photo/\d+/([^/?]+)", link or "")
    if not match:
        return None
    parts = match.group(1).split("-")
    makers = {"airbus", "boeing", "embraer", "bombardier", "cessna", "atr", "pilatus", "dassault", "gulfstream"}
    for index, part in enumerate(parts):
        if part in makers and index + 1 < len(parts):
            tail = parts[index:index + 3]
            return " ".join(piece.upper() if any(char.isdigit() for char in piece) else piece.title() for piece in tail)
    return None


def airport_summary(airport):
    if not isinstance(airport, dict):
        return None
    code = airport.get("iata_code") or airport.get("icao_code")
    if not code:
        return None
    return {
        "code": code,
        "icao": airport.get("icao_code"),
        "iata": airport.get("iata_code"),
        "city": airport.get("municipality"),
        "name": airport.get("name"),
        "country": airport.get("country_name"),
    }


def route_details_from_payload(payload):
    route = ((payload or {}).get("response") or {}).get("flightroute")
    if not isinstance(route, dict):
        return {"origin": None, "destination": None}
    return {
        "origin": airport_summary(route.get("origin")),
        "destination": airport_summary(route.get("destination")),
    }


class FlightDeskHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/traffic":
            self.handle_traffic(parsed)
            return
        if parsed.path == "/api/esp32/radar.png":
            self.handle_esp32_radar(parsed)
            return
        if parsed.path == "/api/aircraft":
            self.handle_aircraft(parsed)
            return
        if parsed.path.startswith("/api/tile/"):
            self.handle_tile(parsed)
            return
        if parsed.path in ("/live", "/live.html") or parsed.path.startswith("/live"):
            self.path = "/index.html"
        super().do_GET()

    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def handle_traffic(self, parsed):
        query = parse_qs(parsed.query)
        lat = clamp_float((query.get("lat") or [None])[0], 46.5197, -85.0, 85.0)
        lon = clamp_float((query.get("lon") or [None])[0], 6.6323, -180.0, 180.0)
        range_km = clamp_float((query.get("range") or [None])[0], 100.0, 20.0, 250.0)
        cache_key = (round(lat, 3), round(lon, 3), round(range_km))
        now = time()

        cached = traffic_cache.get(cache_key)
        if cached and now - cached["created_at"] < TRAFFIC_CACHE_SECONDS:
            self.write_json(cached["payload"])
            return

        try:
            payload = opensky_states(lat, lon, range_km)
            payload["center"] = {"lat": lat, "lon": lon}
            payload["range_km"] = range_km
            traffic_cache[cache_key] = {"created_at": now, "payload": payload}
            self.write_json(payload)
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as error:
            status = getattr(error, "code", 502)
            self.write_json({
                "source": "opensky",
                "error": str(error),
                "status": status,
                "states": [],
                "center": {"lat": lat, "lon": lon},
                "range_km": range_km,
            }, status=502)

    def write_json(self, payload, status=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_aircraft(self, parsed):
        query = parse_qs(parsed.query)
        hex_code = ((query.get("hex") or [""])[0] or "").lower().strip()
        callsign = re.sub(r"[^A-Za-z0-9]", "", ((query.get("callsign") or [""])[0] or "").upper())
        if not re.fullmatch(r"[0-9a-f]{6}", hex_code):
            self.write_json({"error": "invalid hex", "photo": None, "type": None}, status=400)
            return

        now = time()
        cache_key = (hex_code, callsign)
        cached = aircraft_cache.get(cache_key)
        if cached and now - cached["created_at"] < AIRCRAFT_CACHE_SECONDS:
            self.write_json(cached["payload"])
            return

        request = Request(PLANESPOTTERS_URL.format(hex_code=hex_code), headers={
            "Accept": "application/json",
            "User-Agent": "FlightDeskSimulator/0.1 (+https://github.com/jeanne0r/FlightDesk)",
        })

        try:
            with urlopen(request, timeout=8) as response:
                raw = json.loads(response.read().decode("utf-8"))
                photo = (raw.get("photos") or [None])[0]
                payload = aircraft_details_from_photo(photo)
                payload.update(self.route_for_callsign(callsign))
                aircraft_cache[cache_key] = {"created_at": now, "payload": payload}
                self.write_json(payload)
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as error:
            payload = {"error": str(error), "photo": None, "type": None}
            payload.update(self.route_for_callsign(callsign))
            self.write_json(payload)

    def route_for_callsign(self, callsign):
        if not callsign:
            return {"origin": None, "destination": None}

        now = time()
        cached = route_cache.get(callsign)
        if cached and now - cached["created_at"] < ROUTE_CACHE_SECONDS:
            return cached["payload"]

        request = Request(ADSBDB_CALLSIGN_URL.format(callsign=callsign), headers={
            "Accept": "application/json",
            "User-Agent": "FlightDeskSimulator/0.1 (+https://github.com/jeanne0r/FlightDesk)",
        })

        try:
            with urlopen(request, timeout=6) as response:
                raw = json.loads(response.read().decode("utf-8"))
                payload = route_details_from_payload(raw)
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError):
            payload = {"origin": None, "destination": None}

        route_cache[callsign] = {"created_at": now, "payload": payload}
        return payload

    def handle_tile(self, parsed):
        match = re.fullmatch(r"/api/tile/(\d+)/(\d+)/(\d+)\.png", parsed.path)
        if not match:
            self.send_error(404)
            return

        z, x, y = (int(value) for value in match.groups())
        if z < 0 or z > 18:
            self.send_error(400, "invalid zoom")
            return

        max_tile = 2 ** z
        if x < 0 or x >= max_tile or y < 0 or y >= max_tile:
            self.send_error(400, "invalid tile")
            return

        key = (z, x, y)
        now = time()
        cached = tile_cache.get(key)
        if cached and now - cached["created_at"] < TILE_CACHE_SECONDS:
            self.write_tile(cached["body"])
            return

        url = OSM_TILE_URL.format(z=z, x=x, y=y)
        request = Request(url, headers={
            "Accept": "image/png",
            "User-Agent": "FlightDeskSimulator/0.1 (+https://github.com/jeanne0r/FlightDesk)",
        })

        try:
            with urlopen(request, timeout=8) as response:
                body = response.read()
                tile_cache[key] = {"created_at": now, "body": body}
                self.write_tile(body)
        except (HTTPError, URLError, TimeoutError) as error:
            self.send_error(502, str(error))

    def write_tile(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Cache-Control", "public, max-age=86400")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_esp32_radar(self, parsed):
        query = parse_qs(parsed.query)
        lat = clamp_float((query.get("lat") or [None])[0], 46.5197, -85.0, 85.0)
        lon = clamp_float((query.get("lon") or [None])[0], 6.6323, -180.0, 180.0)
        range_km = clamp_float((query.get("range") or [None])[0], 50.0, 20.0, 250.0)

        try:
            payload = opensky_states(lat, lon, range_km)
            aircraft = compact_aircraft(payload.get("states") or [], lat, lon, range_km)
            body = render_radar_png(aircraft, range_km, source="LIVE")
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError):
            body = render_radar_png([], range_km, source="OFFLINE")

        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def haversine_km(lat1, lon1, lat2, lon2):
    earth_km = 6371.0
    p1 = radians(lat1)
    p2 = radians(lat2)
    dp = radians(lat2 - lat1)
    dl = radians(lon2 - lon1)
    a = sin(dp / 2) ** 2 + cos(p1) * cos(p2) * sin(dl / 2) ** 2
    return earth_km * 2 * atan2(sqrt(a), sqrt(1 - a))


def initial_bearing_deg(lat1, lon1, lat2, lon2):
    p1 = radians(lat1)
    p2 = radians(lat2)
    dl = radians(lon2 - lon1)
    y = sin(dl) * cos(p2)
    x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)
    return (atan2(y, x) * 180 / 3.141592653589793 + 360) % 360


def compact_aircraft(states, lat, lon, range_km):
    aircraft = []
    for row in states:
        if not isinstance(row, list) or len(row) < 11:
            continue
        aircraft_lon = row[5]
        aircraft_lat = row[6]
        on_ground = bool(row[8])
        if not isinstance(aircraft_lat, (int, float)) or not isinstance(aircraft_lon, (int, float)) or on_ground:
            continue
        distance = haversine_km(lat, lon, aircraft_lat, aircraft_lon)
        if distance > range_km:
            continue
        aircraft.append({
            "callsign": str(row[1] or row[0] or "").strip()[:8],
            "distance": distance,
            "bearing": initial_bearing_deg(lat, lon, aircraft_lat, aircraft_lon),
            "heading": row[10] if isinstance(row[10], (int, float)) else 0,
        })
    return sorted(aircraft, key=lambda item: item["distance"])


def render_radar_png(aircraft, range_km, source):
    size = 240
    center = size // 2
    radius = 106
    green = (102, 255, 110)
    dim = (26, 94, 44)

    image = Image.new("RGB", (size, size), (0, 0, 0))
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(glow)

    draw.ellipse((6, 6, 234, 234), fill=(3, 18, 7, 255), outline=(52, 84, 58, 255), width=4)
    draw.ellipse((20, 20, 220, 220), outline=(26, 88, 42, 150), width=2)
    for index in range(1, 5):
        r = radius * index / 4
        draw.ellipse((center - r, center - r, center + r, center + r), outline=(30, 110, 50, 95), width=1)
    for angle in range(0, 360, 30):
        a = radians(angle)
        draw.line((center, center, center + cos(a) * radius, center + sin(a) * radius), fill=(38, 118, 56, 90), width=1)

    sweep_angle = radians((int(time() * 50) % 360) - 90)
    draw.pieslice((center - radius, center - radius, center + radius, center + radius), int(time() * 50) % 360 - 34, int(time() * 50) % 360, fill=(82, 224, 121, 44))
    draw.line((center, center, center + cos(sweep_angle) * radius, center + sin(sweep_angle) * radius), fill=(120, 255, 120, 190), width=2)

    font_small = ImageFont.load_default()
    font_big = ImageFont.load_default(size=34)
    font_mid = ImageFont.load_default(size=14)
    draw.text((center, 26), "FLIGHTDESK", fill=green, font=font_mid, anchor="mm")
    draw.text((center, 45), source, fill=(110, 210, 116), font=font_small, anchor="mm")
    draw.text((center, 74), str(len(aircraft)), fill=green, font=font_big, anchor="mm")
    draw.text((center, 94), "AVIONS", fill=green, font=font_mid, anchor="mm")
    draw.text((185, 121), str(int(range_km)), fill=(120, 210, 110), font=font_small, anchor="lm")
    draw.text((185, 135), "KM", fill=(120, 210, 110), font=font_small, anchor="lm")

    for item in aircraft[:80]:
        a = radians(item["bearing"] - 90)
        r = radius * item["distance"] / range_km
        x = center + cos(a) * r
        y = center + sin(a) * r
        h = radians((item["heading"] or item["bearing"]) - 90)
        points = [
            (x + cos(h) * 9, y + sin(h) * 9),
            (x + cos(h + 2.55) * 7, y + sin(h + 2.55) * 7),
            (x + cos(h + 3.14) * 2, y + sin(h + 3.14) * 2),
            (x + cos(h - 2.55) * 7, y + sin(h - 2.55) * 7),
        ]
        halo = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        hdraw = ImageDraw.Draw(halo)
        hdraw.polygon(points, fill=(141, 255, 111, 190), outline=(196, 255, 165, 220))
        glow.alpha_composite(halo.filter(ImageFilter.GaussianBlur(2)))
        draw.polygon(points, fill=(141, 255, 111, 210), outline=(210, 255, 180, 240))

    draw.ellipse((center - 5, center - 5, center + 5, center + 5), outline=green, width=1)
    image = Image.alpha_composite(image.convert("RGBA"), glow)

    buffer = BytesIO()
    image.convert("RGB").save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


if __name__ == "__main__":
    server = ThreadingHTTPServer(("0.0.0.0", 4173), FlightDeskHandler)
    print("FlightDesk simulator on http://0.0.0.0:4173")
    server.serve_forever()

#!/usr/bin/env python3
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
from math import atan2, cos, floor, log, log2, pi, radians, sin, sqrt, tan
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
esp32_state = {
    "lat": 46.5197,
    "lon": 6.6323,
    "home_lat": 46.5197,
    "home_lon": 6.6323,
    "range_km": 50.0,
    "mode": "radar",
    "selected_id": None,
    "favorites": set(),
    "last_aircraft": [],
}


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
        if parsed.path == "/api/esp32/action":
            self.handle_esp32_action(parsed)
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
        lat = clamp_float((query.get("lat") or [esp32_state["lat"]])[0], esp32_state["lat"], -85.0, 85.0)
        lon = clamp_float((query.get("lon") or [esp32_state["lon"]])[0], esp32_state["lon"], -180.0, 180.0)
        range_km = clamp_float((query.get("range") or [esp32_state["range_km"]])[0], esp32_state["range_km"], 20.0, 250.0)
        esp32_state["lat"] = lat
        esp32_state["lon"] = lon
        esp32_state["range_km"] = range_km

        try:
            payload = opensky_states(lat, lon, range_km)
            aircraft = compact_aircraft(payload.get("states") or [], lat, lon, range_km)
            esp32_state["last_aircraft"] = aircraft
            if esp32_state["selected_id"] and not any(item["id"] == esp32_state["selected_id"] for item in aircraft):
                esp32_state["selected_id"] = None
            body = render_radar_png(aircraft, esp32_state, source="LIVE")
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError):
            body = render_radar_png(esp32_state["last_aircraft"], esp32_state, source="OFFLINE")

        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_esp32_action(self, parsed):
        query = parse_qs(parsed.query)
        action = (query.get("action") or ["tap"])[0]
        x = clamp_float((query.get("x") or [None])[0], -1, -1000, 1000)
        y = clamp_float((query.get("y") or [None])[0], -1, -1000, 1000)

        if action == "tap" and x >= 0 and y >= 0:
            handle_esp32_tap(x, y)
        elif action == "zoom_in":
            cycle_esp32_range(-1)
        elif action == "zoom_out":
            cycle_esp32_range(1)
        elif action == "recenter":
            esp32_state["lat"] = esp32_state["home_lat"]
            esp32_state["lon"] = esp32_state["home_lon"]
            esp32_state["selected_id"] = None
        elif action == "close":
            esp32_state["selected_id"] = None
        self.write_json({"ok": True, "state": serializable_esp32_state()})


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
        callsign = str(row[1] or row[0] or "").strip()[:8]
        aircraft.append({
            "id": str(row[0] or callsign),
            "hex": str(row[0] or "").lower(),
            "callsign": callsign,
            "country": str(row[2] or "OpenSky"),
            "distance": distance,
            "bearing": initial_bearing_deg(lat, lon, aircraft_lat, aircraft_lon),
            "latitude": aircraft_lat,
            "longitude": aircraft_lon,
            "altitude": row[13] if isinstance(row[13] if len(row) > 13 else None, (int, float)) else row[7],
            "speed": (row[9] * 3.6) if isinstance(row[9], (int, float)) else 0,
            "heading": row[10] if isinstance(row[10], (int, float)) else 0,
        })
    return sorted(aircraft, key=lambda item: item["distance"])


def serializable_esp32_state():
    return {
        "lat": esp32_state["lat"],
        "lon": esp32_state["lon"],
        "range_km": esp32_state["range_km"],
        "mode": esp32_state["mode"],
        "selected_id": esp32_state["selected_id"],
        "favorites": sorted(esp32_state["favorites"]),
    }


def handle_esp32_tap(x, y):
    if esp32_state["selected_id"]:
        if 179 <= x <= 213 and 77 <= y <= 111:
            esp32_state["favorites"].add(esp32_state["selected_id"])
            return
        if 207 <= x <= 238 and 77 <= y <= 111:
            esp32_state["selected_id"] = None
            return

    nav = [
        ("radar", 19, 194, 60, 225),
        ("search", 63, 194, 104, 225),
        ("favorites", 107, 194, 148, 225),
        ("settings", 151, 194, 192, 225),
        ("assistant", 195, 194, 236, 225),
    ]
    for mode, x1, y1, x2, y2 in nav:
        if x1 <= x <= x2 and y1 <= y <= y2:
            esp32_state["mode"] = mode
            esp32_state["selected_id"] = None
            return

    if 12 <= x <= 42 and 28 <= y <= 58:
        esp32_state["lat"] = esp32_state["home_lat"]
        esp32_state["lon"] = esp32_state["home_lon"]
        esp32_state["selected_id"] = None
        return
    if 197 <= x <= 227 and 28 <= y <= 58:
        cycle_esp32_range(-1)
        return
    if 197 <= x <= 227 and 62 <= y <= 92:
        cycle_esp32_range(1)
        return

    if esp32_state["mode"] == "settings":
        ranges = [(20, 48, 122), (50, 86, 122), (100, 124, 122), (250, 166, 122)]
        for value, cx, cy in ranges:
            if abs(x - cx) <= 20 and abs(y - cy) <= 18:
                esp32_state["range_km"] = float(value)
                return

    selected = nearest_aircraft_at(x, y)
    if selected:
        esp32_state["mode"] = "radar"
        esp32_state["selected_id"] = selected["id"]


def nearest_aircraft_at(x, y):
    best = None
    best_distance = 18
    for item in esp32_state["last_aircraft"]:
        point = polar_to_screen(item["bearing"], item["distance"], esp32_state["range_km"])
        distance = sqrt((x - point[0]) ** 2 + (y - point[1]) ** 2)
        if distance < best_distance:
            best = item
            best_distance = distance
    return best


def cycle_esp32_range(direction):
    ranges = [20.0, 50.0, 100.0, 250.0]
    current = min(range(len(ranges)), key=lambda idx: abs(ranges[idx] - esp32_state["range_km"]))
    esp32_state["range_km"] = ranges[max(0, min(len(ranges) - 1, current + direction))]
    esp32_state["selected_id"] = None


def lat_lon_to_world(lat, lon, zoom):
    scale = 256 * (2 ** zoom)
    x = (lon + 180.0) / 360.0 * scale
    lat_rad = radians(max(-85.0511, min(85.0511, lat)))
    y = (1 - log(tan(lat_rad) + 1 / cos(lat_rad)) / pi) / 2 * scale
    return x, y


def fetch_osm_tile(z, x, y):
    max_tile = 2 ** z
    x %= max_tile
    if y < 0 or y >= max_tile:
        return None
    key = (z, x, y)
    now = time()
    cached = tile_cache.get(key)
    if cached and now - cached["created_at"] < TILE_CACHE_SECONDS:
        return cached["body"]
    url = OSM_TILE_URL.format(z=z, x=x, y=y)
    request = Request(url, headers={
        "Accept": "image/png",
        "User-Agent": "FlightDeskSimulator/0.1 (+https://github.com/jeanne0r/FlightDesk)",
    })
    with urlopen(request, timeout=5) as response:
        body = response.read()
        tile_cache[key] = {"created_at": now, "body": body}
        return body


def map_zoom_for_range(lat, range_km, radius_px):
    meters_per_px = range_km * 1000 / radius_px
    zoom = log2(156543.03392 * cos(radians(lat)) / max(1, meters_per_px))
    return int(max(5, min(13, round(zoom))))


def render_map_layer(lat, lon, range_km, size, radius):
    zoom = map_zoom_for_range(lat, range_km, radius)
    center_x, center_y = lat_lon_to_world(lat, lon, zoom)
    top_left_x = center_x - size / 2
    top_left_y = center_y - size / 2
    layer = Image.new("RGB", (size, size), (4, 9, 5))
    for tx in range(floor(top_left_x / 256), floor((top_left_x + size) / 256) + 1):
        for ty in range(floor(top_left_y / 256), floor((top_left_y + size) / 256) + 1):
            try:
                body = fetch_osm_tile(zoom, tx, ty)
            except (HTTPError, URLError, TimeoutError):
                body = None
            if not body:
                continue
            tile = Image.open(BytesIO(body)).convert("RGB")
            px = int(tx * 256 - top_left_x)
            py = int(ty * 256 - top_left_y)
            layer.paste(tile, (px, py))

    gray = layer.convert("L")
    green = Image.merge("RGB", (
        gray.point(lambda value: int(value * 0.05)),
        gray.point(lambda value: int(22 + value * 0.42)),
        gray.point(lambda value: int(value * 0.06)),
    ))
    return green.filter(ImageFilter.GaussianBlur(0.35))


def polar_to_screen(bearing, distance, range_km):
    size = 240
    center = size // 2
    radius = 96
    angle = radians(bearing - 90)
    r = radius * distance / range_km
    return center + cos(angle) * r, center + sin(angle) * r


def text(draw, xy, value, font, fill, anchor=None):
    draw.text(xy, str(value), font=font, fill=fill, anchor=anchor)


def render_radar_png(aircraft, state, source):
    size = 240
    center = size // 2
    radius = 96
    range_km = state["range_km"]
    green = (102, 255, 110)

    image = Image.new("RGBA", (size, size), (0, 0, 0, 255))
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(glow)

    try:
        map_layer = render_map_layer(state["lat"], state["lon"], range_km, size, radius)
        mask = Image.new("L", (size, size), 0)
        ImageDraw.Draw(mask).ellipse((4, 4, 236, 236), fill=112)
        image.alpha_composite(Image.composite(map_layer.convert("RGBA"), Image.new("RGBA", (size, size)), mask))
    except Exception:
        pass

    draw.ellipse((3, 3, 237, 237), outline=(57, 74, 58, 220), width=5)
    draw.ellipse((12, 12, 228, 228), fill=(0, 0, 0, 72), outline=(12, 30, 18, 240), width=4)
    draw.ellipse((24, 24, 216, 216), fill=(5, 18, 7, 88), outline=(44, 129, 57, 92), width=1)
    for index in range(1, 5):
        r = radius * index / 4
        draw.ellipse((center - r, center - r, center + r, center + r), outline=(62, 210, 82, 52), width=1)
    for angle in range(0, 360, 30):
        a = radians(angle)
        draw.line((center, center, center + cos(a) * radius, center + sin(a) * radius), fill=(76, 194, 83, 48), width=1)

    sweep_angle = radians((int(time() * 50) % 360) - 90)
    draw.pieslice((center - radius, center - radius, center + radius, center + radius), int(time() * 50) % 360 - 42, int(time() * 50) % 360, fill=(82, 224, 121, 44))
    draw.line((center, center, center + cos(sweep_angle) * radius, center + sin(sweep_angle) * radius), fill=(120, 255, 120, 178), width=2)

    font_tiny = load_font(8)
    font_small = load_font(10)
    font_big = load_font(32)
    font_mid = load_font(14)
    font_title = load_font(22)

    draw.text((center, 26), "18:47", fill=(232, 244, 234, 205), font=font_mid, anchor="mm")
    draw.text((center, 43), "LIVE OPENSKY" if source == "LIVE" else "OFFLINE", fill=(130, 235, 119, 190), font=font_tiny, anchor="mm")
    draw.text((center, 67), str(len(aircraft)), fill=green, font=font_big, anchor="mm")
    draw.text((center, 88), "AVIONS", fill=green, font=font_mid, anchor="mm")
    draw.text((176, 121), str(int(range_km * 0.4)), fill=(120, 210, 110, 165), font=font_small, anchor="lm")
    draw.text((207, 121), str(int(range_km)), fill=(120, 210, 110, 165), font=font_small, anchor="lm")
    draw.text((207, 135), "KM", fill=(120, 210, 110, 165), font=font_small, anchor="lm")

    draw.rounded_rectangle((14, 30, 39, 55), radius=8, fill=(1, 7, 3, 156), outline=(120, 255, 120, 150), width=1)
    draw.text((26, 43), "⌂", fill=(141, 255, 111, 220), font=font_small, anchor="mm")
    draw.rounded_rectangle((201, 31, 226, 56), radius=8, fill=(1, 7, 3, 156), outline=(120, 255, 120, 120), width=1)
    draw.text((213, 43), "+", fill=(141, 255, 111, 225), font=font_mid, anchor="mm")
    draw.rounded_rectangle((201, 64, 226, 89), radius=8, fill=(1, 7, 3, 156), outline=(120, 255, 120, 120), width=1)
    draw.text((213, 76), "-", fill=(141, 255, 111, 225), font=font_mid, anchor="mm")

    visible = aircraft
    for item in visible:
        x, y = polar_to_screen(item["bearing"], item["distance"], range_km)
        h = radians((item["heading"] or item["bearing"]) - 90)
        selected = item["id"] == state["selected_id"]
        favorite = item["id"] in state["favorites"]
        color = (240, 201, 90) if favorite else (141, 255, 111)
        scale = 1.35 if selected else 1.0
        points = [
            (x + cos(h) * 10 * scale, y + sin(h) * 10 * scale),
            (x + cos(h + 2.55) * 7 * scale, y + sin(h + 2.55) * 7 * scale),
            (x + cos(h + 3.14) * 2, y + sin(h + 3.14) * 2),
            (x + cos(h - 2.55) * 7 * scale, y + sin(h - 2.55) * 7 * scale),
        ]
        halo = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        hdraw = ImageDraw.Draw(halo)
        hdraw.polygon(points, fill=(*color, 190), outline=(220, 255, 190, 230))
        glow.alpha_composite(halo.filter(ImageFilter.GaussianBlur(3 if selected else 2)))
        draw.polygon(points, fill=(*color, 215), outline=(224, 255, 188, 240))

    draw.ellipse((center - 5, center - 5, center + 5, center + 5), outline=green, width=1)
    draw.text((center, center + 1), "⌂", fill=(100, 245, 114, 190), font=font_small, anchor="mm")

    if state["mode"] == "settings":
        draw_settings(draw, state, font_small, font_mid)
    elif state["mode"] != "radar":
        draw_mode_panel(draw, state["mode"], font_small, font_mid)

    selected = next((item for item in aircraft if item["id"] == state["selected_id"]), None)
    if selected:
        draw_aircraft_popup(draw, selected, state, font_tiny, font_small, font_mid, font_title)
    else:
        draw_nav(draw, state["mode"], font_tiny)

    vignette = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    vdraw = ImageDraw.Draw(vignette)
    vdraw.ellipse((0, 0, size, size), outline=(0, 0, 0, 255), width=18)
    vdraw.rectangle((0, 0, size, 18), fill=(0, 0, 0, 150))
    image = Image.alpha_composite(image.convert("RGBA"), glow)
    image = Image.alpha_composite(image, vignette)

    buffer = BytesIO()
    image.convert("RGB").save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def draw_nav(draw, active_mode, font):
    modes = [("radar", "RADAR"), ("search", "RECH"), ("favorites", "FAV"), ("settings", "RÉGL"), ("assistant", "IA")]
    x = 20
    for mode, label in modes:
        active = active_mode == mode
        draw.rounded_rectangle((x, 196, x + 36, 220), radius=10, fill=(6, 13, 8, 205), outline=(120, 255, 120, 210 if active else 70), width=2 if active else 1)
        draw.text((x + 18, 209), label, fill=(141, 255, 111, 240) if active else (225, 235, 228, 180), font=font, anchor="mm")
        x += 44


def draw_mode_panel(draw, mode, font_small, font_mid):
    titles = {
        "search": ("RECHERCHE", "Touchez un avion"),
        "favorites": ("FAVORIS", "Vols suivis en jaune"),
        "assistant": ("ASSISTANT", "Sélectionnez un vol"),
    }
    title, subtitle = titles.get(mode, ("RADAR", ""))
    draw.rounded_rectangle((42, 101, 198, 148), radius=12, fill=(2, 9, 5, 218), outline=(82, 224, 121, 105), width=1)
    draw.text((120, 119), title, fill=(141, 255, 111, 235), font=font_mid, anchor="mm")
    draw.text((120, 137), subtitle, fill=(238, 244, 239, 180), font=font_small, anchor="mm")
    draw_nav(draw, mode, font_small)


def draw_settings(draw, state, font_small, font_mid):
    draw.rounded_rectangle((24, 55, 216, 185), radius=16, fill=(3, 10, 6, 232), outline=(82, 224, 121, 130), width=2)
    draw.text((42, 78), "RÉGLAGES", fill=(141, 255, 111, 240), font=font_mid, anchor="lm")
    draw.text((42, 101), "Rayon", fill=(238, 244, 239, 160), font=font_small, anchor="lm")
    x = 34
    for value in (20, 50, 100, 250):
        active = int(state["range_km"]) == value
        draw.rounded_rectangle((x, 114, x + 35, 137), radius=8, fill=(82, 224, 121, 55 if active else 18), outline=(141, 255, 111, 180 if active else 80), width=1)
        draw.text((x + 17, 126), str(value), fill=(141, 255, 111, 240) if active else (238, 244, 239, 170), font=font_small, anchor="mm")
        x += 42
    draw.text((42, 158), "Source", fill=(238, 244, 239, 150), font=font_small, anchor="lm")
    draw.text((94, 158), "OpenSky + carte", fill=(141, 255, 111, 225), font=font_small, anchor="lm")
    draw_nav(draw, "settings", font_small)


def clipped(value, size):
    value = str(value or "INCONNU")
    if len(value) <= size:
        return value
    return value[:size - 1] + "…"


def draw_aircraft_popup(draw, aircraft, state, font_tiny, font_small, font_mid, font_title):
    details = lookup_aircraft_details_for_png(aircraft)
    draw.rounded_rectangle((43, 82, 218, 174), radius=12, fill=(3, 11, 6, 232), outline=(82, 224, 121, 165), width=2)
    draw.text((55, 101), "AVION SÉLECTIONNÉ", fill=(141, 255, 111, 220), font=font_tiny, anchor="lm")
    draw.text((55, 126), clipped(aircraft["callsign"], 8), fill=(112, 255, 113, 255), font=font_title, anchor="lm")
    subtitle = clipped(details.get("type") or aircraft.get("country") or "OpenSky", 20)
    draw.text((55, 144), subtitle, fill=(238, 244, 239, 188), font=font_small, anchor="lm")
    route = city_route(details)
    if route:
        draw.text((55, 160), clipped(route, 18), fill=(238, 244, 239, 160), font=font_tiny, anchor="lm")
    else:
        draw.text((55, 160), f"{round(aircraft['distance'])} km  {round(aircraft['speed'])} km/h", fill=(238, 244, 239, 180), font=font_tiny, anchor="lm")
    draw.text((168, 126), f"{round(aircraft['altitude'] or 0)} m", fill=(238, 244, 239, 205), font=font_small, anchor="lm")
    draw.text((168, 143), f"{round(aircraft['heading'] or 0)}°", fill=(238, 244, 239, 180), font=font_small, anchor="lm")
    draw.rounded_rectangle((181, 88, 205, 112), radius=12, fill=(82, 224, 121, 28), outline=(141, 255, 111, 120), width=1)
    draw.text((193, 100), "★", fill=(141, 255, 111, 230), font=font_small, anchor="mm")
    draw.rounded_rectangle((209, 88, 232, 112), radius=12, fill=(82, 224, 121, 28), outline=(141, 255, 111, 120), width=1)
    draw.text((220, 100), "×", fill=(141, 255, 111, 240), font=font_mid, anchor="mm")


def lookup_aircraft_details_for_png(aircraft):
    hex_code = aircraft.get("hex")
    callsign = re.sub(r"[^A-Za-z0-9]", "", aircraft.get("callsign") or "")
    if not hex_code or not re.fullmatch(r"[0-9a-f]{6}", hex_code):
        return {}
    key = (hex_code, callsign)
    cached = aircraft_cache.get(key)
    if cached and time() - cached["created_at"] < AIRCRAFT_CACHE_SECONDS:
        return cached["payload"]
    payload = {"origin": None, "destination": None, "type": None, "photo": None}
    try:
        request = Request(PLANESPOTTERS_URL.format(hex_code=hex_code), headers={"Accept": "application/json", "User-Agent": "FlightDeskSimulator/0.1"})
        with urlopen(request, timeout=2) as response:
            raw = json.loads(response.read().decode("utf-8"))
            payload.update(aircraft_details_from_photo((raw.get("photos") or [None])[0]))
    except (HTTPError, URLError, TimeoutError, json.JSONDecodeError):
        pass
    payload.update(FlightDeskHandler.route_for_callsign(None, callsign))
    aircraft_cache[key] = {"created_at": time(), "payload": payload}
    return payload


def city_route(details):
    origin = (details.get("origin") or {}).get("city")
    destination = (details.get("destination") or {}).get("city")
    if origin and destination:
        return f"{origin} → {destination}"
    return None


def load_font(size):
    for path in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        try:
            return ImageFont.truetype(path, size=size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)


if __name__ == "__main__":
    server = ThreadingHTTPServer(("0.0.0.0", 4173), FlightDeskHandler)
    print("FlightDesk simulator on http://0.0.0.0:4173")
    server.serve_forever()

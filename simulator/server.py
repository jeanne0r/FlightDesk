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

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont


OPEN_SKY_URL = "https://opensky-network.org/api/states/all"
AIRPLANES_LIVE_POINT_URL = "https://api.airplanes.live/v2/point/{lat:.5f}/{lon:.5f}/{radius_nm:.1f}"
PLANESPOTTERS_URL = "https://api.planespotters.net/pub/photos/hex/{hex_code}"
ADSBDB_CALLSIGN_URL = "https://api.adsbdb.com/v0/callsign/{callsign}"
NOMINATIM_SEARCH_URL = "https://nominatim.openstreetmap.org/search"
OSM_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
USER_AGENT = "FlightDesk/0.1 (+https://github.com/jeanne0r/FlightDesk/issues)"
TRAFFIC_CACHE_SECONDS = 30
AIRCRAFT_CACHE_SECONDS = 86400
ROUTE_CACHE_SECONDS = 21600
TILE_CACHE_SECONDS = 86400
traffic_cache = {}
aircraft_cache = {}
route_cache = {}
tile_cache = {}
photo_cache = {}
esp32_state = {
    "lat": 46.5197,
    "lon": 6.6323,
    "home_lat": 46.5197,
    "home_lon": 6.6323,
    "postal_code": "1188",
    "postal_draft": "1188",
    "place": "Gimel",
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
        "User-Agent": USER_AGENT,
    })
    with urlopen(request, timeout=8) as response:
        payload = json.loads(response.read().decode("utf-8"))
        return {
            "source": "opensky",
            "time": payload.get("time"),
            "states": payload.get("states") or [],
            "rate_limit_remaining": response.headers.get("x-rate-limit-remaining"),
        }


def airplanes_live_states(lat, lon, range_km):
    radius_nm = max(1.0, min(250.0, range_km / 1.852))
    url = AIRPLANES_LIVE_POINT_URL.format(lat=lat, lon=lon, radius_nm=radius_nm)
    request = Request(url, headers={
        "Accept": "application/json",
        "User-Agent": USER_AGENT,
    })
    with urlopen(request, timeout=8) as response:
        payload = json.loads(response.read().decode("utf-8"))
        return {
            "source": "airplanes.live",
            "time": payload.get("now"),
            "aircraft": compact_airplanes_live(payload.get("ac") or [], lat, lon, range_km),
        }


def geocode_postal_code(postal_code):
    params = urlencode({
        "format": "jsonv2",
        "country": "Switzerland",
        "postalcode": postal_code,
        "limit": "1",
        "addressdetails": "1",
    })
    request = Request(f"{NOMINATIM_SEARCH_URL}?{params}", headers={
        "Accept": "application/json",
        "User-Agent": USER_AGENT,
    })
    with urlopen(request, timeout=6) as response:
        payload = json.loads(response.read().decode("utf-8"))
    if not payload:
        return None
    result = payload[0]
    address = result.get("address") or {}
    return {
        "lat": float(result["lat"]),
        "lon": float(result["lon"]),
        "place": address.get("village") or address.get("town") or address.get("city") or result.get("name") or postal_code,
    }


def numeric(value):
    if isinstance(value, (int, float)):
        return float(value)
    return None


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
            "User-Agent": USER_AGENT,
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
            "User-Agent": USER_AGENT,
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
            "User-Agent": USER_AGENT,
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

        source = "AIRPLANES"
        try:
            payload = airplanes_live_states(lat, lon, range_km)
            aircraft = payload.get("aircraft") or []
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError):
            try:
                payload = opensky_states(lat, lon, range_km)
                aircraft = compact_aircraft(payload.get("states") or [], lat, lon, range_km)
                source = "OPENSKY"
            except (HTTPError, URLError, TimeoutError, json.JSONDecodeError):
                source = "CACHE" if esp32_state["last_aircraft"] else "OFFLINE"
                aircraft = esp32_state["last_aircraft"]

        if source != "CACHE" and source != "OFFLINE":
            esp32_state["last_aircraft"] = aircraft
            if esp32_state["selected_id"] and not any(item["id"] == esp32_state["selected_id"] for item in aircraft):
                esp32_state["selected_id"] = None
        body = render_radar_png(aircraft, esp32_state, source=source)

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


def compact_airplanes_live(items, lat, lon, range_km):
    aircraft = []
    for item in items:
        if not isinstance(item, dict):
            continue
        aircraft_lat = numeric(item.get("lat"))
        aircraft_lon = numeric(item.get("lon"))
        if aircraft_lat is None or aircraft_lon is None:
            continue
        if item.get("ground") == "1" or item.get("alt_baro") == "ground":
            continue

        distance = haversine_km(lat, lon, aircraft_lat, aircraft_lon)
        if distance > range_km:
            continue

        callsign = str(item.get("flight") or item.get("r") or item.get("hex") or "").strip()[:8]
        altitude_ft = numeric(item.get("alt_geom")) or numeric(item.get("alt_baro"))
        speed_kt = numeric(item.get("gs"))
        heading = numeric(item.get("track")) or numeric(item.get("true_heading")) or numeric(item.get("mag_heading")) or 0
        aircraft.append({
            "id": str(item.get("hex") or callsign),
            "hex": str(item.get("hex") or "").lower(),
            "callsign": callsign or "LIVE",
            "country": str(item.get("ownOp") or item.get("dbFlags") or "Airplanes.live"),
            "distance": distance,
            "bearing": initial_bearing_deg(lat, lon, aircraft_lat, aircraft_lon),
            "latitude": aircraft_lat,
            "longitude": aircraft_lon,
            "altitude": altitude_ft * 0.3048 if altitude_ft is not None else 0,
            "speed": speed_kt * 1.852 if speed_kt is not None else 0,
            "heading": heading,
            "aircraft_type": item.get("desc") or item.get("t"),
            "registration": item.get("r"),
        })
    return sorted(aircraft, key=lambda item: item["distance"])


def serializable_esp32_state():
    return {
        "lat": esp32_state["lat"],
        "lon": esp32_state["lon"],
        "home_lat": esp32_state["home_lat"],
        "home_lon": esp32_state["home_lon"],
        "postal_code": esp32_state["postal_code"],
        "place": esp32_state["place"],
        "range_km": esp32_state["range_km"],
        "mode": esp32_state["mode"],
        "selected_id": esp32_state["selected_id"],
        "favorites": sorted(esp32_state["favorites"]),
    }


def handle_esp32_tap(x, y):
    if esp32_state["selected_id"]:
        if 156 <= x <= 188 and 74 <= y <= 108:
            esp32_state["favorites"].add(esp32_state["selected_id"])
            return
        if 190 <= x <= 224 and 74 <= y <= 108:
            esp32_state["selected_id"] = None
            return

    if 88 <= x <= 152 and 194 <= y <= 226:
        esp32_state["mode"] = "menu" if esp32_state["mode"] != "menu" else "radar"
        esp32_state["selected_id"] = None
        return

    if esp32_state["mode"] == "menu":
        menu_targets = [
            ("radar", 72, 60, 168, 92),
            ("settings", 28, 105, 112, 139),
            ("map", 128, 105, 212, 139),
            ("recenter", 28, 150, 112, 184),
            ("assistant", 128, 150, 212, 184),
        ]
        for mode, x1, y1, x2, y2 in menu_targets:
            if x1 <= x <= x2 and y1 <= y <= y2:
                if mode == "recenter":
                    esp32_state["lat"] = esp32_state["home_lat"]
                    esp32_state["lon"] = esp32_state["home_lon"]
                    esp32_state["mode"] = "radar"
                else:
                    esp32_state["mode"] = mode
                esp32_state["selected_id"] = None
                return

    if esp32_state["mode"] == "map":
        if 86 <= x <= 154 and 80 <= y <= 112:
            pan_esp32_map(0, -1)
            return
        if 86 <= x <= 154 and 156 <= y <= 188:
            pan_esp32_map(0, 1)
            return
        if 26 <= x <= 94 and 118 <= y <= 150:
            pan_esp32_map(-1, 0)
            return
        if 146 <= x <= 214 and 118 <= y <= 150:
            pan_esp32_map(1, 0)
            return
        if 86 <= x <= 154 and 118 <= y <= 150:
            esp32_state["lat"] = esp32_state["home_lat"]
            esp32_state["lon"] = esp32_state["home_lon"]
            esp32_state["selected_id"] = None
            return
        if 88 <= x <= 152 and 194 <= y <= 226:
            esp32_state["mode"] = "menu"
            return

    if esp32_state["mode"] == "settings":
        if 178 <= x <= 212 and 50 <= y <= 86:
            esp32_state["mode"] = "radar"
            esp32_state["selected_id"] = None
            return
        if 37 <= x <= 124 and 147 <= y <= 176:
            esp32_state["postal_draft"] = esp32_state["postal_code"]
            esp32_state["mode"] = "postal"
            esp32_state["selected_id"] = None
            return
        ranges = [(20, 48, 121), (50, 88, 121), (100, 128, 121), (250, 168, 121)]
        for value, cx, cy in ranges:
            if abs(x - cx) <= 22 and abs(y - cy) <= 20:
                esp32_state["range_km"] = float(value)
                return

    if esp32_state["mode"] == "postal":
        if 178 <= x <= 212 and 50 <= y <= 86:
            esp32_state["mode"] = "settings"
            return
        handle_postal_keypad_tap(x, y)
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


def pan_esp32_map(dx, dy):
    lat_step = esp32_state["range_km"] * 0.35 / 111.0
    lon_step = esp32_state["range_km"] * 0.35 / max(10.0, 111.0 * cos(radians(esp32_state["lat"])))
    esp32_state["lat"] = max(-85.0, min(85.0, esp32_state["lat"] + dy * lat_step))
    esp32_state["lon"] = max(-180.0, min(180.0, esp32_state["lon"] + dx * lon_step))
    esp32_state["selected_id"] = None


def handle_postal_keypad_tap(x, y):
    keys = [
        ("1", 58, 92), ("2", 100, 92), ("3", 142, 92),
        ("4", 58, 120), ("5", 100, 120), ("6", 142, 120),
        ("7", 58, 148), ("8", 100, 148), ("9", 142, 148),
        ("CLR", 58, 176), ("0", 100, 176), ("OK", 158, 176),
    ]
    for key, cx, cy in keys:
        width = 36 if key not in {"CLR", "OK"} else 50
        if abs(x - cx) <= width / 2 and abs(y - cy) <= 14:
            if key == "CLR":
                esp32_state["postal_draft"] = ""
            elif key == "OK":
                apply_postal_draft()
            elif len(esp32_state["postal_draft"]) < 6:
                esp32_state["postal_draft"] += key
            return


def apply_postal_draft():
    postal_code = re.sub(r"[^0-9]", "", esp32_state["postal_draft"])
    if len(postal_code) < 4:
        return
    try:
        location = geocode_postal_code(postal_code)
    except (HTTPError, URLError, TimeoutError, ValueError, json.JSONDecodeError):
        location = None
    if location:
        esp32_state["postal_code"] = postal_code
        esp32_state["postal_draft"] = postal_code
        esp32_state["lat"] = location["lat"]
        esp32_state["lon"] = location["lon"]
        esp32_state["home_lat"] = location["lat"]
        esp32_state["home_lon"] = location["lon"]
        esp32_state["place"] = location["place"]
        esp32_state["selected_id"] = None
        esp32_state["mode"] = "radar"


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
        "User-Agent": USER_AGENT,
    })
    with urlopen(request, timeout=5) as response:
        body = response.read()
        tile_cache[key] = {"created_at": now, "body": body}
        return body


def fetch_photo_thumb(url, width=72, height=52):
    if not url:
        return None
    now = time()
    key = (url, width, height)
    cached = photo_cache.get(key)
    if cached and now - cached["created_at"] < AIRCRAFT_CACHE_SECONDS:
        return cached["image"]
    try:
        request = Request(url, headers={"User-Agent": USER_AGENT})
        with urlopen(request, timeout=3) as response:
            source = Image.open(BytesIO(response.read())).convert("RGB")
    except (HTTPError, URLError, TimeoutError, OSError):
        return None
    source_ratio = source.width / max(1, source.height)
    target_ratio = width / height
    if source_ratio > target_ratio:
        crop_width = int(source.height * target_ratio)
        left = max(0, (source.width - crop_width) // 2)
        source = source.crop((left, 0, left + crop_width, source.height))
    else:
        crop_height = int(source.width / target_ratio)
        top = max(0, (source.height - crop_height) // 2)
        source = source.crop((0, top, source.width, top + crop_height))
    source = source.resize((width, height), Image.Resampling.LANCZOS)
    source = ImageEnhance.Brightness(source).enhance(1.75)
    source = ImageEnhance.Contrast(source).enhance(1.55)
    source = ImageEnhance.Color(source).enhance(1.25)
    thumb = Image.new("RGBA", (width, height), (2, 8, 4, 255))
    thumb.alpha_composite(source.convert("RGBA"), (0, 0))
    mask = Image.new("L", (width, height), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, width - 1, height - 1), radius=7, fill=255)
    framed = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    framed.alpha_composite(thumb)
    framed.putalpha(mask)
    photo_cache[key] = {"created_at": now, "image": framed}
    return framed


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
        gray.point(lambda value: int(value * 0.04)),
        gray.point(lambda value: int(34 + value * 0.55)),
        gray.point(lambda value: int(4 + value * 0.08)),
    ))
    return green.filter(ImageFilter.GaussianBlur(0.25))


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
        ImageDraw.Draw(mask).ellipse((4, 4, 236, 236), fill=185)
        image.alpha_composite(Image.composite(map_layer.convert("RGBA"), Image.new("RGBA", (size, size)), mask))
    except Exception:
        pass

    draw.ellipse((3, 3, 237, 237), outline=(57, 74, 58, 220), width=5)
    draw.ellipse((12, 12, 228, 228), fill=(0, 0, 0, 46), outline=(12, 30, 18, 240), width=4)
    draw.ellipse((24, 24, 216, 216), fill=(5, 18, 7, 46), outline=(44, 129, 57, 92), width=1)
    for index in range(1, 5):
        r = radius * index / 4
        draw.ellipse((center - r, center - r, center + r, center + r), outline=(62, 210, 82, 52), width=1)
    for angle in range(0, 360, 30):
        a = radians(angle)
        draw.line((center, center, center + cos(a) * radius, center + sin(a) * radius), fill=(76, 194, 83, 48), width=1)

    font_tiny = load_font(8)
    font_small = load_font(10)
    font_big = load_font(32)
    font_mid = load_font(14)
    font_title = load_font(22)

    draw.text((center, 26), "18:47", fill=(232, 244, 234, 205), font=font_mid, anchor="mm")
    labels = {
        "AIRPLANES": "AIRPLANES.LIVE",
        "OPENSKY": "LIVE OPENSKY",
        "CACHE": "CACHE LIVE",
        "OFFLINE": "OFFLINE",
    }
    draw.text((center, 43), labels.get(source, source), fill=(130, 235, 119, 190), font=font_tiny, anchor="mm")
    draw.text((center, 67), str(len(aircraft)), fill=green, font=font_big, anchor="mm")
    draw.text((center, 88), "AVIONS", fill=green, font=font_mid, anchor="mm")
    draw.text((176, 121), str(int(range_km * 0.4)), fill=(120, 210, 110, 165), font=font_small, anchor="lm")
    draw.text((207, 121), str(int(range_km)), fill=(120, 210, 110, 165), font=font_small, anchor="lm")
    draw.text((207, 135), "KM", fill=(120, 210, 110, 165), font=font_small, anchor="lm")

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
    elif state["mode"] == "menu":
        draw_menu_panel(draw, state, font_small, font_mid)
    elif state["mode"] == "map":
        draw_map_panel(draw, state, font_small, font_mid)
    elif state["mode"] == "postal":
        draw_postal_panel(draw, state, font_tiny, font_small, font_mid)
    elif state["mode"] != "radar":
        draw_mode_panel(draw, state["mode"], font_small, font_mid)

    selected = next((item for item in aircraft if item["id"] == state["selected_id"]), None)
    if selected:
        draw_aircraft_popup(glow, draw, selected, state, font_tiny, font_small, font_mid, font_title)
    else:
        if state["mode"] != "postal":
            draw_menu_button(draw, state["mode"], font_tiny)

    vignette = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    vdraw = ImageDraw.Draw(vignette)
    vdraw.ellipse((0, 0, size, size), outline=(0, 0, 0, 255), width=18)
    vdraw.rectangle((0, 0, size, 18), fill=(0, 0, 0, 150))
    image = Image.alpha_composite(image.convert("RGBA"), glow)
    image = Image.alpha_composite(image, vignette)

    buffer = BytesIO()
    image.convert("RGB").save(buffer, format="PNG", compress_level=1)
    return buffer.getvalue()


def draw_menu_button(draw, active_mode, font):
    active = active_mode == "menu"
    draw.rounded_rectangle((88, 195, 152, 222), radius=12, fill=(4, 12, 7, 218), outline=(120, 255, 120, 230 if active else 110), width=2 if active else 1)
    draw.text((120, 208), "MENU", fill=(141, 255, 111, 245), font=font, anchor="mm")


def draw_menu_panel(draw, state, font_small, font_mid):
    panel = (24, 52, 216, 188)
    draw.rounded_rectangle(panel, radius=18, fill=(2, 9, 5, 226), outline=(82, 224, 121, 150), width=2)
    draw.text((120, 75), "MENU", fill=(141, 255, 111, 240), font=font_mid, anchor="mm")
    items = [
        ("RADAR", 72, 60, 168, 92),
        ("RÉGLAGES", 28, 105, 112, 139),
        ("CARTE", 128, 105, 212, 139),
        ("CENTRER", 28, 150, 112, 184),
        ("IA", 128, 150, 212, 184),
    ]
    for label, x1, y1, x2, y2 in items:
        active = label == "RADAR" and state["mode"] == "radar"
        draw.rounded_rectangle((x1, y1, x2, y2), radius=12, fill=(82, 224, 121, 36 if active else 20), outline=(141, 255, 111, 170 if active else 95), width=1)
        draw.text(((x1 + x2) / 2, (y1 + y2) / 2), label, fill=(141, 255, 111, 240) if active else (235, 242, 236, 190), font=font_small, anchor="mm")


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
    draw_menu_button(draw, mode, font_small)


def draw_settings(draw, state, font_small, font_mid):
    draw.rounded_rectangle((24, 55, 216, 185), radius=16, fill=(3, 10, 6, 232), outline=(82, 224, 121, 130), width=2)
    draw.text((42, 78), "RÉGLAGES", fill=(141, 255, 111, 240), font=font_mid, anchor="lm")
    draw.rounded_rectangle((184, 58, 208, 82), radius=12, fill=(82, 224, 121, 28), outline=(141, 255, 111, 128), width=1)
    draw.text((196, 69), "×", fill=(141, 255, 111, 240), font=font_mid, anchor="mm")
    draw.text((42, 101), "Rayon", fill=(238, 244, 239, 160), font=font_small, anchor="lm")
    x = 34
    for value in (20, 50, 100, 250):
        active = int(state["range_km"]) == value
        draw.rounded_rectangle((x, 114, x + 35, 137), radius=8, fill=(82, 224, 121, 55 if active else 18), outline=(141, 255, 111, 180 if active else 80), width=1)
        draw.text((x + 17, 126), str(value), fill=(141, 255, 111, 240) if active else (238, 244, 239, 170), font=font_small, anchor="mm")
        x += 42
    draw.rounded_rectangle((37, 147, 124, 176), radius=10, fill=(82, 224, 121, 28), outline=(141, 255, 111, 110), width=1)
    draw.text((52, 161), "NPA", fill=(238, 244, 239, 170), font=font_small, anchor="lm")
    draw.text((116, 161), state["postal_code"], fill=(141, 255, 111, 235), font=font_small, anchor="rm")
    draw.text((136, 158), clipped(state["place"], 11), fill=(141, 255, 111, 190), font=font_small, anchor="lm")


def draw_map_panel(draw, state, font_small, font_mid):
    draw.rounded_rectangle((20, 47, 220, 189), radius=18, fill=(2, 9, 5, 224), outline=(82, 224, 121, 140), width=2)
    draw.text((120, 63), "CARTE", fill=(141, 255, 111, 240), font=font_mid, anchor="mm")
    draw.text((120, 78), clipped(state["place"], 18), fill=(238, 244, 239, 160), font=font_small, anchor="mm")
    controls = [
        ("↑", 86, 80, 154, 112),
        ("←", 26, 118, 94, 150),
        ("⌂", 86, 118, 154, 150),
        ("→", 146, 118, 214, 150),
        ("↓", 86, 156, 154, 188),
    ]
    for label, x1, y1, x2, y2 in controls:
        draw.rounded_rectangle((x1, y1, x2, y2), radius=13, fill=(82, 224, 121, 24), outline=(141, 255, 111, 105), width=1)
        draw.text(((x1 + x2) / 2, (y1 + y2) / 2), label, fill=(141, 255, 111, 230), font=font_mid, anchor="mm")
    draw_menu_button(draw, "map", font_small)


def draw_postal_panel(draw, state, font_tiny, font_small, font_mid):
    draw.rounded_rectangle((20, 43, 220, 214), radius=18, fill=(2, 9, 5, 236), outline=(82, 224, 121, 150), width=2)
    draw.text((48, 64), "NPA", fill=(141, 255, 111, 240), font=font_mid, anchor="lm")
    draw.rounded_rectangle((184, 50, 208, 74), radius=12, fill=(82, 224, 121, 28), outline=(141, 255, 111, 128), width=1)
    draw.text((196, 61), "×", fill=(141, 255, 111, 240), font=font_mid, anchor="mm")
    draw.rounded_rectangle((76, 52, 164, 76), radius=10, fill=(0, 0, 0, 145), outline=(141, 255, 111, 95), width=1)
    draw.text((120, 64), state["postal_draft"] or "----", fill=(238, 244, 239, 230), font=font_mid, anchor="mm")
    keys = [
        ("1", 58, 92), ("2", 100, 92), ("3", 142, 92),
        ("4", 58, 120), ("5", 100, 120), ("6", 142, 120),
        ("7", 58, 148), ("8", 100, 148), ("9", 142, 148),
        ("CLR", 58, 176), ("0", 100, 176), ("OK", 158, 176),
    ]
    for label, cx, cy in keys:
        width = 36 if label not in {"CLR", "OK"} else 50
        x1, x2 = cx - width / 2, cx + width / 2
        draw.rounded_rectangle((x1, cy - 14, x2, cy + 14), radius=9, fill=(82, 224, 121, 26), outline=(141, 255, 111, 105), width=1)
        draw.text((cx, cy), label, fill=(141, 255, 111, 230), font=font_tiny if label == "CLR" else font_small, anchor="mm")


def clipped(value, size):
    value = str(value or "INCONNU")
    if len(value) <= size:
        return value
    return value[:size - 1] + "…"


def draw_aircraft_popup(overlay, draw, aircraft, state, font_tiny, font_small, font_mid, font_title):
    details = lookup_aircraft_details_for_png(aircraft)
    panel = (20, 72, 220, 184)
    draw.rounded_rectangle(panel, radius=12, fill=(3, 11, 6, 238), outline=(82, 224, 121, 180), width=2)

    thumb_box = (128, 120, 210, 178)
    thumb = fetch_photo_thumb(details.get("photo"), 80, 56)
    if thumb:
        overlay.alpha_composite(thumb, (129, 121))
    else:
        draw.rounded_rectangle(thumb_box, radius=8, fill=(8, 20, 12, 190), outline=(82, 224, 121, 80), width=1)
        draw.text((169, 141), "PHOTO", fill=(141, 255, 111, 130), font=font_tiny, anchor="mm")
        draw.text((169, 154), "N/D", fill=(238, 244, 239, 120), font=font_tiny, anchor="mm")
    draw.rounded_rectangle((127, 119, 211, 179), radius=9, outline=(132, 255, 126, 155), width=2)

    draw.text((34, 91), "AVION SÉLECTIONNÉ", fill=(141, 255, 111, 220), font=font_tiny, anchor="lm")
    draw.text((34, 116), clipped(aircraft["callsign"], 8), fill=(112, 255, 113, 255), font=font_title, anchor="lm")
    subtitle = clipped(aircraft.get("aircraft_type") or details.get("type") or aircraft.get("country") or "Live", 16)
    draw.text((34, 136), subtitle, fill=(238, 244, 239, 188), font=font_small, anchor="lm")
    route = city_route(details)
    if route:
        draw.text((34, 152), clipped(route, 15), fill=(238, 244, 239, 168), font=font_tiny, anchor="lm")
    else:
        draw.text((34, 152), f"{round(aircraft['distance'])} km  {round(aircraft['speed'])} km/h", fill=(238, 244, 239, 178), font=font_tiny, anchor="lm")
    draw.text((34, 170), f"{round(aircraft['altitude'] or 0)} m  {round(aircraft['heading'] or 0)}°", fill=(238, 244, 239, 165), font=font_tiny, anchor="lm")
    draw.rounded_rectangle((158, 82, 184, 108), radius=13, fill=(82, 224, 121, 30), outline=(141, 255, 111, 140), width=1)
    draw.text((171, 94), "★", fill=(141, 255, 111, 230), font=font_small, anchor="mm")
    draw.rounded_rectangle((188, 82, 214, 108), radius=13, fill=(82, 224, 121, 30), outline=(141, 255, 111, 140), width=1)
    draw.text((201, 94), "×", fill=(141, 255, 111, 240), font=font_mid, anchor="mm")


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
        request = Request(PLANESPOTTERS_URL.format(hex_code=hex_code), headers={"Accept": "application/json", "User-Agent": USER_AGENT})
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

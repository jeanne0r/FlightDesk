#!/usr/bin/env python3
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from math import cos, radians
from time import time
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlencode, urlparse
from urllib.request import Request, urlopen
import json
import re


OPEN_SKY_URL = "https://opensky-network.org/api/states/all"
PLANESPOTTERS_URL = "https://api.planespotters.net/pub/photos/hex/{hex_code}"
OSM_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
TRAFFIC_CACHE_SECONDS = 12
AIRCRAFT_CACHE_SECONDS = 86400
TILE_CACHE_SECONDS = 86400
traffic_cache = {}
aircraft_cache = {}
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


class FlightDeskHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/traffic":
            self.handle_traffic(parsed)
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
        if not re.fullmatch(r"[0-9a-f]{6}", hex_code):
            self.write_json({"error": "invalid hex", "photo": None, "type": None}, status=400)
            return

        now = time()
        cached = aircraft_cache.get(hex_code)
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
                aircraft_cache[hex_code] = {"created_at": now, "payload": payload}
                self.write_json(payload)
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as error:
            self.write_json({"error": str(error), "photo": None, "type": None}, status=502)

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


if __name__ == "__main__":
    server = ThreadingHTTPServer(("0.0.0.0", 4173), FlightDeskHandler)
    print("FlightDesk simulator on http://0.0.0.0:4173")
    server.serve_forever()

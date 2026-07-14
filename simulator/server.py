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
OSM_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
TRAFFIC_CACHE_SECONDS = 12
TILE_CACHE_SECONDS = 86400
traffic_cache = {}
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


class FlightDeskHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/traffic":
            self.handle_traffic(parsed)
            return
        if parsed.path.startswith("/api/tile/"):
            self.handle_tile(parsed)
            return
        if parsed.path in ("/live", "/live.html"):
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

const canvas = document.getElementById("radar");
const ctx = canvas.getContext("2d");
const mapLayer = document.getElementById("map-layer");

const swissPostalCenters = {
  "1000": { label: "Lausanne", lat: 46.5197, lon: 6.6323 },
  "1010": { label: "Lausanne", lat: 46.5347, lon: 6.6613 },
  "1020": { label: "Renens", lat: 46.5333, lon: 6.5916 },
  "1110": { label: "Morges", lat: 46.5113, lon: 6.4985 },
  "1200": { label: "Genève", lat: 46.2044, lon: 6.1432 },
  "1260": { label: "Nyon", lat: 46.3833, lon: 6.2396 },
  "1400": { label: "Yverdon-les-Bains", lat: 46.7785, lon: 6.6412 },
  "1700": { label: "Fribourg", lat: 46.8065, lon: 7.1619 },
  "1800": { label: "Vevey", lat: 46.4628, lon: 6.8419 },
  "1820": { label: "Montreux", lat: 46.4312, lon: 6.9107 },
  "1950": { label: "Sion", lat: 46.2331, lon: 7.3606 },
  "2000": { label: "Neuchâtel", lat: 46.9918, lon: 6.9310 },
  "2500": { label: "Bienne", lat: 47.1368, lon: 7.2468 },
  "3000": { label: "Berne", lat: 46.9480, lon: 7.4474 },
  "4000": { label: "Bâle", lat: 47.5596, lon: 7.5886 },
  "5000": { label: "Aarau", lat: 47.3925, lon: 8.0442 },
  "6000": { label: "Lucerne", lat: 47.0502, lon: 8.3093 },
  "6900": { label: "Lugano", lat: 46.0037, lon: 8.9511 },
  "7000": { label: "Coire", lat: 46.8508, lon: 9.5320 },
  "8000": { label: "Zurich", lat: 47.3769, lon: 8.5417 },
  "9000": { label: "Saint-Gall", lat: 47.4245, lon: 9.3767 }
};

const state = {
  rangeKm: 50,
  brightness: 82,
  volume: 55,
  night: true,
  muted: false,
  postalCode: localStorage.getItem("flightdesk:postalCode") || "1000",
  mapCenter: null,
  mapZoom: 11,
  trafficSource: "simulated",
  trafficStatus: "Connexion trafic en attente…",
  liveAircraft: [],
  lastTrafficFetchMs: 0,
  mode: "radar",
  sweepDeg: 0,
  selectedId: null,
  favorites: new Set(JSON.parse(localStorage.getItem("flightdesk:favorites") || "[]")),
  aircraft: []
};

const airlines = {
  LX: "Swiss",
  EZY: "easyJet",
  AFR: "Air France",
  KLM: "KLM",
  BAW: "British Airways",
  DLH: "Lufthansa",
  TAP: "TAP Air Portugal",
  UAE: "Emirates"
};

const demoFlights = [
  { id: "4b181a", callsign: "LX281", airline: "Swiss", distance: 11, bearing: 56, altitude: 11250, speed: 812, heading: 216 },
  { id: "406b31", callsign: "EZY61Q", airline: "easyJet", distance: 18, bearing: 312, altitude: 9300, speed: 744, heading: 154 },
  { id: "39c742", callsign: "AFR123", airline: "Air France", distance: 28, bearing: 128, altitude: 10400, speed: 790, heading: 284 },
  { id: "48520b", callsign: "KLM923", airline: "KLM", distance: 42, bearing: 224, altitude: 8600, speed: 705, heading: 42 },
  { id: "4009f8", callsign: "BAW74", airline: "British Airways", distance: 61, bearing: 22, altitude: 11900, speed: 834, heading: 238 },
  { id: "3c65a8", callsign: "DLH5TR", airline: "Lufthansa", distance: 84, bearing: 170, altitude: 10100, speed: 768, heading: 310 },
  { id: "4952ab", callsign: "TAP934", airline: "TAP Air Portugal", distance: 118, bearing: 88, altitude: 9700, speed: 731, heading: 254 },
  { id: "89644a", callsign: "UAE21", airline: "Emirates", distance: 176, bearing: 270, altitude: 12100, speed: 882, heading: 68 }
];

const trafficPollMs = 18000;

const elements = {
  count: document.getElementById("aircraft-count"),
  rangeLabel: document.getElementById("range-label"),
  modeLabel: document.getElementById("mode-label"),
  postalLabel: document.getElementById("postal-label"),
  selectedEmpty: document.getElementById("selected-empty"),
  selectedFlight: document.getElementById("selected-flight"),
  selectedCallsign: document.getElementById("selected-callsign"),
  selectedAirline: document.getElementById("selected-airline"),
  selectedDistance: document.getElementById("selected-distance"),
  selectedAltitude: document.getElementById("selected-altitude"),
  selectedSpeed: document.getElementById("selected-speed"),
  selectedHeading: document.getElementById("selected-heading"),
  favoriteToggle: document.getElementById("favorite-toggle"),
  assistantAnswer: document.getElementById("assistant-answer"),
  mapStatus: document.getElementById("map-status")
};

document.getElementById("postal-control").value = state.postalCode;
setPostalCode(state.postalCode);

function saveFavorites() {
  localStorage.setItem("flightdesk:favorites", JSON.stringify([...state.favorites]));
}

function updateAircraft(timeSeconds) {
  if (state.trafficSource === "live" && state.liveAircraft.length) {
    state.aircraft = state.liveAircraft.map((aircraft) => ({
      ...aircraft,
      visible: aircraft.distance <= state.rangeKm,
      favorite: state.favorites.has(aircraft.id)
    }));
    if (state.selectedId && !state.aircraft.some((aircraft) => aircraft.id === state.selectedId && aircraft.visible)) {
      state.selectedId = null;
    }
    return;
  }

  state.aircraft = demoFlights.map((flight, index) => {
    const wobble = Math.sin(timeSeconds * (0.16 + index * 0.025) + index) * 4;
    const bearing = (flight.bearing + timeSeconds * (1.8 + index * 0.22) + wobble + 360) % 360;
    const distance = Math.max(4, flight.distance + Math.sin(timeSeconds * 0.12 + index * 1.7) * 3);
    return {
      ...flight,
      bearing,
      distance,
      visible: distance <= state.rangeKm,
      favorite: state.favorites.has(flight.id)
    };
  });

  if (state.selectedId && !state.aircraft.some((aircraft) => aircraft.id === state.selectedId && aircraft.visible)) {
    state.selectedId = null;
  }
}

function haversineKm(lat1, lon1, lat2, lon2) {
  const earthKm = 6371;
  const p1 = (lat1 * Math.PI) / 180;
  const p2 = (lat2 * Math.PI) / 180;
  const dp = ((lat2 - lat1) * Math.PI) / 180;
  const dl = ((lon2 - lon1) * Math.PI) / 180;
  const a = Math.sin(dp / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  return earthKm * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function initialBearingDeg(lat1, lon1, lat2, lon2) {
  const p1 = (lat1 * Math.PI) / 180;
  const p2 = (lat2 * Math.PI) / 180;
  const dl = ((lon2 - lon1) * Math.PI) / 180;
  const y = Math.sin(dl) * Math.cos(p2);
  const x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
  return ((Math.atan2(y, x) * 180) / Math.PI + 360) % 360;
}

function normalizeOpenSkyState(row) {
  if (!state.mapCenter || !Array.isArray(row)) return null;
  const lon = row[5];
  const lat = row[6];
  if (typeof lat !== "number" || typeof lon !== "number") return null;

  const distance = haversineKm(state.mapCenter.lat, state.mapCenter.lon, lat, lon);
  const bearing = initialBearingDeg(state.mapCenter.lat, state.mapCenter.lon, lat, lon);
  const callsign = String(row[1] || row[0] || "UNKNOWN").trim() || String(row[0]).toUpperCase();
  const velocityMs = typeof row[9] === "number" ? row[9] : 0;
  const altitude = typeof row[13] === "number" ? row[13] : (typeof row[7] === "number" ? row[7] : 0);
  const heading = typeof row[10] === "number" ? row[10] : bearing;

  return {
    id: String(row[0] || callsign),
    callsign,
    airline: row[2] || "OpenSky",
    latitude: lat,
    longitude: lon,
    distance,
    bearing,
    altitude,
    speed: velocityMs * 3.6,
    heading,
    onGround: Boolean(row[8])
  };
}

async function fetchLiveTraffic(force = false) {
  if (!state.mapCenter) return;
  const now = Date.now();
  if (!force && now - state.lastTrafficFetchMs < trafficPollMs) return;
  state.lastTrafficFetchMs = now;

  const params = new URLSearchParams({
    lat: String(state.mapCenter.lat),
    lon: String(state.mapCenter.lon),
    range: String(state.rangeKm)
  });

  try {
    state.trafficStatus = "Connexion OpenSky…";
    const response = await fetch(`/api/traffic?${params.toString()}`, { cache: "no-store" });
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`);
    const aircraft = (payload.states || []).map(normalizeOpenSkyState).filter(Boolean);
    state.liveAircraft = aircraft
      .filter((item) => item.distance <= state.rangeKm && !item.onGround)
      .sort((a, b) => a.distance - b.distance)
      .slice(0, 48);
    state.trafficSource = "live";
    state.trafficStatus = `${state.liveAircraft.length} avion(s) live OpenSky`;
  } catch (error) {
    state.trafficSource = "simulated";
    state.liveAircraft = [];
    state.trafficStatus = "OpenSky indisponible, trafic simulé";
  }
}

function polarToCanvas(bearingDeg, distanceKm) {
  const size = canvas.width;
  const center = size / 2;
  const radius = size * 0.405;
  const angle = ((bearingDeg - 90) * Math.PI) / 180;
  const r = radius * (distanceKm / state.rangeKm);
  return {
    x: center + Math.cos(angle) * r,
    y: center + Math.sin(angle) * r
  };
}

function drawRadar() {
  const size = canvas.width;
  const center = size / 2;
  const radius = size * 0.405;
  const intensity = state.brightness / 100;
  const glow = state.night ? 1 : 0.72;

  ctx.clearRect(0, 0, size, size);
  ctx.fillStyle = "rgba(2, 5, 3, 0.28)";
  ctx.fillRect(0, 0, size, size);

  const bg = ctx.createRadialGradient(center, center, 0, center, center, radius * 1.15);
  bg.addColorStop(0, `rgba(24, 112, 43, ${0.05 * intensity})`);
  bg.addColorStop(0.7, "rgba(3, 16, 8, 0.34)");
  bg.addColorStop(1, "rgba(2, 5, 3, 0.62)");
  ctx.fillStyle = bg;
  ctx.beginPath();
  ctx.arc(center, center, radius * 1.05, 0, Math.PI * 2);
  ctx.fill();

  ctx.save();
  ctx.translate(center, center);
  ctx.strokeStyle = `rgba(82, 224, 121, ${0.17 * glow})`;
  ctx.lineWidth = 1.4;
  for (let i = 1; i <= 4; i += 1) {
    ctx.beginPath();
    ctx.arc(0, 0, (radius / 4) * i, 0, Math.PI * 2);
    ctx.stroke();
  }
  for (let i = 0; i < 12; i += 1) {
    const a = (i * Math.PI) / 6;
    ctx.beginPath();
    ctx.moveTo(Math.cos(a) * radius * 0.08, Math.sin(a) * radius * 0.08);
    ctx.lineTo(Math.cos(a) * radius, Math.sin(a) * radius);
    ctx.stroke();
  }
  ctx.restore();

  drawSweep(center, radius, intensity);
  drawRangeLabels(center, radius);
  drawHome(center);
  drawAircraft();
  drawOverlayText(center);
}

function drawSweep(center, radius, intensity) {
  const start = ((state.sweepDeg - 90) * Math.PI) / 180;
  const spread = Math.PI / 5.2;
  const gradient = ctx.createRadialGradient(center, center, 0, center, center, radius);
  gradient.addColorStop(0, `rgba(82, 224, 121, ${0.52 * intensity})`);
  gradient.addColorStop(0.72, `rgba(82, 224, 121, ${0.28 * intensity})`);
  gradient.addColorStop(1, "rgba(82, 224, 121, 0)");

  ctx.fillStyle = gradient;
  ctx.beginPath();
  ctx.moveTo(center, center);
  ctx.arc(center, center, radius, start - spread, start, false);
  ctx.closePath();
  ctx.fill();

  ctx.strokeStyle = `rgba(141, 255, 111, ${0.78 * intensity})`;
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(center, center);
  ctx.lineTo(center + Math.cos(start) * radius, center + Math.sin(start) * radius);
  ctx.stroke();
}

function drawRangeLabels(center, radius) {
  ctx.fillStyle = "rgba(141, 255, 111, 0.78)";
  ctx.font = "18px ui-sans-serif, system-ui";
  ctx.textAlign = "left";
  ctx.fillText(String(Math.round(state.rangeKm * 0.4)), center + radius * 0.42, center + radius * 0.04);
  ctx.fillText(String(Math.round(state.rangeKm)), center + radius * 0.72, center + radius * 0.04);
  ctx.fillText("KM", center + radius * 0.75, center + radius * 0.12);
}

function drawHome(center) {
  ctx.strokeStyle = "rgba(82, 224, 121, 0.9)";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.arc(center, center, 14, 0, Math.PI * 2);
  ctx.stroke();
  ctx.fillStyle = "rgba(82, 224, 121, 0.9)";
  ctx.font = "20px ui-sans-serif, system-ui";
  ctx.textAlign = "center";
  ctx.fillText("⌂", center, center + 7);
}

function drawAircraft() {
  const visibleAircraft = state.aircraft.filter((aircraft) => aircraft.visible);
  const selected = state.selectedId;

  visibleAircraft.forEach((aircraft) => {
    const point = polarToCanvas(aircraft.bearing, aircraft.distance);
    const isSelected = aircraft.id === selected;
    const color = aircraft.favorite ? "#f0c95a" : "#8dff6f";

    ctx.save();
    ctx.translate(point.x, point.y);
    ctx.rotate(((aircraft.heading - 90) * Math.PI) / 180);
    ctx.strokeStyle = color;
    ctx.fillStyle = color;
    ctx.lineWidth = isSelected ? 3 : 2;
    ctx.shadowColor = color;
    ctx.shadowBlur = isSelected ? 18 : 8;
    ctx.beginPath();
    ctx.moveTo(12, 0);
    ctx.lineTo(-9, -7);
    ctx.lineTo(-5, 0);
    ctx.lineTo(-9, 7);
    ctx.closePath();
    ctx.stroke();
    if (isSelected) ctx.fill();
    ctx.restore();

    if (isSelected) {
      ctx.fillStyle = "rgba(8, 14, 10, 0.78)";
      ctx.strokeStyle = "rgba(82, 224, 121, 0.35)";
      const labelX = Math.min(point.x + 18, canvas.width - 148);
      const labelY = Math.max(point.y - 36, 62);
      roundRect(labelX, labelY, 128, 54, 8);
      ctx.fill();
      ctx.stroke();
      ctx.fillStyle = "#8dff6f";
      ctx.font = "18px ui-sans-serif, system-ui";
      ctx.textAlign = "left";
      ctx.fillText(aircraft.callsign, labelX + 10, labelY + 22);
      ctx.fillStyle = "rgba(238, 244, 239, 0.82)";
      ctx.font = "14px ui-sans-serif, system-ui";
      ctx.fillText(`${Math.round(aircraft.distance)} km · ${Math.round(aircraft.speed)} km/h`, labelX + 10, labelY + 42);
    }
  });
}

function drawOverlayText(center) {
  const visible = state.aircraft.filter((aircraft) => aircraft.visible);
  ctx.textAlign = "center";
  ctx.fillStyle = "rgba(238, 244, 239, 0.92)";
  ctx.font = "18px ui-sans-serif, system-ui";
  ctx.fillText("18:47", center, 120);
  ctx.fillStyle = "rgba(141, 255, 111, 0.62)";
  ctx.font = "700 13px ui-sans-serif, system-ui";
  ctx.fillText(state.trafficSource === "live" ? "LIVE OPENSKY" : "TRAFIC SIMULÉ", center, 142);
  ctx.fillStyle = "#52e079";
  ctx.font = "700 44px ui-sans-serif, system-ui";
  ctx.fillText(String(visible.length), center, 184);
  ctx.font = "700 18px ui-sans-serif, system-ui";
  ctx.fillText("AVIONS", center, 210);

  if (state.mode !== "radar") {
    ctx.fillStyle = "rgba(3, 9, 5, 0.72)";
    roundRect(center - 128, center - 38, 256, 76, 14);
    ctx.fill();
    ctx.strokeStyle = "rgba(82, 224, 121, 0.28)";
    ctx.stroke();
    ctx.fillStyle = "#8dff6f";
    ctx.font = "700 20px ui-sans-serif, system-ui";
    ctx.fillText(modeTitle(state.mode), center, center - 6);
    ctx.fillStyle = "rgba(238, 244, 239, 0.72)";
    ctx.font = "14px ui-sans-serif, system-ui";
    ctx.fillText(modeSubtitle(state.mode), center, center + 20);
  }
}

function roundRect(x, y, width, height, radius) {
  ctx.beginPath();
  ctx.moveTo(x + radius, y);
  ctx.arcTo(x + width, y, x + width, y + height, radius);
  ctx.arcTo(x + width, y + height, x, y + height, radius);
  ctx.arcTo(x, y + height, x, y, radius);
  ctx.arcTo(x, y, x + width, y, radius);
  ctx.closePath();
}

function modeTitle(mode) {
  return {
    search: "Recherche",
    favorites: "Favoris",
    settings: "Réglages",
    assistant: "Assistant"
  }[mode] || "Radar";
}

function modeSubtitle(mode) {
  return {
    search: "Entrez un indicatif",
    favorites: `${state.favorites.size} vol(s) suivi(s)`,
    settings: "Rayon, volume, luminosité",
    assistant: "Question vocale simulée"
  }[mode] || "Temps réel simulé";
}

function updatePanel() {
  const visible = state.aircraft.filter((aircraft) => aircraft.visible);
  const selected = state.aircraft.find((aircraft) => aircraft.id === state.selectedId);
  elements.count.textContent = visible.length;
  elements.rangeLabel.textContent = state.rangeKm;
  elements.modeLabel.textContent = modeTitle(state.mode);
  elements.postalLabel.textContent = state.postalCode || "—";
  document.querySelector(".panel-header p:last-child").textContent =
    state.trafficSource === "live"
      ? `Données trafic live via OpenSky. ${state.trafficStatus}.`
      : `Données trafic simulées. ${state.trafficStatus}.`;

  if (!selected) {
    elements.selectedEmpty.classList.remove("hidden");
    elements.selectedFlight.classList.add("hidden");
    return;
  }

  elements.selectedEmpty.classList.add("hidden");
  elements.selectedFlight.classList.remove("hidden");
  elements.selectedCallsign.textContent = selected.callsign;
  elements.selectedAirline.textContent = selected.airline || airlineFromCallsign(selected.callsign);
  elements.selectedDistance.textContent = Math.round(selected.distance);
  elements.selectedAltitude.textContent = Math.round(selected.altitude);
  elements.selectedSpeed.textContent = Math.round(selected.speed);
  elements.selectedHeading.textContent = Math.round(selected.heading);
  elements.favoriteToggle.textContent = state.favorites.has(selected.id) ? "★" : "☆";
}

function airlineFromCallsign(callsign) {
  const prefix = callsign.replace(/[0-9].*$/, "");
  return airlines[prefix] || "Compagnie inconnue";
}

function animate(timestamp) {
  const seconds = timestamp / 1000;
  state.sweepDeg = (state.sweepDeg + 1.7) % 360;
  fetchLiveTraffic(false);
  updateAircraft(seconds);
  drawRadar();
  updatePanel();
  requestAnimationFrame(animate);
}

canvas.addEventListener("click", (event) => {
  const rect = canvas.getBoundingClientRect();
  const scale = canvas.width / rect.width;
  const x = (event.clientX - rect.left) * scale;
  const y = (event.clientY - rect.top) * scale;
  let nearest = null;
  let nearestDistance = 34;

  state.aircraft.filter((aircraft) => aircraft.visible).forEach((aircraft) => {
    const point = polarToCanvas(aircraft.bearing, aircraft.distance);
    const distance = Math.hypot(point.x - x, point.y - y);
    if (distance < nearestDistance) {
      nearest = aircraft;
      nearestDistance = distance;
    }
  });

  if (nearest) state.selectedId = nearest.id;
});

document.getElementById("range-control").addEventListener("change", (event) => {
  state.rangeKm = Number(event.target.value);
  fetchLiveTraffic(true);
});

let postalInputTimer = null;
document.getElementById("postal-control").addEventListener("input", (event) => {
  const nextCode = event.target.value.trim() || "1000";
  state.postalCode = nextCode;
  elements.postalLabel.textContent = nextCode;
  elements.mapStatus.textContent = `Carte en attente pour ${nextCode}…`;
  window.clearTimeout(postalInputTimer);
  postalInputTimer = window.setTimeout(() => setPostalCode(nextCode), 550);
});

document.getElementById("brightness-control").addEventListener("input", (event) => {
  state.brightness = Number(event.target.value);
});

document.getElementById("volume-control").addEventListener("input", (event) => {
  state.volume = Number(event.target.value);
});

document.getElementById("night-control").addEventListener("change", (event) => {
  state.night = event.target.checked;
});

document.getElementById("mute-control").addEventListener("change", (event) => {
  state.muted = event.target.checked;
});

document.querySelectorAll(".nav-button").forEach((button) => {
  button.addEventListener("click", () => {
    document.querySelectorAll(".nav-button").forEach((item) => item.classList.remove("active"));
    button.classList.add("active");
    state.mode = button.dataset.mode;
  });
});

elements.favoriteToggle.addEventListener("click", () => {
  if (!state.selectedId) return;
  if (state.favorites.has(state.selectedId)) {
    state.favorites.delete(state.selectedId);
  } else {
    state.favorites.add(state.selectedId);
  }
  saveFavorites();
});

document.getElementById("ask-button").addEventListener("click", () => {
  const visible = state.aircraft.filter((aircraft) => aircraft.visible);
  if (!visible.length) {
    elements.assistantAnswer.textContent = "Aucun avion visible dans le rayon actuel.";
    return;
  }
  const nearest = [...visible].sort((a, b) => a.distance - b.distance)[0];
  elements.assistantAnswer.textContent =
    `Le vol le plus proche est ${nearest.callsign} (${nearest.airline}), ` +
    `à ${Math.round(nearest.distance)} km, altitude ${Math.round(nearest.altitude)} m, ` +
    `vitesse ${Math.round(nearest.speed)} km/h.`;
  state.selectedId = nearest.id;
  state.mode = "assistant";
  document.querySelectorAll(".nav-button").forEach((item) => {
    item.classList.toggle("active", item.dataset.mode === "assistant");
  });
});

requestAnimationFrame(animate);

function lonLatToPixel(lat, lon, zoom) {
  const sinLat = Math.sin((lat * Math.PI) / 180);
  const scale = 256 * 2 ** zoom;
  return {
    x: ((lon + 180) / 360) * scale,
    y: (0.5 - Math.log((1 + sinLat) / (1 - sinLat)) / (4 * Math.PI)) * scale
  };
}

function renderMapTiles() {
  if (!state.mapCenter) return;

  const width = mapLayer.clientWidth || 560;
  const height = mapLayer.clientHeight || width;
  const zoom = state.mapZoom;
  const centerPx = lonLatToPixel(state.mapCenter.lat, state.mapCenter.lon, zoom);
  const topLeft = {
    x: centerPx.x - width / 2,
    y: centerPx.y - height / 2
  };
  const minTileX = Math.floor(topLeft.x / 256);
  const minTileY = Math.floor(topLeft.y / 256);
  const maxTileX = Math.floor((topLeft.x + width) / 256);
  const maxTileY = Math.floor((topLeft.y + height) / 256);
  const tileCount = 2 ** zoom;
  const fragment = document.createDocumentFragment();

  mapLayer.replaceChildren();

  for (let tileY = minTileY; tileY <= maxTileY; tileY += 1) {
    if (tileY < 0 || tileY >= tileCount) continue;
    for (let tileX = minTileX; tileX <= maxTileX; tileX += 1) {
      const wrappedX = ((tileX % tileCount) + tileCount) % tileCount;
      const img = document.createElement("img");
      img.alt = "";
      img.decoding = "async";
      img.loading = "lazy";
      img.src = `https://tile.openstreetmap.org/${zoom}/${wrappedX}/${tileY}.png`;
      img.style.left = `${Math.round(tileX * 256 - topLeft.x)}px`;
      img.style.top = `${Math.round(tileY * 256 - topLeft.y)}px`;
      fragment.appendChild(img);
    }
  }

  mapLayer.appendChild(fragment);
}

async function geocodePostalCode(postalCode) {
  const known = swissPostalCenters[postalCode];
  if (known) return known;

  const url = new URL("https://nominatim.openstreetmap.org/search");
  url.searchParams.set("format", "jsonv2");
  url.searchParams.set("countrycodes", "ch");
  url.searchParams.set("postalcode", postalCode);
  url.searchParams.set("limit", "1");

  const response = await fetch(url.toString(), { headers: { Accept: "application/json" } });
  if (!response.ok) throw new Error("Geocoding failed");
  const results = await response.json();
  if (!results.length) return null;

  return {
    label: results[0].display_name.split(",").slice(0, 2).join(","),
    lat: Number(results[0].lat),
    lon: Number(results[0].lon)
  };
}

let postalLookupId = 0;
async function setPostalCode(postalCode) {
  const cleanCode = postalCode.replace(/[^\dA-Za-z -]/g, "").slice(0, 10) || "1000";
  const lookupId = ++postalLookupId;
  state.postalCode = cleanCode;
  localStorage.setItem("flightdesk:postalCode", cleanCode);
  elements.postalLabel.textContent = cleanCode;
  elements.mapStatus.textContent = `Recherche de la carte pour ${cleanCode}…`;

  try {
    const center = await geocodePostalCode(cleanCode);
    if (lookupId !== postalLookupId) return;
    if (!center) throw new Error("Postal code not found");
    state.mapCenter = center;
    elements.mapStatus.textContent = `Carte réelle centrée sur ${cleanCode} ${center.label}.`;
  } catch (error) {
    if (lookupId !== postalLookupId) return;
    state.mapCenter = swissPostalCenters["1000"];
    elements.mapStatus.textContent = `Code postal non trouvé, carte centrée sur 1000 Lausanne.`;
  }

  renderMapTiles();
  fetchLiveTraffic(true);
}

window.addEventListener("resize", renderMapTiles);

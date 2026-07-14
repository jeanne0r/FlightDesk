const canvas = document.getElementById("radar");
const ctx = canvas.getContext("2d");

const state = {
  rangeKm: 50,
  brightness: 82,
  volume: 55,
  night: true,
  muted: false,
  postalCode: localStorage.getItem("flightdesk:postalCode") || "1000",
  mode: "radar",
  sweepDeg: 0,
  selectedId: null,
  favorites: new Set(JSON.parse(localStorage.getItem("flightdesk:favorites") || "[]")),
  aircraft: [],
  mapPaths: []
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
  assistantAnswer: document.getElementById("assistant-answer")
};

document.getElementById("postal-control").value = state.postalCode;
state.mapPaths = buildMapPaths(state.postalCode);

function saveFavorites() {
  localStorage.setItem("flightdesk:favorites", JSON.stringify([...state.favorites]));
}

function updateAircraft(timeSeconds) {
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
  ctx.fillStyle = "#020503";
  ctx.fillRect(0, 0, size, size);

  const bg = ctx.createRadialGradient(center, center, 0, center, center, radius * 1.15);
  bg.addColorStop(0, `rgba(24, 112, 43, ${0.12 * intensity})`);
  bg.addColorStop(0.7, "rgba(3, 16, 8, 0.92)");
  bg.addColorStop(1, "#020503");
  ctx.fillStyle = bg;
  ctx.beginPath();
  ctx.arc(center, center, radius * 1.05, 0, Math.PI * 2);
  ctx.fill();

  drawMapWatermark(center, radius, intensity);

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

function postalSeed(postalCode) {
  return String(postalCode || "1000").split("").reduce((seed, char) => {
    return (seed * 31 + char.charCodeAt(0)) >>> 0;
  }, 17);
}

function seededRandom(seed) {
  let value = seed >>> 0;
  return () => {
    value = (value * 1664525 + 1013904223) >>> 0;
    return value / 4294967296;
  };
}

function buildMapPaths(postalCode) {
  const random = seededRandom(postalSeed(postalCode));
  const paths = [];
  const roadCount = 9;

  for (let i = 0; i < roadCount; i += 1) {
    const vertical = random() > 0.45;
    const base = -0.86 + random() * 1.72;
    const drift = -0.18 + random() * 0.36;
    const points = [];
    for (let step = 0; step < 6; step += 1) {
      const t = -0.98 + step * 0.392;
      const bend = Math.sin((step + random() * 2) * 1.35 + i) * 0.07;
      points.push(vertical
        ? { x: base + bend + drift * step * 0.18, y: t }
        : { x: t, y: base + bend + drift * step * 0.18 });
    }
    paths.push({ kind: i % 4 === 0 ? "major" : "minor", points });
  }

  const river = [];
  for (let step = 0; step < 8; step += 1) {
    const t = -1 + step * 0.285;
    river.push({
      x: t,
      y: Math.sin(t * 3.1 + random() * 2.2) * 0.18 + 0.12
    });
  }
  paths.push({ kind: "river", points: river });

  return paths;
}

function drawMapWatermark(center, radius, intensity) {
  ctx.save();
  ctx.beginPath();
  ctx.arc(center, center, radius * 1.02, 0, Math.PI * 2);
  ctx.clip();
  ctx.globalCompositeOperation = "screen";

  state.mapPaths.forEach((path) => {
    const alpha = path.kind === "major" ? 0.17 : path.kind === "river" ? 0.13 : 0.09;
    ctx.strokeStyle = path.kind === "river"
      ? `rgba(82, 224, 121, ${alpha * intensity})`
      : `rgba(141, 255, 111, ${alpha * intensity})`;
    ctx.lineWidth = path.kind === "major" ? 3 : path.kind === "river" ? 5 : 1.5;
    ctx.beginPath();
    path.points.forEach((point, index) => {
      const x = center + point.x * radius * 0.92;
      const y = center + point.y * radius * 0.92;
      if (index === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  });

  ctx.strokeStyle = `rgba(82, 224, 121, ${0.08 * intensity})`;
  ctx.lineWidth = 1;
  for (let i = -2; i <= 2; i += 1) {
    const offset = i * radius * 0.22;
    ctx.beginPath();
    ctx.moveTo(center - radius, center + offset);
    ctx.lineTo(center + radius, center + offset + radius * 0.08);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(center + offset, center - radius);
    ctx.lineTo(center + offset - radius * 0.08, center + radius);
    ctx.stroke();
  }

  ctx.restore();
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
  ctx.fillStyle = "#52e079";
  ctx.font = "700 44px ui-sans-serif, system-ui";
  ctx.fillText(String(visible.length), center, 170);
  ctx.font = "700 18px ui-sans-serif, system-ui";
  ctx.fillText("AVIONS", center, 196);

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
});

document.getElementById("postal-control").addEventListener("input", (event) => {
  state.postalCode = event.target.value.trim() || "1000";
  state.mapPaths = buildMapPaths(state.postalCode);
  localStorage.setItem("flightdesk:postalCode", state.postalCode);
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

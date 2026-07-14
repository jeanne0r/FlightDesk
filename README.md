# FlightDesk

![FlightDesk overview](docs/assets/flightdesk-overview.jpg)

**FlightDesk** est un radar aérien personnel de bureau basé sur un écran tactile
rond **Waveshare ESP32-S3 Touch LCD 2.8C**. L'objectif : visualiser les avions
autour de chez soi en temps réel, sélectionner un vol au toucher, écouter des
annonces vocales et poser des questions à un assistant vocal Gemini intégré.

> État actuel : squelette initial du projet. La logique radar, les réglages
> persistants et le simulateur de trafic sont présents. L'adaptateur matériel
> exact de l'écran Waveshare sera finalisé à partir de l'exemple constructeur
> livré avec la carte afin d'éviter d'inventer un brochage.

## Expérience visée

- Radar vert animé inspiré d'un scope aérien compact.
- Suivi des avions proches via données ADS-B récupérées par Internet, avec
  rayon 20 / 50 / 100 / 250 km.
- Sélection tactile d'un avion pour afficher indicatif, compagnie, distance,
  altitude, vitesse et direction.
- Favoris pour mettre certains vols en avant.
- Assistant vocal Gemini pour interroger les avions visibles.
- Annonces vocales via micro I2S INMP441, ampli MAX98357A et haut-parleur.
- Réglages embarqués : position maison, rayon, intensité radar, mode nuit,
  volume, muet, langue, Wi-Fi et OTA.

## Matériel prévu

- Waveshare ESP32-S3 Touch LCD 2.8C.
- Microphone I2S INMP441.
- Amplificateur audio I2S MAX98357A.
- Haut-parleur 40 mm, 4 Ω / 3 W.
- Fournisseur de données trafic via Internet.
- Boîtier imprimé en 3D.
- Alimentation USB-C 5 V / 2 A.

## Démarrage firmware

1. Installer VS Code et PlatformIO.
2. Ouvrir le dossier `firmware`.
3. Copier `include/secrets.example.h` vers `include/secrets.h`.
4. Renseigner les secrets localement, sans les publier.
5. Compiler l'environnement `flightdesk`.
6. Avant le premier flash réel, reporter dans `include/board_pins.h` les broches
   de l'exemple officiel Waveshare correspondant exactement à la révision reçue.

## Preview simulateur

Un simulateur web permet de travailler l'interface avant réception du matériel :

```bash
cd simulator
python3 -m http.server 4173
```

Puis ouvrir `http://127.0.0.1:4173`.

Le simulateur couvre déjà le radar animé, les avions simulés, la sélection d'un
vol, les favoris, les réglages principaux et une réponse Gemini simulée.

## Structure

```text
FlightDesk/
├── docs/
│   ├── assets/
│   ├── ARCHITECTURE.md
│   └── ROADMAP.md
├── enclosure/
├── firmware/
│   ├── include/
│   ├── src/
│   └── platformio.ini
├── hardware/
├── simulator/
├── LICENSE
└── README.md
```

## Roadmap courte

- `v0.1` : architecture PlatformIO, modèle avion, radar simulé, réglages.
- `v0.2` : simulateur web, intégration écran Waveshare, LVGL et tactile.
- `v0.3` : Wi-Fi, position maison, fournisseur Internet et cache réseau.
- `v0.4` : pipeline audio INMP441 / MAX98357A.
- `v0.5` : Gemini, transcription, réponses vocales et contexte avions.
- `v1.0` : OTA, boîtier imprimé, guide de montage et release publique.

La roadmap détaillée est dans [docs/ROADMAP.md](docs/ROADMAP.md).

## Sécurité des clés

Ne jamais publier une clé Gemini, Wi-Fi ou fournisseur trafic dans GitHub.
`firmware/include/secrets.h` est ignoré par Git. Seul
`firmware/include/secrets.example.h` doit rester versionné.

## Licence

MIT

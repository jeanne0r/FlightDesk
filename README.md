# FlightDesk

Radar aérien de bureau DIY basé sur un **Waveshare ESP32-S3 Touch LCD 2.8C**, avec affichage radar vert, données ADS-B, tactile et assistant vocal Gemini.

> État actuel : squelette initial du projet. La logique radar, les réglages persistants et le simulateur de trafic sont présents. L'adaptateur matériel exact de l'écran Waveshare sera finalisé à partir de l'exemple constructeur livré avec la carte afin d'éviter d'inventer un brochage.

## Objectif

- radar vert animé ;
- avions autour d'une position configurable ;
- sélection tactile d'un avion ;
- rayon 20 / 50 / 100 / 250 km ;
- menu Réglages ;
- volume et mode muet logiciels ;
- micro I2S INMP441 ;
- ampli I2S MAX98357A et haut-parleur 4 Ω / 3 W ;
- interrogation vocale Gemini à la demande ;
- configuration Wi-Fi initiale ;
- mises à jour OTA.

## Matériel prévu

- Waveshare ESP32-S3 Touch LCD 2.8C ;
- INMP441 ;
- MAX98357A ;
- haut-parleur 40 mm, 4 Ω / 3 W ;
- fils et connecteurs ;
- boîtier imprimé en 3D ;
- alimentation USB-C 5 V / 2 A.

## Démarrage

1. Installer VS Code et PlatformIO.
2. Ouvrir le dossier `firmware`.
3. Copier `include/secrets.example.h` vers `include/secrets.h`.
4. Compiler l'environnement `flightdesk`.
5. Avant le premier flash réel, reporter dans `include/board_pins.h` les broches de l'exemple officiel Waveshare correspondant exactement à la révision reçue.

## Structure

```text
FlightDesk/
├── firmware/
│   ├── include/
│   ├── src/
│   └── platformio.ini
├── hardware/
├── enclosure/
├── docs/
├── LICENSE
└── README.md
```

## Sécurité des clés

Ne jamais publier une clé Gemini dans GitHub. `secrets.h` est ignoré par Git.

## Licence

MIT

# Architecture

```text
API trafic Internet / trafic simulé
          |
          v
     AdsbClient
          |
          v
 modèle Aircraft + radar_math
          |
          +------> DisplayAdapter / LVGL
          |
          +------> GeminiClient
          |
          +------> AudioManager
```

Les dépendances matérielles sont isolées afin de pouvoir changer d'écran sans
réécrire la logique trafic ou Gemini. FlightDesk ne prévoit pas de récepteur
ADS-B local : les positions d'avions seront récupérées par Internet.

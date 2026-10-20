# FlightDesk pour Waveshare ESP32-S3 Touch LCD 2.8C

Cette cible pilote le panneau rond 480 × 480 directement : initialisation du
ST7701 par SPI, transfert des pixels par le bus RGB 16 bits et tactile GT911
par I2C. Le port série est réservé aux diagnostics. La cible est le modèle
Waveshare 2.8C, SKU 29086, avec 16 Mo de flash et 8 Mo de PSRAM.

## Interface

Le radar reprend la palette verte, les cercles, le balayage, les avions, la
carte en filigrane et la navigation du simulateur web. Les touches permettent
de sélectionner un avion, ouvrir les pages, choisir un code postal parmi les
présélections, régler le rayon (20/50/100/250 km), basculer le mode nuit et
recentrer la carte. Le rayon et le mode nuit sont enregistrés en NVS.

La fiche avion récupère l'itinéraire, l'immatriculation, le type et une
miniature par le serveur du simulateur (`/api/aircraft` et
`/api/esp32/photo.jpg`). ADSBdb reste un repli si le serveur local est
indisponible. Ces informations sont conservées lors des mises à jour du trafic
et la sélection suit l'identifiant ICAO plutôt que la position de l'avion dans
la liste. Toutes les fiches n'ont pas nécessairement de photo.

L'ESP32 utilise le flux OpenSky du simulateur local pour retrouver les mêmes
avions que le web. Il tente ensuite les fournisseurs ADS-B directs. L'adresse
du simulateur est définie par `FLIGHTDESK_SIMULATOR_BASE_URL` dans les
options de compilation (par défaut `http://192.168.1.16:4173`). Les tuiles
OpenStreetMap sont téléchargées par l'ESP32. Une carte embarquée et des avions
de démonstration servent de repli. Les pages Recherche et Assistant restent
des maquettes fonctionnelles limitées : saisie texte et audio ne sont pas
encore portés. Le rendu doit encore être ajusté sur un écran physique.

## Compilation et flash

```bash
cd firmware/standalone-waveshare-2.8c
cp include/secrets.example.h include/secrets.h
# Renseigner les identifiants Wi-Fi dans secrets.h, sans les publier.
pio run -e waveshare_2_8c_bringup
pio run -e waveshare_2_8c_bringup -t upload
pio device monitor
```

Le portail Wi-Fi local `FlightDesk-Setup` permet aussi de configurer le réseau
sur l'appareil. L'environnement `waveshare_2_8c_ota` peut ensuite servir aux
mises à jour OTA lorsque l'appareil est joignable sur le réseau local.

Pour contrôler un appareil sur le LAN sans exposer les identifiants Wi-Fi :
`http://<ip-de-l-ecran>:8081/status` renvoie l'état de l'écran, du trafic et
de la fiche sélectionnée.

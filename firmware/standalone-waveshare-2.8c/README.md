# FlightDesk pour Waveshare ESP32-S3 Touch LCD 2.8C

Cette cible pilote le panneau rond 480 × 480 directement : initialisation du
ST7701 par SPI, transfert des pixels par le bus RGB 16 bits et tactile GT911
par I2C. Le port série est réservé aux diagnostics. La cible est le modèle
Waveshare 2.8C, SKU 29086, avec 16 Mo de flash et 8 Mo de PSRAM.

## Interface

Le radar reprend la palette verte, les cercles, le balayage, les avions, la
carte en filigrane et la navigation du simulateur web. Les touches permettent
de sélectionner un avion, ouvrir les pages, saisir un NPA suisse sur un clavier
tactile, régler le rayon (20/50/100/250 km) et recentrer la carte. Le NPA et
le rayon sont enregistrés en NVS. Les NPA hors des présélections sont résolus
directement par le service de recherche de swisstopo.

La fiche avion récupère directement par Internet l'itinéraire,
l'immatriculation, le type et une miniature. Elle interroge ADSBdb, HexDB pour
les appareils absents d'ADSBdb et Planespotters pour les photos. Certaines
routes ne sont pas publiées et certains appareils n'ont pas de photo. Les
informations trouvées sont conservées lors des mises à jour du trafic et la
sélection suit l'identifiant ICAO plutôt que la position dans la liste. Les
photos JPEG classiques et progressives sont décodées localement, redimensionnées
pour le cadre de la fiche et associées à l'identifiant de l'avion sélectionné.

L'ESP32 récupère le trafic directement sur Internet via adsb.fi, adsb.lol puis
airplanes.live. Il ne dépend d'aucun serveur sur le réseau local. Les tuiles
OpenStreetMap sont téléchargées en HTTPS et conservées en cache local LittleFS.
Pendant un glissement, la carte suit le doigt ; les avions sont repositionnés
au relâchement, puis le trafic est actualisé. Le trafic accepte jusqu'à 512 avions
dans le rayon choisi et conserve les dernières données lors d'une panne temporaire
de l'API. Les symboles avancent entre deux réponses selon leur cap et leur vitesse.
Une carte embarquée sert de repli ; les avions de démonstration ne
s'affichent que sans Wi-Fi. La barre du radar contient HOME, REGL et IA ;
HOME recentre la carte. La page Assistant reste une maquette limitée :
la saisie texte et audio n'est pas encore portée. Elle permet maintenant de
configurer une clé Gemini et de tester directement la connexion depuis l'ESP32.

## Configuration Gemini

Depuis l'écran, ouvrir **IA → CONFIG**. Se connecter au réseau
`FlightDesk-Setup` avec le mot de passe affiché, puis ouvrir
`http://192.168.4.1/ai` sur le téléphone. La clé API est enregistrée dans la
mémoire NVS de l'appareil, sans passer par le simulateur. Après enregistrement,
l'ESP32 appelle `gemini-3.5-flash-lite` pour vérifier l'accès au modèle. Le
bouton **TEST** relance cette vérification. La page et `/status` affichent le
résultat sans révéler la clé. Le certificat HTTPS de Gemini est vérifié avec
les racines Google Trust Services ; l'écran synchronise son horloge avant le
test. Le portail Wi-Fi dispose d'un mot de passe propre à chaque appareil.

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
sur l'appareil. Son mot de passe apparaît sur l'écran après avoir touché
**RÉGL → WIFI SETUP**. L'environnement `waveshare_2_8c_ota` peut ensuite servir aux
mises à jour OTA lorsque l'appareil est joignable sur le réseau local.

Pour contrôler un appareil sur le LAN sans exposer les identifiants Wi-Fi :
`http://<ip-de-l-ecran>:8081/status` renvoie l'état de l'écran, du trafic et
de la fiche sélectionnée.

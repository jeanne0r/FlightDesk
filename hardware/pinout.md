# Validation des GPIO Waveshare 2.8C

Carte reçue : Waveshare `ESP32-S3-Touch-LCD-2.8C`, SKU `29086`.

Le port de bring-up est dans
`firmware/standalone-waveshare-2.8c`.

GPIO déjà documentés :

| Fonction | GPIO |
|---|---:|
| I2C SCL externe | 7 |
| I2C SDA externe | 15 |

À compléter depuis l'exemple constructeur officiel avant de brancher l'audio.

Checklist :

- [x] Révision exacte de la carte confirmée
- [ ] GPIO écran relevés
- [ ] GPIO tactile relevés
- [ ] GPIO microSD relevés
- [ ] GPIO IMU/RTC relevés
- [x] I2C externe relevé
- [ ] GPIO libres identifiés
- [ ] I2S BCLK choisi
- [ ] I2S LRCLK choisi
- [ ] I2S entrée micro choisi
- [ ] I2S sortie haut-parleur choisi
- [ ] Aucun conflit au démarrage

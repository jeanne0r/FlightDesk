# Câblage prévisionnel Waveshare 2.8C

## Important

Les GPIO restent volontairement à `-1` dans `firmware/include/board_pins.h`.

Avant tout branchement, relever le brochage exact dans l'exemple officiel
Waveshare livré pour la révision de la carte reçue. Les modules intégrés
peuvent déjà utiliser plusieurs GPIO.

La carte reçue expose au moins un bus I2C documenté :

| Signal | GPIO |
|---|---:|
| I2C SCL | 7 |
| I2C SDA | 15 |

Ce bus est utilisé pour le bring-up et les périphériques I2C. Ne pas y brancher
les signaux I2S audio.

## INMP441

| INMP441 | Connexion |
|---|---|
| VDD | 3,3 V |
| GND | GND |
| SCK | I2S BCLK validé |
| WS | I2S LRCLK validé |
| SD | I2S MIC DIN validé |
| L/R | GND pour canal gauche |

## MAX98357A

| MAX98357A | Connexion |
|---|---|
| VIN | 5 V |
| GND | GND |
| BCLK | même I2S BCLK |
| LRC | même I2S LRCLK |
| DIN | I2S SPK DOUT validé |
| SPK+ / SPK- | haut-parleur 4 Ω / 3 W |

Ne jamais relier une sortie du haut-parleur à la masse.

## Ordre de montage

1. Flasher le firmware bring-up Waveshare et confirmer le boot série.
2. Confirmer le scan I2C sur GPIO7/GPIO15.
3. Importer l'initialisation écran/tactile officielle Waveshare.
4. Identifier les GPIO libres après écran, tactile, SD, IMU, RTC et buzzer.
5. Choisir les trois lignes I2S partagées `BCLK`, `LRCLK`, `MIC DIN` et la
   sortie `SPK DOUT`.
6. Brancher seulement l'INMP441 et valider l'enregistrement.
7. Brancher ensuite le MAX98357A et le haut-parleur.
8. Activer Gemini audio en dernier.

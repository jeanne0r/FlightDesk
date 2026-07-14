# Câblage audio prévisionnel

## Important

Les GPIO restent volontairement à `-1` dans `firmware/include/board_pins.h`.

Avant tout branchement, relever le brochage exact dans l'exemple officiel
Waveshare livré pour la révision de la carte reçue. Les modules intégrés
peuvent déjà utiliser plusieurs GPIO.

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

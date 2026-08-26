# Application moteur Cortex-M4

Cette image réalise la première partie de la phase 9, sans potentiomètre et sans IPC.

## Build et flash

```bash
west build -p always -d build_m4 \
  -b stm32h747i_disco/stm32h747xx/m4 app_m4
west flash -d build_m4
```

La STM32H747I-DISCO démarre normalement les deux cœurs. L'image M7 réside à
`0x08000000` et l'image M4 à `0x08100000`. Flasher l'une ne doit pas remplacer l'autre.

## Comportement

- READY : LD4 bleue clignote ; moteur arrêté.
- START : démarre en direction courante avec un PWM fixe à 90 % et 5 kHz.
- STOP : arrête immédiatement le PWM et désactive les deux entrées du pont.
- DIRECTION : inverse la direction uniquement lorsque le moteur est arrêté.
- RESET : arrête, efface le défaut logiciel et revient en direction avant.
- FAULT : LD3 rouge fixe ; moteur désactivé.

Le bouton STOP est une commande logicielle échantillonnée et anti-rebondée ; ce
n'est pas un arrêt d'urgence certifié. Un arrêt de sécurité réel doit couper
matériellement l'alimentation du moteur.

Ajouter une résistance de rappel de 10 kΩ entre `EN1,2` et GND sur le montage afin
que le L293D reste désactivé pendant le reset et avant l'initialisation Zephyr.

## Câblage STMod+ et L293D

| STMod+ P2 | MCU | Connexion externe |
|---:|---|---|
| 14 | PF8 | L293D pin 1, `EN1,2` |
| 1 | PA11 | L293D pin 2, `IN1/1A` |
| 2 | PC3 | L293D pin 7, `IN2/2A` |
| 8 | PB15 | Bouton START, autre borne vers GND |
| 9 | PB14 | Bouton STOP, autre borne vers GND |
| 3 | PC2 | Bouton DIRECTION, autre borne vers GND |
| 12 | PJ13 | Bouton RESET, autre borne vers GND |
| 5 ou 16 | GND | Masse commune STM32, L293D et MB V2 |

Les pull-up internes STM32 sont activés : ne pas relier les boutons au +5 V.
P2.13/PA4 reste libre pour le futur potentiomètre. Ce brochage suppose la
configuration SPI d'origine de la carte principale : SB32, SB34 et SB36 fermés.
Les étiquettes `CS`, `MOSI` et `MISO` du fan-out correspondent respectivement à
P2.1, P2.2 et P2.3.

Ne pas utiliser P2.7/P2.10, réservées à l'I2C4 du LCD. P2.11, P2.17, P2.19 et
P2.20 sont également évitées car leurs GPIO sont affectés à SDMMC1 sur le M7.

| L293D | Connexion |
|---:|---|
| 1 | PWM P2.14 + résistance 10 kΩ vers GND |
| 2 | IN1, P2.1 |
| 3 | Première borne moteur |
| 4, 5, 12, 13 | GND commun |
| 6 | Deuxième borne moteur |
| 7 | IN2, P2.2 |
| 8 | `VCC2`, alimentation moteur +5 V MB V2 |
| 9 | GND, deuxième pont désactivé |
| 10, 15 | GND, entrées inutilisées fixées |
| 11, 14 | Non connectées |
| 16 | `VCC1`, logique +5 V MB V2 |

Ne pas relier ensemble le +5 V de P2 et le +5 V de la MB V2 lorsqu'ils sont
alimentés par deux sources différentes. Relier uniquement les masses.

Placer au minimum un condensateur céramique de 100 nF entre les bornes du moteur,
un 100 nF entre VCC1 et GND, puis un 100 nF accompagné de 47 à 100 µF entre VCC2
et GND près du L293D.

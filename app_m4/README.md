# Application moteur Cortex-M4

Cette image réalise la phase 9 locale : boutons, potentiomètre, machine d'état et
commande PWM. Elle reste autonome pour le pilotage du moteur. La phase 10.1 ajoute
un canal IPC de diagnostic avec le M7, sans encore exposer de commande moteur à
distance.

## Build et flash

```bash
west build -p always -d build_m4 \
  -b stm32h747i_disco/stm32h747xx/m4 app_m4
west flash -d build_m4
```

La STM32H747I-DISCO démarre normalement les deux cœurs. L'image M7 réside à
`0x08000000` et l'image M4 à `0x08100000`. Flasher l'une ne doit pas remplacer l'autre.

## Validation IPC M7 ↔ M4 — phase 10.1

Les deux images utilisent le backend Zephyr `IPC Service` / `icmsg`, deux zones
non cacheables de 32 KiB dans la SRAM4 partagée, et les mailboxes matériels.
L'endpoint commun s'appelle `motor-control`.

Construire puis flasher les deux cœurs, avant de tester depuis le shell UART du M7 :

```bash
west build -p always -d build_m7 \
  -b stm32h747i_disco/stm32h747xx/m7 app_m7 -- \
  -DSHIELD=st_b_lcd40_dsi1_mb1166 -DEXTRA_CONF_FILE=lcd.conf
west flash -d build_m7 --runner openocd

west build -p always -d build_m4 \
  -b stm32h747i_disco/stm32h747xx/m4 app_m4
west flash -d build_m4 --runner openocd
```

```text
uart:~$ m4 status
IPC initialized : yes
M4 endpoint    : bound
Last error     : 0

uart:~$ m4 ping
M4 pong in 0..20 ms
```

Le délai dépend de la boucle M4 de 10 ms et de l'ordonnancement des deux cœurs ;
une valeur comprise entre 0 et 20 ms est normale. Si l'endpoint reste `unbound`,
vérifier que les deux images ont été flashées puis effectuer un reset matériel de la carte.
Le moteur demeure exclusivement contrôlé par ses boutons et son potentiomètre à
cette étape.

## Comportement

- READY : LD4 bleue clignote lentement ; moteur arrêté.
- START : applique un boost à 100 % pendant 250 ms, puis rejoint progressivement
  la consigne du potentiomètre avec un PWM à 5 kHz.
- Potentiomètre : la course `0..3,3 V` est filtrée et convertie en `800..1000 ‰`
  (80 à 100 %), plage utile constatée avec ce moteur et ce L293D.
- STOP : passe en `STOPPING`, descend le PWM à zéro en environ 250 ms au maximum,
  désactive `IN1/IN2`, puis revient en READY.
- DIRECTION : inverse la direction uniquement lorsque le moteur est arrêté.
- RESET : coupe immédiatement le pont, efface le défaut logiciel et revient en
  direction avant.
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
| 13 | PA4 / ADC1_INP18 | Broche centrale du potentiomètre |
| 5 ou 16 | GND | Masse commune STM32, L293D et MB V2 |

Les pull-up internes STM32 sont activés : ne pas relier les boutons au +5 V.
Ce brochage suppose la configuration SPI d'origine de la carte principale :
SB32, SB34 et SB36 fermés.
Les étiquettes `CS`, `MOSI` et `MISO` du fan-out correspondent respectivement à
P2.1, P2.2 et P2.3.

Ne pas utiliser P2.7/P2.10, réservées à l'I2C4 du LCD. P2.11, P2.17, P2.19 et
P2.20 sont également évitées car leurs GPIO sont affectés à SDMMC1 sur le M7.

### Potentiomètre à trois broches

Vu côté broches, la borne centrale est le curseur. Relier :

| Broche du potentiomètre | Connexion |
|---|---|
| Une borne extérieure | `3V3` de la carte/fan-out STMod+ |
| Borne centrale (curseur) | P2.13 `AN`, donc PA4 |
| Autre borne extérieure | GND commun |

Ne jamais alimenter le potentiomètre en 5 V : PA4 accepte ici une mesure comprise
entre 0 et 3,3 V. Si le sens de rotation paraît inversé, permuter uniquement les
deux bornes extérieures. Une valeur de 10 kΩ est idéale. Le filtrage logiciel rend
le condensateur sur le curseur optionnel.

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

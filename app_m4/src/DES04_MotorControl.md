# DES04 — Contrôle moteur Cortex-M4

## Objectif

L'application M4 pilote localement un moteur DC via un L293D. Elle ne dépend ni du
réseau, ni du M7, ni de l'IPC pendant la phase 9.

```mermaid
flowchart LR
    B["4 boutons STMod+"] --> DB["Anti-rebond 30 ms"]
    DB --> LOOP["Boucle motor_main"]
    LOOP --> SM["Machine d'état moteur"]
    SM --> PWM["TIM13_CH1 / PF8"]
    SM --> DIR["PA11 + PC3"]
    PWM --> L293D["L293D"]
    DIR --> L293D
    L293D --> M["Moteur DC"]
    SM --> LED["LD3 rouge + LD4 bleue"]
```

## Contexte d'exécution

| Élément | Valeur |
|---|---|
| Cœur | Cortex-M4 à 200 MHz |
| Tâche | Thread Zephyr `main` (`motor_main`) |
| Stack | 2048 octets |
| Priorité | Préemptive 5 |
| Période | 10 ms |
| Anti-rebond | 3 échantillons, soit 30 ms |
| PWM | TIM13_CH1, 5 kHz, consigne fixe 90 % |

## Machine d'état

```mermaid
stateDiagram-v2
    [*] --> READY: initialisation sûre
    READY --> RUNNING: START
    RUNNING --> READY: STOP
    READY --> READY: DIRECTION
    RUNNING --> RUNNING: DIRECTION ignorée
    READY --> READY: RESET
    RUNNING --> READY: RESET
    READY --> FAULT: erreur GPIO/PWM
    RUNNING --> FAULT: erreur GPIO/PWM
    FAULT --> READY: RESET réussi
```

- Le PWM et `IN1/IN2` sont mis à zéro au boot, à l'arrêt et en défaut.
- `STOP` est prioritaire sur toutes les commandes du même cycle.
- La direction ne change que lorsque le moteur est arrêté.
- Aucun redémarrage automatique n'est effectué après un boot ou un défaut.

## Allocation STMod+ P2

| P2 | MCU | Usage |
|---:|---|---|
| 14 | PF8 | PWM L293D `EN1,2` |
| 1 | PA11 | L293D `IN1` via SB32 |
| 2 | PC3 | L293D `IN2` via SB34 |
| 8 | PB15 | START |
| 9 | PB14 | STOP |
| 3 | PC2 | DIRECTION via SB36 |
| 12 | PJ13 | RESET |
| 13 | PA4 | Réservé au potentiomètre |

Les broches P2.1, P2.2 et P2.3 utilisent le routage SPI d'origine de la carte
principale (SB32, SB34 et SB36 fermés). Les broches PD12/PD13 restent réservées à
l'I2C4 du LCD M7. PC6, PC7, PB9 et PB8 restent au SDMMC1 du M7. Les fonctions
audio qui partagent PC3, PJ13 et PB14 ne doivent pas être activées simultanément.

## Signalisation

| État | LD4 bleue | LD3 rouge |
|---|---|---|
| READY | Clignotement 1 Hz | Éteinte |
| RUNNING | Allumée | Éteinte |
| FAULT | Éteinte | Allumée |

## Sources

- `main.c` : boucle, arbitrage des boutons et LED.
- `motor_buttons.c` : lecture et anti-rebond.
- `motor_control.c` : état, direction et PWM.
- `boards/stm32h747i_disco_stm32h747xx_m4.overlay` : câblage STMod+.

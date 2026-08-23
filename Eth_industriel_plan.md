# Plan Démo : Ethernet Industriel sous Zephyr

## Chapitre 1 — Objectif & Résumé du plan

### 1.1 — But du projet

Ce plan décrit la réalisation d'un **prototype embarqué industriel** tournant sous **Zephyr RTOS**, dont la vocation est de démontrer, sur une carte de développement dual-core (~€85), les mêmes capacités protocolaires et sécuritaires que des cartes de communication industrielles professionnelles.

Le projet vise trois objectifs complémentaires :

1. **Valider la faisabilité d'une stack industrielle complète sous Zephyr** : TCP/IP IPv4/IPv6, Modbus TCP (server + scanner via Unit-ID 2), EtherNet/IP, DPWS/WS-Discovery, DHCP, mDNS, HTTPS, gestion de login.
2. **Démontrer un niveau de sécurité proche du monde industriel** : firmware signé, secure boot avec chaîne de confiance, protection du code en flash.
3. **Donner un rôle produit à la carte** : le M7 joue la carte de communication industrielle et le M4 pilote localement un petit moteur DC via un L293D, avec commandes physiques, signalisation et arrêt sur défaut.

Le résultat attendu est un **device réseau découvrable automatiquement**, exposant :
- un **webserver HTTP/HTTPS dual-stack IPv4/IPv6** avec interface de supervision et d'administration (registres, diagnostics, FW update),
- un **serveur Modbus TCP** (port 502) avec Unit-ID 1 pour les registres principaux et Unit-ID 2 pour la zone scanner,
- un **device EtherNet/IP** identifiable par un automate Rockwell ou un outil EIPScan,
- un **endpoint WS-Discovery** (DPWS, port UDP 3702) permettant la découverte automatique sur le réseau local,
- un **contrôleur moteur local sur Cortex-M4**, commandable ensuite depuis un PLC ou le Web par l'intermédiaire du M7.

---

### 1.2 — Résumé du plan

Le plan est structuré en **phases 0 à 15**, chacune apportant une couche fonctionnelle ou sécuritaire supplémentaire. Les phases sont conçues pour être validées indépendamment, chaque livrable étant testable avant de passer à la suivante.

| Phase | Thème | Livrable clé |
|---|---|---|
| **Phase 0** | Setup toolchain (Zephyr SDK, West, ST-Link) | `west flash` fonctionne sur STM32H747I-DISCO |
| **Phase 1** | Connectivité réseau (Ethernet, DHCP, ping) | Carte joignable sur le réseau via DHCP |
| **Phase 2** | Modbus TCP Server (FC03/04/06/16/23) | Lecture/écriture Modbus depuis QModMaster |
| **Phase 3** | Modbus Scanner via Unit-ID 2 | Zone 10 registres pilotable par un device externe |
| **Phase 4** | Webserver HTTP + REST API + frontend | Dashboard web + registres/scanner pilotables |
| **Phase 5** | Dashboard LCD local (Cortex-M7, LVGL) | LCD affichant IP, services et fenêtre scanner |
| **Phase 6** | EtherNet/IP via OpENer | Device CIP identifiable par RSLinx / EIPScan |
| **Phase 7** | IPv6, identification et HTTP dual-stack | Ping IPv6, commande `ident`, dashboard sur IPv4 et IPv6 |
| **Phase 8** | DPWS / WS-Discovery (UDP multicast 3702) | Découverte auto depuis WSDiscoveryTool |
| **Phase 9** | Application M4 et commande locale du moteur DC | Moteur piloté par boutons, L293D et PWM, état sur LD3/LD4 |
| **Phase 10** | IPC M7↔M4 et commande distante | Moteur commandable par Modbus, EtherNet/IP et Web |
| **Phase 11** | Sécurité HTTP : login, tokens, HTTPS/TLS | Interface protégée par login + TLS (certif ECC P-256) |
| **Phase 12** | Firmware Update + rollback MCUboot | Upload d'un `.bin` signé via le web, rollback auto |
| **Phase 13** | Signature ECDSA des images | Seule une image signée par la clé légitime s'installe |
| **Phase 14** | Secure Boot (RDP, OTP, chain of trust) | Chaîne ROM → MCUboot → Apps M7/M4 inviolable |
| **Phase 15** | Polish : logs, diagnostics, mDNS, doc | Démo finale complète + hostname `industrial-ethernet.local` |


**Carte retenue : STM32H747I-DISCO** — Dual-core Cortex-M7 @ 480 MHz + Cortex-M4 @ 240 MHz, 1 MB SRAM + 32 MB SDRAM, 2 MB Flash + 128 MB QSPI, Ethernet PHY + RJ45 intégrés, LCD 4" tactile, crypto HW complet (AES-256/HASH/RNG/PKA), support Zephyr ★★★★★.

**Stack logicielle finale** : Zephyr 3.7 LTS · MCUboot · mbedTLS · LittleFS · HTTP server v2 · Modbus subsys · OpENer (EIP) · DPWS custom ~200 LOC · LVGL (LCD dashboard) · HTML+JS vanilla · West + CMake + imgtool.

---

## Chapitre 2 — Choix de la carte

### Tableau comparatif (orienté démo industrielle)

| Carte | MCU | RAM | Flash | ETH | Crypto HW | Prix | Zephyr | Note démo |
|---|---|---|---|---|---|---|---|---|
| **STM32H747I-DISCO** | STM32H747 **M7 480MHz + M4 240MHz** | 1 MB + **32 MB SDRAM** | 2 MB + **128 MB QSPI** | 100M (PHY LAN8742) | AES-256/HASH/RNG/PKA | ~€85 | ★★★★★ | **✅ RETENUE** |
| Nucleo-H753ZI | STM32H753 M7 480MHz | 1 MB | 2 MB | 100M (PHY LAN8742) | AES-256/DES/SHA/RNG/PKA | ~€40 | ★★★★★ | Alternative single-core |
| MIMXRT1060-EVK | i.MX RT1060 M7 600MHz | 1 MB TCM | QSPI ext. | 100M | CAAM | ~€80 | ★★★★ | Plus proche du Sitara |
| Nucleo-F767ZI | STM32F767 M7 216MHz | 512 KB | 2 MB | 100M | AES/HASH/RNG | ~€25 | ★★★★★ | Backup low-cost |
| SAM E70 Xplained | SAME70 M7 300MHz | 384 KB | 2 MB | **Gigabit** | TRNG/AES | ~€60 | ★★★★ | Si Gigabit requis |

### Décision : **STM32H747I-DISCO (Discovery kit)**

**Justification :**
- **Dual-core** Cortex-M7 (480 MHz) + Cortex-M4 (240 MHz) → architecture très proche du Sitara (un cœur réseau + un cœur temps réel)
- PHY LAN8742 + connecteur RJ45 **intégrés** et soudés → prêt à l'emploi
- **1 MB SRAM + 32 MB SDRAM externe** → aucune contrainte mémoire (TLS, EIP, Modbus, webserver simultanés)
- **2 MB Flash interne + 128 MB QSPI NOR** → MCUboot dual-slot très confortable, LittleFS pages web illimité
- **LCD 4" tactile capacitif (800×480)** → dashboard local en plus du webserver, très visuel pour la démo
- Crypto HW complet (AES-256, HASH SHA HW, RNG, PKA) → secure boot + signature accélérés
- ST-Link **V3** intégré → flash + debug rapide en USB unique
- Support Zephyr `stm32h747i_disco/stm32h747xx/m7` et `/m4` mature
- USB OTG HS → alternative FW update via USB
- Communauté STM32 énorme → résolution rapide des bugs

### Architecture dual-core retenue

```
┌─────────────────────────────────────────────────────────┐
│  Cortex-M7 @ 480 MHz (maître)                           │
│  ├── Zephyr + Net stack (TCP/IP, DHCP, mDNS)           │
│  ├── HTTP/HTTPS server (port 80/443)                   │
│  ├── Modbus TCP server multi Unit-ID (port 502)        │
│  ├── EtherNet/IP — OpENer (port 44818)                 │
│  ├── DPWS / WS-Discovery (port 3702)                  │
│  ├── LCD tactile / LVGL et supervision                 │
│  ├── mbedTLS (TLS handshake, signature)                │
│  └── MCUboot (firmware update + rollback)              │
├─────────────────────────────────────────────────────────┤
│  Cortex-M4 @ 240 MHz (contrôle temps réel)              │
│  ├── Machine d'état du moteur DC                       │
│  ├── PWM + direction vers le L293D                     │
│  ├── Boutons, potentiomètre, LEDs LD3/LD4              │
│  └── Watchdog de commande et arrêt local               │
└─────────────────────────────────────────────────────────┘
         ▲▼ Shared SRAM + mailbox (Zephyr IPC Service / icmsg)
```

---

## Chapitre 3 — Plan détaillé Step-by-Step

### Phase 0 — Setup environnement (1 étape)

**Étape 0.1 — Toolchain & SDK**
- Installer Zephyr SDK ≥ 0.17 (toolchain `arm-zephyr-eabi`)
- Installer West : `pip install west`
- Init workspace : `west init demo-ws && cd demo-ws && west update`
- Vérifier OpenOCD / ST-Link
- Test : flasher `samples/hello_world` sur la STM32H747I-DISCO (cœur M7)

**Livrable :** `west build -b stm32h747i_disco/stm32h747xx/m7 samples/hello_world && west flash` fonctionne.

---

### Phase 1 — Connectivité réseau de base

**Étape 1.1 — Ethernet brut + LED de vie**
- Sample `samples/net/zperf` ou un blink + activation `CONFIG_NET_L2_ETHERNET=y`
- Vérifier que le lien ETH monte (LED PHY allumée, log `ethernet link up`)

**Étape 1.2 — DHCP client**
```kconfig
CONFIG_NET_DHCPV4=y
CONFIG_NET_IPV4=y
CONFIG_NET_LOG=y
```
- Connecter la carte au routeur, vérifier que l'IP est attribuée
- Log attendu : `DHCPv4 lease acquired ... 192.168.x.x`

**Étape 1.3 — Ping**
- `ping <IP>` depuis le PC doit répondre
- Sample : `samples/net/sockets/echo_server` pour valider TCP

**Livrable :** carte joignable en réseau, DHCP fonctionnel.

---

### Phase 2 — Modbus TCP Server

**Étape 2.1 — Activer Modbus**
```kconfig
CONFIG_MODBUS=y
CONFIG_MODBUS_SERVER=y
CONFIG_MODBUS_FC08_DIAGNOSTIC=y
```
- Zephyr fournit un sample `samples/subsys/modbus/tcp_server` à adapter

**Étape 2.2 — Mapping registres interne**
- Exposer un tableau `static uint16_t registers[100]` via les callbacks Modbus
- Implémenter callbacks Modbus :
  - `holding_reg_rd(addr, *reg)` → `*reg = registers[addr]`
  - `holding_reg_wr(addr, reg)` → `registers[addr] = reg`
  - `coil_rd/wr`, `input_reg_rd`, `discrete_input_rd`
- Fonctions supportées : FC03, FC04, FC06, FC16, FC23

**Étape 2.3 — Test client**
- Outil : **modpoll** ou **QModMaster** depuis PC
- `modpoll -m tcp -a 1 -r 1 -c 10 <IP>` → lecture
- `modpoll -m tcp -a 1 -r 1 192.168.x.x 1234` → écriture

**Livrable :** Modbus TCP server unit-ID 1, port 502, mappé sur `registers[]`.

---

### Phase 3 — Modbus Scanner via Unit-ID 2

Objectif : exposer sur la même carte deux espaces Modbus distincts, pilotables par un device externe :
- **Unit-ID 1** : registres principaux de la carte, déjà exposés par la phase 2.
- **Unit-ID 2** : zone "scanner" limitée à 10 registres (`scanner_regs[0..9]`) utilisée pour lire et écrire périodiquement les valeurs préparées depuis l'extérieur.

Dans cette phase, la carte reste **Modbus TCP server**. Le "scanner" est volontairement modélisé comme une zone de registres dédiée accessible via Unit-ID 2, afin qu'une PLC, un PC ou un autre device Modbus puisse lire/écrire périodiquement les valeurs sans ajouter tout de suite un client Modbus embarqué.

**Étape 3.1 — Routage par Unit-ID**
- Conserver le serveur Modbus TCP sur le port 502.
- Router les requêtes selon l'Unit-ID :
  - Unit-ID 1 → `registers[]` de la phase 2.
  - Unit-ID 2 → `scanner_regs[10]`.
- Refuser proprement toute adresse registre hors plage `0..9` pour Unit-ID 2.
- Supporter au minimum FC03, FC06 et FC16 sur les registres Unit-ID 2.

**Étape 3.2 — Table de mapping scanner**
- Ajouter dans Unit-ID 1 une table de mapping dédiée :
  - `registers[40..49]` : mapping des 10 registres scanner.
  - `registers[40 + i]` contient directement l'adresse du holding register exposé par `scanner_regs[i]`.
  - `0xFFFF` = RAM locale, avec lecture/écriture dans une valeur propre à `scanner_regs[i]`.
- Initialiser le mapping par défaut au boot :
  - `registers[40..49] = 10..19` → `scanner_regs[0..9]` exposent les registres libres `registers[10..19]`.
  - Ce choix protège la configuration réseau (`registers[1..7]`) et les commandes (`registers[8..9]`) des écritures cycliques externes.
- Le device externe peut modifier ce mapping en runtime en écrivant dans `registers[40..49]` via Unit-ID 1.

**Étape 3.3 — Fenêtre scanner dynamique**
- `scanner_regs[0..9]` n'est pas une copie fixe : c'est une fenêtre dynamique pilotée par `registers[40..49]`.
- Quand un device externe lit `scanner_regs[i]` via Unit-ID 2, la carte retourne la valeur interne indiquée par `registers[40 + i]`.
- Quand un device externe écrit `scanner_regs[i]` via Unit-ID 2, la carte écrit dans la donnée interne indiquée par `registers[40 + i]`.
- Si `registers[40 + i] = 0xFFFF`, `scanner_regs[i]` lit/écrit une valeur RAM locale.
- Exemple :
  - au boot : `registers[40] = 10`, donc `scanner_regs[0]` lit/écrit `registers[10]`.
  - si le device externe écrit ensuite `registers[40] = 4` et `registers[41] = 5`, alors `scanner_regs[0]` et `scanner_regs[1]` exposent le masque réseau.
  - une écriture dans `scanner_regs[0]` met alors à jour le mot haut du masque, pas `registers[10]`.
- Le device externe peut lire/écrire périodiquement les adresses `0..9` via Unit-ID 2 sans connaître les registres internes réels.

Par défaut, les valeurs de `scanner_regs[0..9]` sont les valeurs brutes de `registers[10..19]`.

**Étape 3.4 — Test avec device externe**
- Depuis un PC ou une PLC, lire `registers[40..49]` via Unit-ID 1 pour vérifier le mapping scanner courant.
- Modifier `registers[40..49]` via Unit-ID 1 pour changer ce que Unit-ID 2 expose.
- Lire/écrire ensuite Unit-ID 2, adresses `0..9`, pour accéder aux valeurs mappées.
- Vérifier que les lectures/écritures périodiques sur Unit-ID 2 mettent à jour les diagnostics.

Exemples de tests avec `modpoll` :
```bash
# Lire le mapping scanner via Unit-ID 1 : registers[40..49]
modpoll -m tcp -a 1 -r 41 -c 10 <IP>

# Lire les 10 registres scanner via Unit-ID 2
modpoll -m tcp -a 2 -r 1 -c 10 <IP>

# Remapper scanner_regs[0..1] vers le masque réseau MSW/LSW
modpoll -m tcp -a 1 -r 41 -c 2 <IP> 4 5

# Écrire dans scanner_regs[0] : met à jour le mot haut du masque réseau
modpoll -m tcp -a 2 -r 1 <IP> 65535
```

**Livrable :** serveur Modbus TCP multi Unit-ID : Unit-ID 1 configure le mapping scanner via `registers[40..49]`, Unit-ID 2 expose une fenêtre `scanner_regs[0..9]` lisible/écrivable périodiquement selon ce mapping.

---

### Phase 4 — Webserver HTTP + REST API + frontend

Objectif : exposer une interface web embarquée de supervision/configuration, sans framework lourd, connectée aux mêmes données que Modbus.

**Étape 4.1 — Serveur HTTP embarqué**
```kconfig
CONFIG_NET_SOCKETS=y
CONFIG_NET_MAX_CONTEXTS=14
```
- Implémentation actuelle : serveur HTTP léger basé sur sockets Zephyr, port 80.
- Assets web embarqués provisoirement en C : HTML/CSS/JS + logo JPG servi via `GET /logo.jpg`.
- Migration prévue plus tard : assets statiques dans LittleFS/QSPI (`/www/index.html`, `/www/app.js`, `/www/logo.jpg`) et éventuellement HTTP server v2.

**Étape 4.2 — REST API minimale**
- `GET /api/status` → état réel actif de la carte : IP active, mode actif, heartbeat, connexions Modbus.
- `GET /api/registers` → tableau des 50 holding registers.
- `GET /api/registers/<id>` → lecture d'un registre.
- `PUT /api/registers/<id>` → écriture d'un registre avec body JSON `{"value":123}`.
- `GET /api/scanner` → mapping `registers[40..49]` + fenêtre Unit-ID 2.
- `GET /api/scanner/<id>` → lecture d'un slot scanner.
- `PUT /api/scanner/<id>` → écriture d'un slot scanner avec body JSON `{"value":123}`.
- `POST /api/commands/save-config` → déclenche la sauvegarde de configuration réseau.
- Convention retenue : `GET` pour lire, `PUT` pour modifier une ressource connue, `POST` pour déclencher une commande.

**Étape 4.3 — Frontend industriel**
- HTML/CSS/Vanilla JS, sans framework lourd.
- Dashboard :
  - KPI : mode IP actif, IP active, connexions Modbus, heartbeat.
  - Badge de configuration : vert si les registres réseau correspondent à l'état actif, orange si un reboot/apply est requis.
  - Deux panneaux graphiques génériques de prévisualisation pour futurs widgets.
- Page Registres :
  - registres réseau décodés : signature en hex, mode DHCP/Static, IP/mask/gateway au format IPv4.
  - commande `Save config` via REST API.
  - mapping scanner `REG40..REG49` affiché avant les registres libres.
  - registres libres `REG10..REG39` en valeurs brutes.
- Page Scanner :
  - `Scanner 0..9` avec cible lisible, par exemple `Mode IP (REG1)` ou `Free`.
  - écriture via `PUT /api/scanner/<id>`.
- Refresh automatique toutes les 1,5 s, avec verrou d'édition pour ne pas écraser les valeurs en cours de saisie.

**Livrable :** ouvrir `http://<IP>/`, superviser l'état actif, modifier les registres via REST API, configurer le mapping scanner et visualiser la fenêtre Unit-ID 2.

---

### Phase 5 — Dashboard LCD local (Cortex-M7)

Objectif : ajouter une supervision locale sur l'écran tactile de la STM32H747I-DISCO, indépendante du webserver.

> **Choix d'architecture :** sous Zephyr 4.4, le shield LCD `st_b_lcd40_dsi1_mb1166`
> et les périphériques LTDC/MIPI-DSI sont supportés sur la cible M7 uniquement. Le M4 est
> réservé au contrôle temps réel du moteur de la phase 9, puis communique avec le M7 via
> Zephyr IPC Service / `icmsg` à partir de la phase 10.

**Étape 5.1 — Activer le LCD et LVGL**
- Ajouter le module `lvgl` au manifeste West puis exécuter `west update`.
- Construire l'image M7 avec le shield correspondant à la révision de la dalle :
  - `west build -p always -b stm32h747i_disco/stm32h747xx/m7 app -- -DSHIELD=st_b_lcd40_dsi1_mb1166 -DEXTRA_CONF_FILE=lcd.conf`
  - variante dalle A09 : `-DSHIELD=st_b_lcd40_dsi1_mb1166_a09`.

**Étape 5.2 — Module UI séparé**
- Sources dans `app/src/ui/app_lcd.c` et `app_lcd.h`.
- Une tâche LCD dédiée est l'unique propriétaire des objets LVGL.
- La configuration LVGL est isolée dans `app/lcd.conf` : le build Ethernet historique sans shield reste disponible et le dashboard est alors désactivé avec un log explicite.

**Étape 5.3 — Dashboard local**
- LVGL affiche sur le LCD 4" :
  - IP courante.
  - mode DHCP/statique.
  - état du lien Ethernet.
  - nombre de connexions Modbus et heartbeat.
  - statut de la dernière commande.
  - cinq premières valeurs de la fenêtre scanner sous forme de barres.
- Rafraîchissement local toutes les 500 ms, sans navigateur et sans requête HTTP.
- Démarrage en veille du panneau et du backlight ; le premier toucher réveille l'affichage sans déclencher d'action dans l'IHM. Le bouton `Sleep` remet l'écran en veille.

**Livrable :** le LCD affiche un dashboard local alimenté directement par les services M7, sans dépendre du navigateur web.

---

### Phase 6 — EtherNet/IP (CIP)

**Étape 6.1 — Stack EIP**
- Zephyr n'a pas de stack EIP officielle.
- Choix retenu : **OpENer**, intégré comme sous-module Git dans `app/third_party/opener/`.
- Le port Zephyr est isolé dans `app/src/protocols/eip/` : sockets `zsock`, horloges, mémoire et callbacks applicatifs.
- TCP/UDP 44818 : encapsulation EtherNet/IP, découverte, sessions et messaging explicite CIP.
- UDP 2222 : I/O implicites cycliques Class 1 via une connexion Exclusive Owner.
- Après un clone : `git submodule update --init --recursive`.

**Étape 6.2 — Objets CIP basiques**
- Objets standards OpENer : Identity (0x01), Assembly (0x04), TCP/IP Interface (0xF5), Ethernet Link (0xF6), Connection Manager et QoS.
- **Configuration Assembly 1** : assembly vide de 0 octet pour les outils qui exigent un chemin de configuration.
- **Output Assembly 100 (O→T)** : 20 octets écrits cycliquement par le PLC dans les 10 mots de la fenêtre scanner.
- **Input Assembly 101 (T→O)** : 20 octets, soit les 10 mots de la fenêtre scanner, lus par le PLC.
- Le mapping partagé garantit la cohérence entre EtherNet/IP, Modbus Unit-ID 2 et le webserver REST.
- Les échanges multi-octets des assemblies utilisent l'ordre little-endian CIP.

**Test :** `python tools/eip_probe.py <ip-carte>`, Wireshark (`enip || cip`), puis RSLinx, Studio 5000 ou EIPScanner pour ouvrir une connexion Class 1 sur 100/101.

**Livrable :** la carte est un device EtherNet/IP identifiable, répond au messaging explicite et échange 10 mots cycliques via les assemblies 100/101.

---

### Phase 7 — IPv6, identification et HTTP dual-stack

Objectif : ajouter IPv6 sans supprimer IPv4, centraliser l'identité de la carte et rendre le dashboard accessible sur les deux familles d'adresses.

**Étape 7.1 — Activer IPv6 dans Zephyr**
```kconfig
CONFIG_NET_IPV6=y
CONFIG_NET_IPV6_ND=y
CONFIG_NET_IPV6_DAD=y
CONFIG_NET_IPV6_MLD=y
```
- Zephyr génère automatiquement une adresse IPv6 link-local `fe80::/64` à partir de la MAC.
- Neighbor Discovery (ND) assure la résolution des voisins IPv6.
- Duplicate Address Detection (DAD) valide l'adresse avant son passage à l'état `preferred`.
- Multicast Listener Discovery (MLD) prépare l'interface pour les protocoles multicast IPv6.
- Test Windows : relever l'index Ethernet avec `netsh interface ipv6 show interfaces`, puis exécuter `ping -6 <ipv6-link-local>%<index>`.

**Étape 7.2 — Centraliser l'identification de la carte**
- Ajouter `app/src/core/ident.c` et `ident.h`.
- Centraliser le nom du device, le hostname, le fabricant, le modèle et la version firmware `MAJOR.MINOR.PATCH`.
- Lire l'identifiant matériel STM32 via `hwinfo_get_device_id()`.
- Exposer la MAC, l'IPv4 active, l'IPv6 link-local et un UUID stable dérivé de la MAC.
- Ajouter la commande shell `ident` pour afficher toutes ces informations.
- Conserver les identifiants propres aux protocoles dans leurs modules respectifs : numéro de série et Product Code CIP dans EtherNet/IP, signature et version du mapping dans Modbus.

**Étape 7.3 — Serveur HTTP dual-stack**
- Conserver le listener IPv4 `0.0.0.0:80`.
- Ajouter un listener IPv6 `[::]:80` avec `IPV6_V6ONLY`.
- Utiliser `zsock_poll()` dans l'unique thread web pour attendre les deux listeners sans dupliquer le traitement HTTP/REST.
- Valider l'accès IPv4 avec `http://192.168.0.3/`.
- Valider l'accès IPv6 link-local sous Windows avec :
  ```cmd
  curl.exe -g "http://[fe80::80:e1ff:fe4c:b2d7%25<index>]/"
  ```
- Une adresse link-local exige un identifiant de zone. Certains navigateurs Chromium refusent cette syntaxe ; `curl` permet de valider le serveur indépendamment de cette limitation.

**Livrable :** la carte répond au ping IPv4/IPv6, expose son identité via la commande `ident` et sert le même dashboard HTTP sur IPv4 et IPv6.

---

### Phase 8 — DPWS / WS-Discovery et métadonnées HTTP

Objectif : reproduire le cycle de découverte observé sur le drive Schneider :
un `Probe` WS-Discovery multicast, un `ProbeMatch` contenant le `XAddr`, puis
une requête HTTP WS-Transfer pour obtenir les informations détaillées du device.

**Étape 8.1 — WS-Discovery IPv6 et IPv4**
- Listener UDP 3702 sur les deux familles d'adresses.
- Rejoindre les groupes multicast standards :
  - IPv6 `ff02::c` sur l'interface Ethernet ;
  - IPv4 `239.255.255.250`.
- Attendre que l'adresse IPv6 link-local soit validée par DAD avant de rejoindre le groupe IPv6.
- Accepter les profils WS-Discovery 2005/04 et 2009/01.
- Répondre aux probes `wsdp:Device` avec un `ProbeMatch` unicast contenant :
  - l'EndpointReference `urn:uuid:<device-uuid>` ;
  - les types `wsdp:Device` et `IndustrialEthernetDevice` ;
  - les scopes du device ;
  - les XAddrs HTTP IPv6 et IPv4 ;
  - la version des métadonnées.

**Étape 8.2 — Métadonnées DPWS via WS-Transfer**
- Ajouter un endpoint HTTP `POST /dpws/<device-uuid>` sur le serveur dual-stack existant.
- Accepter l'action SOAP `http://schemas.xmlsoap.org/ws/2004/09/transfer/Get`.
- Retourner les sections DPWS `ThisModel`, `ThisDevice` et une section applicative.
- Publier les informations centralisées par `ident` : nom, fabricant, modèle,
  version FW, hardware ID, MAC, IPv4, IPv6 link-local et UUID.
- Ajouter les informations applicatives temporaires : ProductCode, gamme,
  capacités, emplacement, hostname, uptime et services supportés.

**Étape 8.3 — Client Python de validation**
- Ajouter `tools/dpws_probe.py`, sans dépendance Scapy.
- Envoyer le probe multicast IPv6 sur l'index Ethernet fourni.
- Vérifier la corrélation `MessageID` / `RelatesTo`.
- Extraire l'EndpointReference et les XAddrs du `ProbeMatch`.
- Effectuer automatiquement le `WS-Transfer Get`, en ajoutant le scope IPv6
  Windows pour les adresses link-local.
- Afficher les informations d'identification et les services du device.

**Test Windows :**
```powershell
netsh interface ipv6 show interfaces
python tools/dpws_probe.py --interface-index 11
```

**Test Wireshark :** `udp.port == 3702` pour la découverte, puis `http || xml`
pour les métadonnées SOAP.

**Livrable :** découverte automatique IPv6/IPv4 et récupération des métadonnées
DPWS de la carte, selon le même cycle que le drive Schneider.

---

### Phase 9 — Application Cortex-M4 et commande locale du moteur DC

Objectif : créer une application Zephyr M4 autonome qui pilote localement un petit moteur DC.
Le fonctionnement du moteur ne doit dépendre ni du réseau, ni du M7, ni de l'IPC.

**Étape 9.1 — Créer l'application M4 séparée**
- Ajouter `app_m4/` comme application Zephyr dédiée à la cible
  `stm32h747i_disco/stm32h747xx/m4`.
- Fournir son `CMakeLists.txt`, son `prj.conf`, son overlay carte et une procédure de build/flash.
- Préparer un build sysbuild dual-image afin de construire les applications M7 et M4 de façon
  reproductible, tout en gardant chaque image testable séparément.
- Définir explicitement la propriété des ressources :
  - M7 : Ethernet, LCD/tactile, LD1 verte et LD2 orange ;
  - M4 : moteur, boutons, potentiomètre, LD3 rouge et LD4 bleue.

**Étape 9.2 — Pilote moteur et L293D**
- Créer un module M4 `motor_control` indépendant des boutons et des LED.
- Piloter `EN1,2` du L293D par un PWM et `IN1`/`IN2` par deux GPIO de direction.
- Définir une machine d'états simple : `DISABLED`, `READY`, `RUNNING`, `STOPPING`, `FAULT`.
- Au boot, pendant un reset ou en cas d'erreur : PWM à 0 %, pont désactivé et moteur arrêté.
- Documenter les broches réellement retenues, la masse commune STM32/MB V2/L293D et
  l'alimentation moteur séparée fournie par la MB V2.

**Étape 9.3 — Commande par quatre boutons poussoir**
- Configurer quatre entrées GPIO avec pull-up et anti-rebond logiciel :
  `START`, `STOP`, `DIRECTION` et `RESET/MODE`.
- Donner la priorité absolue à `STOP` sur les autres commandes.
- N'autoriser un changement de direction qu'après retour du PWM à zéro.
- Traiter les boutons par événements ; ne pas relancer continuellement une commande tant
  qu'un bouton reste appuyé.

**Étape 9.4 — Retour d'état par LED**
- Utiliser LD4 bleue pour l'état `RUNNING` et LD3 rouge pour `FAULT`.
- Utiliser des motifs de clignotement documentés pour `READY`, `STOPPING` et défaut acquittable.
- Conserver les deux premières LED sous le contrôle du M7 pour le heartbeat et l'état réseau.

**Étape 9.5 — Consigne par potentiomètre**
- Ajouter dans un second incrément la lecture ADC du potentiomètre, limitée à `0..3,3 V`.
- Filtrer la mesure et ajouter une zone morte pour éviter les variations de PWM dues au bruit.
- Mapper la valeur ADC sur une consigne de rapport cyclique `0..1000` (0,0 à 100,0 %).
- Ajouter une rampe d'accélération/décélération et, si nécessaire, un seuil minimal de démarrage.
- Tant qu'aucun encodeur n'est ajouté, parler de **consigne PWM**, pas de vitesse en tr/min.

**Étape 9.6 — Robustesse et validation locale**
- Interdire tout redémarrage automatique du moteur après boot, reset ou défaut.
- Tester l'anti-rebond, l'arrêt prioritaire, l'inversion, les valeurs ADC min/max et les cycles
  d'alimentation.
- Surveiller l'échauffement du L293D et vérifier que le courant du moteur de démonstration reste
  compatible avec le composant et l'alimentation.

**Livrable :** une image M4 autonome pilote le moteur DC via L293D, boutons et potentiomètre,
avec un état clair sur LD3/LD4 et un démarrage systématiquement sûr.

---

### Phase 10 — IPC M7↔M4 et commande distante du moteur

Objectif : transformer le M7 en passerelle de communication industrielle, tout en laissant au M4
la propriété exclusive du matériel moteur et des décisions temps réel.

**Étape 10.1 — Canal inter-cœur**
- Utiliser **Zephyr IPC Service avec le backend `icmsg`**, déjà supporté par les overlays
  STM32H747I-DISCO M7 et M4 de Zephyr 4.4.
- Réserver la SRAM partagée et les mailboxes dans les devicetrees des deux images.
- Créer un endpoint `motor-control` et valider d'abord un échange ping/pong bidirectionnel.
- Laisser le backend gérer la synchronisation et la cohérence de la mémoire partagée ; ne pas
  partager directement une structure C brute entre les caches M7 et M4.

**Étape 10.2 — Contrat de données versionné**
- Définir des messages de taille fixe avec `magic`, version, taille et numéro de séquence.
- Séparer impérativement les commandes M7→M4 des états M4→M7.
- Image de commande proposée, 10 mots de 16 bits :
  - mot 0 : enable, run, direction, reset fault, quick stop ;
  - mot 1 : consigne PWM `0..1000` ;
  - mots 2/3 : rampes d'accélération/décélération ;
  - mot 4 : mode et source de commande ;
  - mot 5 : timeout de communication ;
  - mots 6..8 : réservés ; mot 9 : séquence de commande.
- Image d'état proposée, 10 mots de 16 bits :
  - mot 0 : ready, running, stopping, fault, local, remote ;
  - mot 1 : PWM réellement appliqué ; mot 2 : direction ; mot 3 : code défaut ;
  - mot 4 : état des boutons ; mot 5 : valeur potentiomètre filtrée ;
  - mots 6/7 : âge de commande et temps de boucle ;
  - mot 8 : heartbeat M4 ; mot 9 : séquence d'état.

**Étape 10.3 — Modes local et distant**
- En mode local, les boutons et le potentiomètre pilotent le moteur directement sur le M4.
- En mode distant, le M4 applique les commandes reçues du M7 après validation des bornes.
- Le bouton physique `STOP` reste actif et prioritaire dans tous les modes.
- En cas de perte IPC ou de commande trop ancienne, appliquer une décélération contrôlée puis
  passer dans un état sûr ; ne jamais conserver indéfiniment la dernière consigne.
- Publier la source propriétaire courante afin que Web, Modbus et EtherNet/IP expliquent pourquoi
  une commande est acceptée ou refusée.

**Étape 10.4 — Exposition Modbus, EtherNet/IP, Web et LCD**
- Modbus : réserver deux zones distinctes pour la commande et l'état moteur. Incrémenter
  `APP_MODBUS_MAP_VERSION` puisque le contrat public évolue, sans changer sa signature `0x0747`.
- EtherNet/IP : utiliser l'Assembly 100 comme sortie PLC/commande et l'Assembly 101 comme
  entrée PLC/état ; supprimer leur miroir actuel sur la même fenêtre de dix mots.
- Web/REST : ajouter les endpoints état/commande et une vue moteur avec mode, consigne,
  PWM appliqué, direction et défaut.
- LCD M7 : ajouter une synthèse en lecture seule et les commandes autorisées par l'arbitrage.

**Étape 10.5 — Tests d'intégration et de défaut**
- Tester séparément les commandes Web, Modbus et EtherNet/IP, puis leur concurrence.
- Tester la priorité du mode local, le bouton `STOP`, les messages invalides et les séquences
  anciennes.
- Redémarrer chaque cœur séparément et débrancher Ethernet pendant que le moteur tourne.
- Vérifier que la perte M7/IPC/réseau conduit toujours le M4 vers l'état sûr défini.

**Livrable :** un PLC ou le dashboard commande le moteur via le M7 ; le M4 conserve le contrôle
temps réel, renvoie son état et arrête le moteur de manière déterministe en cas de défaut.

---

### Phase 11 — Sécurité : Login & HTTPS

**Étape 11.1 — Login management**
- Subsys **settings** pour stocker user/password hashé (SHA-256 + salt)
- Endpoint `POST /api/login` → renvoie un token (UUID random via TRNG HW)
- Middleware HTTP : routes mutantes REST (`PUT /api/registers/<id>`, `PUT /api/scanner/<id>`, `POST /api/commands/*`) exigent header `Authorization: Bearer <token>`
- Tokens en RAM avec expiration (timer Zephyr)

**Étape 11.2 — Changement mot de passe**
- Endpoint `POST /api/password` (auth requise)
- Validation force (longueur min, charset)
- Première connexion → forcer le changement (comme ATV)

**Étape 11.3 — HTTPS**
```kconfig
CONFIG_MBEDTLS=y
CONFIG_MBEDTLS_TLS_VERSION_1_2=y
CONFIG_NET_SOCKETS_SOCKOPT_TLS=y
CONFIG_HTTP_SERVER_TLS=y
```
- Générer certificat auto-signé (script `openssl` côté host) — clé ECC P-256
- Embarquer certif + clé dans une partition flash dédiée
- Migrer le serveur HTTP sur port 443 (ou keep 80 + 443)

**Livrable :** login obligatoire, HTTPS fonctionnel (certif self-signed accepté manuellement).

---

### Phase 12 — Firmware Update

**Étape 12.1 — MCUboot bootloader**
- Activer dans Zephyr : `west build` avec `-DCONFIG_BOOTLOADER_MCUBOOT=y`
- Configurer le support MCUboot multi-image pour les firmwares M7 et M4.
- Partition layout :
  - `boot_partition` ;
  - une paire slot actif/candidat pour l'image M7 ;
  - une paire slot actif/candidat pour l'image M4 ;
  - `scratch_partition` si le mode de swap retenu l'exige ;
  - `storage_partition` (LittleFS — settings, certs, www)
- Définir une matrice de compatibilité entre la version du contrat IPC et les versions M7/M4.

**Étape 12.2 — Endpoint upload**
- `POST /api/firmware` (multipart ou bundle M7/M4)
- Auth requise + check size
- Écriture progressive des images dans leurs slots respectifs via API `flash_img_*` de Zephyr
- À la fin : `boot_request_upgrade()` puis `sys_reboot()`

**Étape 12.3 — UI**
- Page web "Update" : sélection du bundle signé, progress bar via SSE ou polling
- Affichage séparé des versions M7, M4 et du contrat IPC

**Étape 12.4 — Rollback automatique**
- Le M7 ne confirme la mise à jour qu'après validation de sa propre santé, du démarrage M4 et de
  la compatibilité IPC ; sinon les images reviennent à la paire précédente.

**Livrable :** upload coordonné des firmwares M7/M4 par le web, reboot et rollback cohérent si échec.

---

### Phase 13 — Signature Check (image signing)

**Étape 13.1 — Génération de clés**
- `imgtool keygen -k ecdsa-p256-priv.pem -t ecdsa-p256` (script host)
- Clé publique injectée dans MCUboot (`MCUBOOT_SIGNATURE_KEY_FILE`)

**Étape 13.2 — Signature build**
- Le build sysbuild produit les images M7 et M4 non signées.
- Signer chaque image avec `imgtool` et générer un manifeste de bundle contenant leurs versions,
  hashes et la version du contrat IPC.
- Intégration dans CMake post-build pour automatiser

**Étape 13.3 — Vérif côté MCU**
- MCUboot vérifie la signature ECDSA de chaque image au boot via mbedTLS
- Image non signée / mauvaise clé → refus de boot, fallback slot précédent

**Livrable :** seule une image signée par la clé privée légitime peut s'installer.

---

### Phase 14 — Secure Boot (chain of trust complète)

**Étape 14.1 — RDP / PCROP STM32**
- Activer Read-Out Protection niveau 1 ou 2 sur STM32H7 (option bytes)
- Empêche le dump de la flash via SWD

**Étape 14.2 — Root of Trust**
- Clé publique MCUboot stockée en **OTP** (option bytes / Flash protégée)
- Hash de MCUboot vérifié au démarrage par le ROM bootloader STM32 (RSS — Root Secure Service)

**Étape 14.3 — Chain**
1. ROM ST → vérifie MCUboot (RSS / option SBSFU)
2. MCUboot → vérifie les applications M7 et M4 signées
3. Applications → vérifient leur compatibilité IPC et le filesystem (signature des assets www optionnel)

**Étape 14.4 — Encryption optionnelle**
- MCUboot supporte image chiffrée AES-CTR avec clé dérivée ECIES
- À activer si la démo vise IP protection

**Livrable :** chaîne complète ROM → MCUboot → App, aucun code non signé n'exécute.

---

### Phase 15 — Polish & Démo finale

**Étape 15.1 — Logs structurés**
- `CONFIG_LOG=y` + backend UART
- Niveau par module : net, modbus, http, security

**Étape 15.2 — Diagnostics**
- Endpoint `GET /api/diag` : uptime, RAM libre, stats ETH (RX/TX/erreurs), connexions actives, version FW, hash MCUboot
- Endpoint `GET /api/reboot` (auth) → soft reset

**Étape 15.3 — mDNS / Bonjour**
- `CONFIG_DNS_SD=y` + `CONFIG_MDNS_RESPONDER=y`
- Hostname `industrial-ethernet.local` accessible sans IP

**Étape 15.4 — Doc + démo scénario**
- README : schéma archi, mapping registres, comment tester chaque protocole
- Vidéo de démo : DHCP → web + LCD → Modbus → upload firmware signé → rollback test

---

## Récap timeline (indicatif, ordre logique)

```
Phase 0  : Setup           ────►
Phase 1  : Réseau base      ────►
Phase 2  : Modbus server    ────►
Phase 3  : Modbus scanner   ────►
Phase 4  : Webserver        ────►
Phase 5  : LCD dashboard M7 ────►   ◄── séparé du webserver
Phase 6  : EtherNet/IP      ────►   ◄── peut être skippé si trop ambitieux
Phase 7  : IPv6 + identité  ────►
Phase 8  : DPWS             ────►   ◄── optionnel
Phase 9  : M4 + moteur local ────►   ◄── autonome, sans dépendance réseau
Phase 10 : IPC + commande   ────►   ◄── dépend de la phase 9
Phase 11 : Auth + HTTPS     ────►
Phase 12 : FW Update        ────►
Phase 13 : Signature        ────►   (dépend phase 12)
Phase 14 : Secure Boot      ────►   (couronne finale)
Phase 15 : Polish + démo    ────►
```

## Stack technique finale

| Brique | Choix |
|---|---|
| Board | **STM32H747I-DISCO** (Discovery kit) |
| MCU | STM32H747XIH6 — Cortex-M7 480 MHz + Cortex-M4 240 MHz |
| Mémoire | 1 MB SRAM + 32 MB SDRAM + 2 MB Flash + 128 MB QSPI |
| RTOS | Zephyr 3.7 LTS ou 4.x (M7 : réseau, M4 : temps réel) |
| Inter-core | Zephyr IPC Service / `icmsg` (SRAM partagée + mailbox) |
| TCP/IP | Zephyr native (BSD sockets) |
| Bootloader | MCUboot (dual-slot sur QSPI) |
| Crypto | mbedTLS + accélérateur HW STM32 (AES/HASH/PKA) |
| Filesystem | LittleFS (partition QSPI) |
| Modbus | `subsys/modbus` Zephyr |
| EtherNet/IP | OpENer (port Zephyr) |
| Webserver | `subsys/net/lib/http/server` (v2) |
| DPWS | implémentation custom ~200 LOC |
| Contrôle moteur | Application Zephyr séparée sur Cortex-M4 + PWM/GPIO/ADC |
| Driver moteur | L293D, alimentation moteur externe et masse commune |
| LCD Dashboard | LVGL sur Cortex-M7 (écran 4" tactile) |
| Frontend web | HTML + JS vanilla (≤ 50 KB) |
| Build | West + CMake + imgtool |

Prochaine étape : réaliser la phase 9 sans coupler prématurément le moteur au réseau :
- conserver `app/` pour l'image M7 existante : réseau, protocoles, Web et LCD ;
- créer `app_m4/` pour la machine d'états moteur, PWM/GPIO, boutons, ADC et LD3/LD4 ;
- documenter le câblage L293D/MB V2 et choisir les broches sans conflit entre les deux cœurs ;
- valider l'application M4 seule avec une consigne PWM fixe, puis ajouter le potentiomètre ;
- préparer sysbuild, puis ajouter IPC Service / `icmsg` seulement en phase 10.


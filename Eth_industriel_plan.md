# Plan Démo : Ethernet Industriel sous Zephyr

## Chapitre 1 — Objectif & Résumé du plan

### 1.1 — But du projet

Ce plan décrit la réalisation d'un **prototype embarqué industriel** tournant sous **Zephyr RTOS**, dont la vocation est de démontrer, sur une carte de développement dual-core (~€85), les mêmes capacités protocolaires et sécuritaires que des cartes de communication industrielles professionnelles.

Le projet vise deux objectifs complémentaires :

1. **Valider la faisabilité d'une stack industrielle complète sous Zephyr** : TCP/IP, Modbus TCP (server + scanner via Unit-ID 2), EtherNet/IP, DPWS/WS-Discovery, DHCP, mDNS, HTTPS, gestion de login.
2. **Démontrer un niveau de sécurité proche du monde industriel** : firmware signé, secure boot avec chaîne de confiance, protection du code en flash.

Le résultat attendu est un **device réseau découvrable automatiquement**, exposant :
- un **webserver HTTPS** avec interface de supervision et d'administration (registres, diagnostics, FW update),
- un **serveur Modbus TCP** (port 502) avec Unit-ID 1 pour les registres principaux et Unit-ID 2 pour la zone scanner,
- un **device EtherNet/IP** identifiable par un automate Rockwell ou un outil EIPScan,
- un **endpoint WS-Discovery** (DPWS, port UDP 3702) permettant la découverte automatique sur le réseau local.

---

### 1.2 — Résumé du plan

Le plan est structuré en **11 phases progressives**, chacune apportant une couche fonctionnelle ou sécuritaire supplémentaire. Les phases sont conçues pour être validées indépendamment, chaque livrable étant testable avant de passer à la suivante.

| Phase | Thème | Livrable clé |
|---|---|---|
| **Phase 0** | Setup toolchain (Zephyr SDK, West, ST-Link) | `west flash` fonctionne sur STM32H747I-DISCO |
| **Phase 1** | Connectivité réseau (Ethernet, DHCP, ping) | Carte joignable sur le réseau via DHCP |
| **Phase 2** | Modbus TCP Server (FC03/04/06/16/23) | Lecture/écriture Modbus depuis QModMaster |
| **Phase 3** | Modbus Scanner via Unit-ID 2 | Zone 10 registres pilotable par un device externe |
| **Phase 4** | Webserver HTTP + frontend (registres) | Page web affichant et modifiant des registres |
| **Phase 5** | EtherNet/IP via OpENer | Device CIP identifiable par RSLinx / EIPScan |
| **Phase 6** | DPWS / WS-Discovery (UDP multicast 3702) | Découverte auto depuis WSDiscoveryTool |
| **Phase 7** | Sécurité HTTP : login, tokens, HTTPS/TLS | Interface protégée par login + TLS (certif ECC P-256) |
| **Phase 8** | Firmware Update + rollback MCUboot | Upload d'un `.bin` signé via le web, rollback auto |
| **Phase 9** | Signature ECDSA des images | Seule une image signée par la clé légitime s'installe |
| **Phase 10** | Secure Boot (RDP, OTP, chain of trust) | Chaîne ROM → MCUboot → App inviolable |
| **Phase 11** | Polish : logs, diagnostics, mDNS, doc | Démo finale complète + hostname `industrial-ethernet.local` |


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
│  ├── mbedTLS (TLS handshake, signature)                │
│  └── MCUboot (firmware update + rollback)              │
├─────────────────────────────────────────────────────────┤
│  Cortex-M4 @ 240 MHz (esclave, via OpenAMP/mailbox)     │
│  ├── Simulation "drive" temps réel                     │
│  ├── Génération de données registres (sinusoïde, PWM)  │
│  ├── Affichage LCD dashboard (LVGL)                    │
│  └── Watchdog / monitoring indépendant                 │
└─────────────────────────────────────────────────────────┘
         ▲▼ Shared SRAM (OpenAMP / RPMsg mailbox)
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
  - `registers[50..59]` : mapping des 10 registres scanner.
  - `registers[50 + i]` contient directement l'adresse du holding register exposé par `scanner_regs[i]`.
  - `0xFFFF` = RAM locale, avec lecture/écriture dans une valeur propre à `scanner_regs[i]`.
- Initialiser le mapping par défaut au boot :
  - `registers[50] = 1` → `scanner_regs[0]` expose `registers[1]`, donc le mode IP.
  - `registers[51] = 2` → `scanner_regs[1]` expose `registers[2]`, donc l'adresse IP mot haut.
  - `registers[52] = 3` → `scanner_regs[2]` expose `registers[3]`, donc l'adresse IP mot bas.
  - `registers[53..59] = 0xFFFF` → `scanner_regs[3..9]` utilisent leur RAM locale.
- Le device externe peut modifier ce mapping en runtime en écrivant dans `registers[50..59]` via Unit-ID 1.

**Étape 3.3 — Fenêtre scanner dynamique**
- `scanner_regs[0..9]` n'est pas une copie fixe : c'est une fenêtre dynamique pilotée par `registers[50..59]`.
- Quand un device externe lit `scanner_regs[i]` via Unit-ID 2, la carte retourne la valeur interne indiquée par `registers[50 + i]`.
- Quand un device externe écrit `scanner_regs[i]` via Unit-ID 2, la carte écrit dans la donnée interne indiquée par `registers[50 + i]`.
- Si `registers[50 + i] = 0xFFFF`, `scanner_regs[i]` lit/écrit une valeur RAM locale.
- Exemple :
  - au boot : `registers[50] = 1`, donc `scanner_regs[0]` lit/écrit le mode IP.
  - si le device externe écrit ensuite `registers[50] = 4` et `registers[51] = 5`, alors `scanner_regs[0]` et `scanner_regs[1]` exposent le masque réseau.
  - une écriture dans `scanner_regs[0]` met alors à jour le mot haut du masque, pas le mode IP.
- Le device externe peut lire/écrire périodiquement les adresses `0..9` via Unit-ID 2 sans connaître les registres internes réels.

Exemple : pour `192.168.1.45`, le mapping IP par défaut donne `scanner_regs[1] = 0xC0A8` et `scanner_regs[2] = 0x012D`.

**Étape 3.4 — Test avec device externe**
- Depuis un PC ou une PLC, lire `registers[50..59]` via Unit-ID 1 pour vérifier le mapping scanner courant.
- Modifier `registers[50..59]` via Unit-ID 1 pour changer ce que Unit-ID 2 expose.
- Lire/écrire ensuite Unit-ID 2, adresses `0..9`, pour accéder aux valeurs mappées.
- Vérifier que les lectures/écritures périodiques sur Unit-ID 2 mettent à jour les diagnostics.

Exemples de tests avec `modpoll` :
```bash
# Lire le mapping scanner via Unit-ID 1 : registers[50..59]
modpoll -m tcp -a 1 -r 51 -c 10 <IP>

# Lire les 10 registres scanner via Unit-ID 2
modpoll -m tcp -a 2 -r 1 -c 10 <IP>

# Remapper scanner_regs[0..1] vers le masque réseau MSW/LSW
modpoll -m tcp -a 1 -r 51 -c 2 <IP> 4 5

# Écrire dans scanner_regs[0] : met à jour le mot haut du masque réseau
modpoll -m tcp -a 2 -r 1 <IP> 65535
```

**Livrable :** serveur Modbus TCP multi Unit-ID : Unit-ID 1 configure le mapping scanner via `registers[50..59]`, Unit-ID 2 expose une fenêtre `scanner_regs[0..9]` lisible/écrivable périodiquement selon ce mapping.

---

### Phase 4 — Webserver HTTP (statique puis dynamique)

**Étape 4.1 — HTTP server v2 (Zephyr ≥ 3.5)**
```kconfig
CONFIG_HTTP_SERVER=y
CONFIG_HTTP_PARSER=y
CONFIG_NET_SOCKETS=y
CONFIG_FILE_SYSTEM=y
CONFIG_FILE_SYSTEM_LITTLEFS=y
```
- Page d'accueil statique servie depuis LittleFS (partition flash dédiée)
- Routes initiales : `GET /` (HTML), `GET /api/status` (JSON)

**Étape 4.2 — Pages dynamiques**
- Endpoint `GET /api/registers` → renvoie le tableau de registres du serveur Modbus
- Endpoint `POST /api/registers/<id>` → écrit une valeur
- Endpoint `GET /api/scanner` → JSON du mapping `registers[50..59]`, de la fenêtre Unit-ID 2 et des diagnostics
- La page web devient une vue sur les briques déjà en place, plutôt qu'une source de vérité

**Étape 4.3 — Frontend minimal**
- HTML + Vanilla JS (pas de framework lourd, on tient en <50 KB)
- 1 page : tableau de registres, refresh auto (fetch toutes les 1s), inputs pour écriture
- 1 vue scanner : mapping `registers[50..59]`, fenêtre Unit-ID 2, période observée et diagnostics
- Stylé minimaliste (CSS inline)

**Étape 4.4 — Dashboard LCD (Cortex-M4)**
- Image M4 séparée : `west build -b stm32h747i_disco/stm32h747xx/m4`
- LVGL affiche sur le LCD 4" : IP courante, registres en barre-graphe, statut Modbus/EIP
- Communication M7→M4 via OpenAMP (RPMsg) : le M7 pousse les données registres au M4
- Bonus tactile : bouton "reboot" / switch dark mode sur l'écran

**Livrable :** ouvrir `http://<IP>/` dans un navigateur, voir et modifier des registres, et visualiser le scanner.

---

### Phase 5 — EtherNet/IP (CIP)

**Étape 5.1 — Stack EIP**
- Zephyr n'a **pas** de stack EIP officielle → 2 options :
  - **A)** Intégrer **OpENer** (Apache 2.0) — stack EIP open source légère
  - **B)** Implémenter minimalement l'encapsulation : List Identity (UDP 44818) + Register Session + ListServices
- **Choix recommandé** : OpENer compilé en lib externe, glue avec sockets Zephyr

**Étape 5.2 — Objets CIP basiques**
- Identity Object (0x01), TCP/IP Object (0xF5), Ethernet Link (0xF6)
- Assembly Object pour mapper les registres en I/O implicite (Class1)

**Test :** RSLinx ou EIPScan PC scanne et identifie la carte.

**Livrable :** la carte répond aux requêtes EIP (List Identity, Get Attributes Single).

---

### Phase 6 — DPWS / WS-Discovery (optionnel mais cool)

**Étape 6.1 — WS-Discovery minimaliste**
- Pas de stack DPWS complète dans Zephyr → implémenter manuellement :
  - Listener UDP multicast 239.255.255.250:3702
  - Répondre aux `<Probe>` SOAP avec un `<ProbeMatch>` contenant l'URL HTTP
- ~200 lignes de C avec template XML statique

**Test :** outil **WSDiscoveryTool** (.NET) sur le PC → la carte apparaît dans la liste des devices découverts → clic ouvre la page web.

**Livrable :** discovery auto sur le réseau (comme un drive ATV).

---

### Phase 7 — Sécurité : Login & HTTPS

**Étape 7.1 — Login management**
- Subsys **settings** pour stocker user/password hashé (SHA-256 + salt)
- Endpoint `POST /api/login` → renvoie un token (UUID random via TRNG HW)
- Middleware HTTP : routes `/api/registers POST` exigent header `Authorization: Bearer <token>`
- Tokens en RAM avec expiration (timer Zephyr)

**Étape 7.2 — Changement mot de passe**
- Endpoint `POST /api/password` (auth requise)
- Validation force (longueur min, charset)
- Première connexion → forcer le changement (comme ATV)

**Étape 7.3 — HTTPS**
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

### Phase 8 — Firmware Update

**Étape 8.1 — MCUboot bootloader**
- Activer dans Zephyr : `west build` avec `-DCONFIG_BOOTLOADER_MCUBOOT=y`
- Partition layout :
  - `boot_partition` (MCUboot, ~64 KB)
  - `slot0_partition` (image active)
  - `slot1_partition` (image candidate)
  - `scratch_partition`
  - `storage_partition` (LittleFS — settings, certs, www)

**Étape 8.2 — Endpoint upload**
- `POST /api/firmware` (multipart ou raw binary)
- Auth requise + check size
- Écriture progressive en flash via API `flash_img_*` de Zephyr
- À la fin : `boot_request_upgrade()` puis `sys_reboot()`

**Étape 8.3 — UI**
- Page web "Update" : sélecteur de fichier `.bin` signé, progress bar via SSE ou polling
- Affichage version actuelle (compilée dans le binaire via `APP_VERSION`)

**Étape 8.4 — Rollback automatique**
- MCUboot teste l'image — si l'app ne valide pas (`boot_write_img_confirmed()`) dans X secondes, revert au slot précédent

**Livrable :** upload de firmware par le web, reboot, rollback si échec.

---

### Phase 9 — Signature Check (image signing)

**Étape 9.1 — Génération de clés**
- `imgtool keygen -k ecdsa-p256-priv.pem -t ecdsa-p256` (script host)
- Clé publique injectée dans MCUboot (`MCUBOOT_SIGNATURE_KEY_FILE`)

**Étape 9.2 — Signature build**
- `west build` produit un `.bin` non signé
- `imgtool sign --key ecdsa-p256-priv.pem --version 1.0.0 --header-size 0x200 --slot-size 0x80000 zephyr.bin signed.bin`
- Intégration dans CMake post-build pour automatiser

**Étape 9.3 — Vérif côté MCU**
- MCUboot vérifie la signature ECDSA au boot via mbedTLS
- Image non signée / mauvaise clé → refus de boot, fallback slot précédent

**Livrable :** seule une image signée par la clé privée légitime peut s'installer.

---

### Phase 10 — Secure Boot (chain of trust complète)

**Étape 10.1 — RDP / PCROP STM32**
- Activer Read-Out Protection niveau 1 ou 2 sur STM32H7 (option bytes)
- Empêche le dump de la flash via SWD

**Étape 10.2 — Root of Trust**
- Clé publique MCUboot stockée en **OTP** (option bytes / Flash protégée)
- Hash de MCUboot vérifié au démarrage par le ROM bootloader STM32 (RSS — Root Secure Service)

**Étape 10.3 — Chain**
1. ROM ST → vérifie MCUboot (RSS / option SBSFU)
2. MCUboot → vérifie l'app signée
3. App → vérifie le filesystem (signature des assets www optionnel)

**Étape 10.4 — Encryption optionnelle**
- MCUboot supporte image chiffrée AES-CTR avec clé dérivée ECIES
- À activer si la démo vise IP protection

**Livrable :** chaîne complète ROM → MCUboot → App, aucun code non signé n'exécute.

---

### Phase 11 — Polish & Démo finale

**Étape 11.1 — Logs structurés**
- `CONFIG_LOG=y` + backend UART
- Niveau par module : net, modbus, http, security

**Étape 11.2 — Diagnostics**
- Endpoint `GET /api/diag` : uptime, RAM libre, stats ETH (RX/TX/erreurs), connexions actives, version FW, hash MCUboot
- Endpoint `GET /api/reboot` (auth) → soft reset

**Étape 11.3 — mDNS / Bonjour**
- `CONFIG_DNS_SD=y` + `CONFIG_MDNS_RESPONDER=y`
- Hostname `industrial-ethernet.local` accessible sans IP

**Étape 11.4 — Doc + démo scénario**
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
Phase 5  : EtherNet/IP      ────►   ◄── peut être skippé si trop ambitieux
Phase 6  : DPWS             ────►   ◄── optionnel
Phase 7  : Auth + HTTPS     ────►
Phase 8  : FW Update        ────►
Phase 9  : Signature        ────►   (dépend phase 8)
Phase 10 : Secure Boot      ────►   (couronne finale)
Phase 11 : Polish + démo    ────►
```

## Stack technique finale

| Brique | Choix |
|---|---|
| Board | **STM32H747I-DISCO** (Discovery kit) |
| MCU | STM32H747XIH6 — Cortex-M7 480 MHz + Cortex-M4 240 MHz |
| Mémoire | 1 MB SRAM + 32 MB SDRAM + 2 MB Flash + 128 MB QSPI |
| RTOS | Zephyr 3.7 LTS ou 4.x (M7 : réseau, M4 : temps réel) |
| Inter-core | OpenAMP / RPMsg (shared SRAM) |
| TCP/IP | Zephyr native (BSD sockets) |
| Bootloader | MCUboot (dual-slot sur QSPI) |
| Crypto | mbedTLS + accélérateur HW STM32 (AES/HASH/PKA) |
| Filesystem | LittleFS (partition QSPI) |
| Modbus | `subsys/modbus` Zephyr |
| EtherNet/IP | OpENer (port Zephyr) |
| Webserver | `subsys/net/lib/http/server` (v2) |
| DPWS | implémentation custom ~200 LOC |
| LCD Dashboard | LVGL sur Cortex-M4 (écran 4" tactile) |
| Frontend web | HTML + JS vanilla (≤ 50 KB) |
| Build | West + CMake + imgtool |

Prochaine étape : générer le squelette du projet Zephyr pour la STM32H747I-DISCO :
- `prj.conf` (M7 : réseau) + `prj_m4.conf` (M4 : LCD)
- `CMakeLists.txt` (sysbuild dual-image)
- `west.yml` (manifest)
- DTS overlay (partition layout QSPI + SDRAM + LCD)
- `main.c` M7 : DHCP + Modbus server
- `main.c` M4 : LVGL dashboard + OpenAMP listener


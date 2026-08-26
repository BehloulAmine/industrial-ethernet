# Industrial Ethernet — Zephyr sur STM32H747I-DISCO

Prototype de carte de communication industrielle basé sur la
**STM32H747I-DISCO** et Zephyr 4.4. Le Cortex-M7 porte le réseau et les protocoles
industriels ; le Cortex-M4 pilote localement un moteur DC.

Fonctionnalités principales :

- Ethernet IPv4/IPv6 avec DHCP ou configuration IPv4 statique persistante ;
- serveur HTTP dual-stack, interface web et dashboard LVGL optionnel ;
- Modbus TCP serveur et scanner ;
- EtherNet/IP avec OpENer ;
- découverte DPWS/WS-Discovery et métadonnées HTTP ;
- contrôle moteur M4 par PWM, L293D et boutons ;
- sécurité, mise à jour firmware et secure boot planifiés dans la roadmap.

## Documentation

Pour comprendre le firmware avant d'intervenir, commencer par
[`app_m7/src/ARCHITECTURE.md`](app_m7/src/ARCHITECTURE.md). Ce document présente
les threads, les stacks, les priorités et les contrats partagés, puis renvoie
vers un fichier `DES04_<Fonction>.md` pour chaque fonctionnalité.

- [`app_m4/src/DES04_MotorControl.md`](app_m4/src/DES04_MotorControl.md) : architecture du contrôle moteur M4 ;
- [`Eth_industriel_plan.md`](Eth_industriel_plan.md) : roadmap fonctionnelle de la démonstration ;
- [`Devicetree_memory.md`](Devicetree_memory.md) : Devicetree, mémoire, partitions Flash et stockage QSPI.

## État actuel

### Cortex-M7

- heartbeat et logs UART ;
- IPv4 DHCP/statique et IPv6 link-local ;
- shell UART : `ip show`, `ip dhcp`, `ip static`, `ident`, `uptime`, `reboot` ;
- HTTP IPv4/IPv6 sur le port 80 ;
- DPWS/WS-Discovery sur UDP 3702, IPv4 `239.255.255.250` et IPv6 `ff02::c` ;
- métadonnées DPWS : identité, firmware, MAC, IPv4, IPv6 et UUID ;
- Modbus TCP sur le port 502, Unit-ID 1 et fenêtre scanner Unit-ID 2 ;
- EtherNet/IP OpENer sur TCP/UDP 44818 et UDP 2222 ;
- assemblies Class 1 : Config 1, Output 100 et Input 101 ;
- stockage persistant `settings` via NVS sur la QSPI externe ;
- dashboard LCD/LVGL optionnel avec le shield MB1166.

### Cortex-M4

- commande locale d'un moteur DC avec un L293D ;
- boutons START, STOP, DIRECTION et RESET ;
- PWM TIM13_CH1 sur PF8 ;
- potentiomètre ADC sur PA4, consigne filtrée de 80 à 100 % ;
- boost de démarrage, rampes et états READY/RUNNING/STOPPING/FAULT ;
- communication M7/M4 prévue dans la phase suivante.

## Structure du projet

```text
industrial-ethernet/
├── app_m7/                    Application Cortex-M7
│   ├── boards/                Overlays Devicetree M7
│   ├── src/core/              Démarrage et identification
│   ├── src/net/               Configuration réseau
│   ├── src/protocols/         Modbus, EtherNet/IP et DPWS
│   ├── src/web/               HTTP, REST et assets web
│   ├── src/ui/                Dashboard LCD/LVGL
│   └── src/shell/             Commandes UART
├── app_m4/                    Application de contrôle moteur Cortex-M4
│   ├── boards/                Overlay STMod+ et PWM
│   └── src/                   Boutons, machine d'état et moteur
├── tools/                     Scripts de validation PC
├── patches/                   Correctifs Zephyr spécifiques à Windows
├── west.yml                   Manifest Zephyr 4.4
├── Eth_industriel_plan.md     Roadmap
└── Devicetree_memory.md       Notes Devicetree et mémoire
```

## Installation et setup — Windows ou WSL Ubuntu

Les étapes Zephyr sont communes aux deux environnements. Seules l'installation
des outils système, l'activation du virtualenv et l'accès USB diffèrent.

### 1. Installer les outils système

#### Windows

Dans PowerShell :

```powershell
winget install Kitware.CMake Ninja-build.Ninja oss-winget.gperf Python.Python.3.12 Git.Git oss-winget.dtc wget 7zip.7zip
```

Vérifier que Python 3.12 et 7-Zip sont accessibles :

```powershell
py -3.12 --version
7z
```

#### WSL Ubuntu

```bash
sudo apt update
sudo apt install --no-install-recommends git cmake ninja-build gperf ccache \
  dfu-util device-tree-compiler wget python3-dev python3-venv python3-tk \
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1 \
  minicom usbutils
```

Construire dans le système de fichiers Linux, par exemple `~/work`, est
généralement plus rapide que sous `/mnt/c`.

### 2. Cloner le dépôt et créer le virtualenv

Commandes communes :

```bash
git clone https://github.com/behloulmedamine/industrial-ethernet.git
cd industrial-ethernet
git submodule update --init --recursive
```

Créer et activer le virtualenv selon le terminal utilisé :

```powershell
# Windows PowerShell
py -3.12 -m venv .venv
.\.venv\Scripts\Activate.ps1
```

```bash
# Windows Git Bash
py -3.12 -m venv .venv
source .venv/Scripts/activate
```

```bash
# WSL Ubuntu
python3 -m venv .venv
source .venv/bin/activate
```

### 3. Initialiser le workspace Zephyr

Dans le virtualenv actif :

```bash
python -m pip install --upgrade pip
pip install west
west init -l .
west update
west zephyr-export
west packages pip --install
west sdk install
```

`west update` télécharge plusieurs gigaoctets et peut durer plusieurs dizaines
de minutes. Le SDK est installé en dehors du dépôt.

Sous Windows, installer également Ninja dans le virtualenv si IAR ou un autre
outil place son propre `ninja.exe` en tête du `PATH` :

```powershell
pip install ninja
```

### 4. Correctif CMake Windows

Cette étape ne concerne pas WSL. Si le build Windows échoue sur la gestion des
backslashes de `-fmacro-prefix-map`, appliquer depuis la racine du dépôt :

```bash
git -C ../zephyr apply ../industrial-ethernet/patches/zephyr-cmake-backslash.patch
```

Le patch se trouve dans le dépôt Zephyr externe et peut être perdu après un
`west update`. Ne pas l'appliquer sous WSL.

## Construire les firmwares

Les commandes suivantes sont identiques dans PowerShell, Git Bash et WSL.

### M7 sans LCD

```bash
west build -p always -d build_m7_no_lcd -b stm32h747i_disco/stm32h747xx/m7 app_m7
```

### M7 avec LCD MB1166

```bash
west build -p always -d build_m7 -b stm32h747i_disco/stm32h747xx/m7 app_m7 -- -DSHIELD=st_b_lcd40_dsi1_mb1166 -DEXTRA_CONF_FILE=lcd.conf
```

Si la dalle porte la révision A09, remplacer le shield par
`st_b_lcd40_dsi1_mb1166_a09`.

### M7 avec diagnostics réseau

```bash
west build -p always -d build_m7_diag -b stm32h747i_disco/stm32h747xx/m7 app_m7 -- -DSHIELD=st_b_lcd40_dsi1_mb1166 '-DEXTRA_CONF_FILE=lcd.conf;diagnostics.conf'
```

`diagnostics.conf` ajoute les commandes UART `net conn`, `net mem` et
`net stats`. Il est destiné au développement et augmente la taille du firmware.

### M4 moteur

```bash
west build -p always -d build_m4 -b stm32h747i_disco/stm32h747xx/m4 app_m4
```

Les images M7 et M4 occupent des zones Flash distinctes. La plage et les rampes
du rapport cyclique sont configurées dans `app_m4/src/motor_control.c` ; la
fréquence PWM et le canal ADC sont définis dans l'overlay M4.

### Reconstruction rapide

Après une simple modification C, réutiliser le dossier de build :

```bash
cmake --build build_m7
cmake --build build_m4
```

Relancer `west build -p always` après un changement de board, de configuration,
de shield ou de Devicetree, ou lorsqu'un build propre est nécessaire.

## Flasher les deux cœurs

Avec la carte branchée au port USB ST-LINK :

```bash
west flash -d build_m7 --runner openocd
west flash -d build_m4 --runner openocd
```

Flasher d'abord le M7, puis le M4. Pour une image M7 sans LCD, remplacer
`build_m7` par `build_m7_no_lcd`.

### Accès USB depuis WSL2

Sous Windows, installer `usbipd-win` :

```powershell
winget install --interactive --exact dorssel.usbipd-win
usbipd list
```

Dans un PowerShell administrateur, partager une fois le périphérique ST-LINK :

```powershell
usbipd bind --busid 3-2
```

Adapter `3-2` à la valeur affichée par `usbipd list`, puis l'attacher à WSL à
chaque session :

```powershell
usbipd attach --wsl --busid 3-2
```

Dans WSL :

```bash
lsusb
west flash -d build_m7 --runner openocd
west flash -d build_m4 --runner openocd
minicom -D /dev/ttyACM0 -b 115200
```

Si les permissions USB imposent `sudo`, conserver explicitement le virtualenv :

```bash
sudo .venv/bin/west flash -d build_m7 --runner openocd
```

À la fin de la session :

```powershell
usbipd detach --busid 3-2
```

## Console UART

La console utilise 115200 bauds. Sous Windows, relever le port
`STMicroelectronics STLink Virtual COM Port` dans le Gestionnaire de
périphériques, puis lancer :

```powershell
pip install pyserial
python -m serial.tools.miniterm COM5 115200
```

Adapter `COM5` au port réel. Quitter avec `Ctrl+]`.

Sous WSL :

```bash
minicom -D /dev/ttyACM0 -b 115200
```

Quitter minicom avec `Ctrl-A`, puis `X`.

## Vérifications fonctionnelles

Après le flash M7 :

1. vérifier le heartbeat et les logs UART ;
2. brancher Ethernet et relever l'adresse IP ;
3. exécuter `ping <adresse-ip>` ;
4. ouvrir `http://<adresse-ip>/` ;
5. vérifier Modbus TCP sur le port 502, Unit-ID 1 ;
6. tester EtherNet/IP et DPWS avec les scripts ci-dessous.

### EtherNet/IP

```bash
python tools/eip_probe.py 192.168.0.3
```

Le probe vérifie `ListIdentity`, `RegisterSession` et lit le nom produit dans
l'Identity Object. Filtre Wireshark :

```text
enip || cip
```

Pour une connexion Class 1, utiliser :

- Assembly 100 : 20 octets O-to-T ;
- Assembly 101 : 20 octets T-to-O ;
- Assembly 1 : configuration vide ;
- Run/Idle header uniquement en O-to-T ;
- Point-to-Point dans les deux directions ;
- mots encodés en little-endian.

### DPWS/WS-Discovery

Sous Windows, relever l'index de l'interface Ethernet :

```powershell
netsh interface ipv6 show interfaces
python tools/dpws_probe.py --interface-index 11
```

Le script envoie un Probe à `[ff02::c]:3702`, reçoit le ProbeMatch, suit le
`XAddr`, effectue un WS-Transfer Get et affiche les métadonnées. Il utilise
uniquement la bibliothèque standard Python.

Filtres Wireshark :

```text
udp.port == 3702
http || xml
```

### M4 moteur

Après le flash M4 :

- la LED bleue clignote en READY ;
- START démarre le moteur et allume la LED bleue en continu ;
- le potentiomètre règle la consigne PWM entre 80 et 100 % ;
- STOP effectue une courte rampe d'arrêt puis rétablit le clignotement lent ;
- DIRECTION change le sens uniquement à l'arrêt ;
- la LED rouge indique un défaut logiciel GPIO/PWM.

Le câblage complet est documenté dans
[`app_m4/README.md`](app_m4/README.md).

## Diagnostic et problèmes connus

### Windows : antivirus et `west init`

Sur certains postes d'entreprise, l'antivirus verrouille temporairement les
fichiers `.git`. Réessayer après le clonage manuel et conserver
`west init -l .` depuis la racine du dépôt.

### Windows : mauvais Ninja sélectionné

Si IAR fournit un autre `ninja.exe`, le link peut échouer avec un fichier `.rsp`
contenant un BOM UTF-8. Vérifier la commande utilisée :

```powershell
where.exe ninja
```

Activer le virtualenv après `pip install ninja`, ou placer le Ninja WinGet avant
celui d'IAR dans le `PATH`.

### Flash OpenOCD

OpenOCD peut afficher `clearing lockup after double fault` après le flash.
Appuyer sur RESET si le firmware ne repart pas immédiatement.

### Outils de diagnostic du build

Résumé mémoire :

```bash
cat build_m7/zephyr/zephyr.stat
```

Partition `settings` et QSPI :

```bash
rg -n -C 4 'zephyr,settings-partition|storage_partition|qspi-nor-flash@0' build_m7/zephyr/zephyr.dts
```

Configuration NVS/QSPI active :

```bash
rg -n 'CONFIG_NVS=|CONFIG_SETTINGS_NVS|CONFIG_FLASH_STM32_QSPI|CONFIG_SETTINGS_FCB' build_m7/zephyr/.config
```

## Carte cible

- STM32H747I-DISCO ;
- Cortex-M7 : configuré à 400 MHz, cible `stm32h747i_disco/stm32h747xx/m7` ;
- Cortex-M4 : configuré à 200 MHz, cible `stm32h747i_disco/stm32h747xx/m4` ;
- fréquences maximales du MCU : 480 MHz pour le M7 et 240 MHz pour le M4.

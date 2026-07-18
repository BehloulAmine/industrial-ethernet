# Industrial Ethernet — Zephyr sur STM32H747I-DISCO

Prototype embarqué industriel sur **STM32H747I-DISCO** démontrant :

- TCP/IP, DHCP, mDNS
- Webserver HTTPS
- Modbus TCP (server + scanner)
- EtherNet/IP (OpENer)
- WS-Discovery (DPWS)
- Firmware update + Secure Boot

## Documentation

Ce README sert de guide de setup, build, flash et vérification du projet courant.

- [Eth_industriel_plan.md](Eth_industriel_plan.md) : plan fonctionnel complet de la démo Ethernet industriel, avec les phases Modbus, webserver, EtherNet/IP, DPWS, sécurité, firmware update et secure boot.
- [Devicetree_memory.md](Devicetree_memory.md) : notes techniques sur le devicetree STM32H747, la mémoire visible par l'image M7, les partitions flash et le stockage `settings` sur QSPI.

## État actuel

La branche actuelle correspond au bring-up M7 avec :

- heartbeat LED et logs UART ;
- Ethernet IPv4 avec DHCP ou IP statique persistée ;
- shell UART avec commandes `ip show`, `ip dhcp`, `ip static`, `uptime` et `reboot` ;
- serveur Modbus TCP sur le port 502, unit-ID 1 ;
- device EtherNet/IP OpENer sur TCP/UDP 44818 et I/O UDP 2222 ;
- assemblies Class 1 : Input 100 et Output 101, chacune reliée aux 10 mots de la fenêtre scanner ;
- registres Modbus exposant la configuration réseau, l'IP active, l'uptime, l'état du lien et un compteur de connexions ;
- stockage `settings` via NVS sur la QSPI externe, pour éviter d'utiliser un secteur de flash interne.

Test EtherNet/IP depuis le PC, après avoir flashé la carte :

```powershell
python tools/eip_probe.py 192.168.0.3
```

Le probe vérifie `ListIdentity`, `RegisterSession` et lit le nom produit avec
`Get_Attribute_Single` sur l'Identity Object. Wireshark peut décoder les échanges
avec le filtre `enip || cip`.

Pour un échange cyclique avec un PLC ou un scanner EtherNet/IP, configurer une
connexion Exclusive Owner avec 20 octets T-to-O sur l'Assembly 100 et 20 octets
O-to-T sur l'Assembly 101. Les mots sont encodés en little-endian.

## Prérequis
- **Python 3.12** disponible dans le PATH
- **7-Zip** installé dans `C:\Program Files\7-Zip\` (requis par `west sdk install`)

## Setup WSL Ubuntu

Cette variante est souvent plus simple que Git Bash pour construire Zephyr, car elle evite les
specificites Windows sur les chemins, `7z`, et certains outils CMake.

### 1. Installer les paquets systeme

```bash
sudo apt update
sudo apt install --no-install-recommends git cmake ninja-build gperf \
  ccache dfu-util device-tree-compiler wget python3-dev python3-venv python3-tk \
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1
```

### 2. Cloner et initialiser le workspace

```bash
git clone https://github.com/behloulmedamine/industrial-ethernet.git industrial-ethernet
cd industrial-ethernet
git submodule update --init --recursive

python3 -m venv .venv
source .venv/bin/activate

pip install --upgrade pip
pip install west
west init -l .
west update
```

> ⏳ `west update` est long (~2-5 Go, 20-40 min).

### 3. Installer le SDK Zephyr et les dependances Python

```bash
west zephyr-export
west packages pip --install
west sdk install
```

Sous WSL Ubuntu, le patch Windows `patches/zephyr-cmake-backslash.patch` n'est pas necessaire.

### 4. Build

```bash
west build -p always -b stm32h747i_disco/stm32h747xx/m7 app
```

Pour construire avec le dashboard LCD local (phase 5) :

```bash
west update
west build -p always -b stm32h747i_disco/stm32h747xx/m7 app -- \
  -DSHIELD=st_b_lcd40_dsi1_mb1166 -DEXTRA_CONF_FILE=lcd.conf
```

Si la dalle porte la révision A09, utiliser le shield
`st_b_lcd40_dsi1_mb1166_a09`. Le LCD est piloté par le M7, car c'est la cible
prise en charge par Zephyr pour LTDC/MIPI-DSI sur cette carte.

Sous Windows, installer également Ninja dans le virtualenv avec
`pip install ninja`. Cela évite que le `ninja.exe` fourni par IAR soit utilisé
pour l'archivage de LVGL.

### 5. Flash depuis WSL

Deux approches possibles :

- recommande : lancer le build dans WSL et flasher depuis Windows avec `west flash --runner openocd`
  dans un terminal Windows/Git Bash pointe sur le meme workspace
- alternative : utiliser l'USB dans WSL via `usbipd-win` si ton poste l'autorise

Si tu flashes depuis Windows, le patch CMake Windows peut redevenir utile pour `west build`, mais
pas pour un build deja produit dans WSL.

#### Flash et UART directement depuis WSL2 avec usbipd-win

Si tu veux flasher et lire l'UART directement depuis Ubuntu WSL2, il faut attacher le ST-Link a WSL.

1. Brancher la carte STM32H747I-DISCO en USB.
2. Dans **PowerShell**, lister les peripheriques USB :

```powershell
winget install --interactive --exact dorssel.usbipd-win
usbipd list
```

Exemple de ligne attendue :

```text
3-2    0483:374e  ST-Link Debug, Dispositif de stockage de masse USB, USB Serial Device
```

3. Dans **PowerShell en mode administrateur**, partager le peripherique avec usbipd :

```powershell
usbipd bind --busid 3-2
```

> En pratique, `usbipd bind --busid ...` se fait souvent **une seule fois** par peripherique.
> Il faut le relancer seulement si le partage a ete perdu ou si le bus change.

4. Ensuite, attacher le peripherique a WSL :

```powershell
usbipd attach --wsl --busid 3-2
```

5. Dans Ubuntu WSL, verifier que la carte est visible :

```bash
lsusb
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

Pour cette carte, le port serie est souvent :

```bash
/dev/ttyACM0
```

6. Flasher depuis WSL :

```bash
west flash --runner openocd
```

Si besoin, utiliser `sudo` pour les acces USB :

```bash
sudo .venv/bin/west flash --runner openocd
```

> Avec `sudo`, l'environnement virtuel Python `.venv` n'est pas repris automatiquement, donc la
> commande `west` peut ne plus etre trouvee. Utiliser alors explicitement `.venv/bin/west`.

7. Ouvrir la console serie depuis WSL :

```bash
minicom -D /dev/ttyACM0 -b 115200
```

Si `minicom` n'est pas encore installe :

```bash
sudo apt update
sudo apt install -y minicom
```

Quitter `minicom` avec `Ctrl-A`, puis `X`.

8. Quand tu as fini, detacher le peripherique depuis Windows :

```powershell
usbipd detach --busid 3-2
```

## Setup Windows

### 1. Cloner et initialiser le workspace

```bash
git clone https://github.com/behloulmedamine/industrial-ethernet.git industrial-ethernet
cd industrial-ethernet
git submodule update --init --recursive

# Créer le venv Python
py -3.12 -m venv .venv
source .venv/Scripts/activate   # Windows Git Bash
# source .venv/bin/activate     # Linux/macOS

pip install west
west init -l .
west update
```

> ⏳ `west update` est long (~2-5 Go, 20-40 min).

> ⚠️ Sur Windows entreprise, `west init` peut échouer à cause de l'antivirus qui verrouille les
> fichiers `.git`. Contournement : cloner manuellement puis utiliser `west init -l .`.

### 2. Installer le SDK Zephyr et les dépendances Python

```bash
# Dépendances Python Zephyr
pip install -r ../zephyr/scripts/requirements.txt

# SDK Zephyr + toolchain ARM (installé dans ~/zephyr-sdk-x.x.x/)
west sdk install

# Exporter les packages CMake
west zephyr-export
```

> ⚠️ `west sdk install` requiert que **7-Zip** soit accessible (`7z` en Git Bash).

### 3. (Windows uniquement) Appliquer le patch CMake backslash

```bash
cd ../zephyr
git apply ../industrial-ethernet/patches/zephyr-cmake-backslash.patch
cd ../industrial-ethernet
```

> ⚠️ Ce patch corrige la gestion des backslashes dans `-fmacro-prefix-map` sur Windows.
> Il est perdu à chaque `west update` — le réappliquer si nécessaire.

### 4. Build & Flash

```bash
west build -p always -b stm32h747i_disco/stm32h747xx/m7 app
west flash --runner openocd
```

Quand utiliser `west build` vs `cmake --build build` :

- utiliser `west build -p always -b stm32h747i_disco/stm32h747xx/m7 app` quand tu changes la board, `prj.conf`, un overlay DTS, le toolchain, ou quand tu veux repartir d'un build propre
- utiliser `cmake --build build` quand le dossier `build/` existe deja et que tu veux simplement recompiler vite apres une modif de code ou relancer la generation sans refaire un build pristine
- en cas de doute, rester sur `west build -p always ...` : c'est la commande Zephyr la plus robuste

> ⚠️ Sur certains PC Windows où **IAR** est installé, `west build` peut échouer au link avec une
> erreur liée à un fichier `.rsp` contenant un **BOM** UTF-8 (symptôme typique :
> `cannot find ´╗┐zephyr/...`). Dans ce cas, ajouter la ligne suivante dans `~/.bashrc` pour
> forcer l'utilisation du `ninja.exe` installé par WinGet avant celui de IAR, puis rouvrir Git Bash
> ou exécuter `source ~/.bashrc` :
>
> ```bash
> export PATH="/c/Users/SESA743279/AppData/Local/Microsoft/WinGet/Links:$PATH"
> ```

> ⚠️ `west flash` utilise `STM32CubeProgrammer` par défaut — utiliser `--runner openocd`
> (inclus dans le SDK Zephyr).

### 5. Vérifier

- **LED LD1** clignote (heartbeat ~1 Hz)
- Brancher un câble Ethernet → logs UART affichent l'IP DHCP
- `ping <IP>` depuis le PC
- Le serveur Modbus TCP écoute sur `<IP>:502`, unit-ID `1`

**Logs UART (115200 baud) :**

```bash
pip install pyserial
python -m serial.tools.miniterm COM5 115200  # remplacer COM5 par le port réel
```

Dans le Gestionnaire de périphériques → **Ports (COM et LPT)** → noter le port
**"STMicroelectronics STLink Virtual COM Port"**. Quitter avec `Ctrl+]`.

> OpenOCD peut afficher `clearing lockup after double fault` après le flash — c'est normal.
> Appuyer sur **RESET (B2)** si la LED ne clignote pas.

### 6. Vérifications utiles en Git Bash

Lire le résumé mémoire généré au build :

```bash
cat build/zephyr/zephyr.stat
```

Chercher si `zephyr,settings-partition` pointe bien vers la QSPI :

```bash
grep -nE -C 4 'zephyr,settings-partition|storage_partition|qspi-nor-flash@0' build/zephyr/zephyr.dts
```

Vérifier la config active pour `NVS` et le driver QSPI :

```bash
grep -nE 'CONFIG_NVS=|CONFIG_SETTINGS_NVS|CONFIG_FLASH_STM32_QSPI|CONFIG_SETTINGS_FCB' build/zephyr/.config
```

Si `rg` est installé, tu peux préférer :

```bash
rg -n -C 4 'zephyr,settings-partition|storage_partition|qspi-nor-flash@0' build/zephyr/zephyr.dts
rg -n 'CONFIG_NVS=|CONFIG_SETTINGS_NVS|CONFIG_FLASH_STM32_QSPI|CONFIG_SETTINGS_FCB' build/zephyr/.config
```

## Structure du projet

```
industrial-ethernet/
├── west.yml              ← Manifest Zephyr (pointe vers v4.4.0)
├── README.md             ← Setup, build, flash et vérification
├── Eth_industriel_plan.md ← Plan global de la démo industrielle
├── Devicetree_memory.md  ← Notes mémoire, devicetree et settings/QSPI
├── app/                  ← Application Cortex-M7 (réseau)
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── boards/           ← DTS overlays
│   └── src/              ← Main, réseau, Modbus TCP et commandes shell
├── app_m4/               ← (futur) Application Cortex-M4 (LCD)
└── patches/              ← Patches Zephyr pour Windows
```

## Board

- **STM32H747I-DISCO** — Cortex-M7 480 MHz + Cortex-M4 240 MHz
- Target M7 : `stm32h747i_disco/stm32h747xx/m7`
- Target M4 : `stm32h747i_disco/stm32h747xx/m4`

# H747 Demo — Webserver Industriel sous Zephyr

Prototype embarqué industriel sur **STM32H747I-DISCO** démontrant :
- TCP/IP, DHCP, mDNS
- Webserver HTTPS
- Modbus TCP (server + scanner)
- EtherNet/IP (OpENer)
- WS-Discovery (DPWS)
- Firmware update OTA + Secure Boot

## Quick Start

### 1. Cloner et initialiser le workspace

```bash
git clone <repo-url> h747-demo
cd h747-demo

# Créer le venv Python
python -m venv .venv
source .venv/Scripts/activate   # Windows Git Bash
# source .venv/bin/activate     # Linux/macOS

pip install west
west init -l .
west update
west zephyr-export
pip install -r zephyr/scripts/requirements.txt
```

### 2. (Windows uniquement) Appliquer le patch CMake backslash

```bash
cd zephyr
git apply ../patches/zephyr-cmake-backslash.patch
cd ..
```

### 3. Build & Flash

```bash
west build -p always -b stm32h747i_disco/stm32h747xx/m7 app
west flash --runner openocd
```

### 4. Vérifier

- LED LD1 clignote (heartbeat)
- Brancher un câble Ethernet → logs UART affichent l'IP DHCP
- `ping <IP>` depuis le PC

## Structure du projet

```
h747-demo/
├── west.yml              ← Manifest Zephyr (pointe vers v4.1.0)
├── app/                  ← Application Cortex-M7 (réseau)
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── boards/           ← DTS overlays
│   └── src/main.c
├── app_m4/               ← (futur) Application Cortex-M4 (LCD)
├── patches/              ← Patches Zephyr pour Windows
└── README.md
```

## Board

- **STM32H747I-DISCO** — Cortex-M7 480 MHz + Cortex-M4 240 MHz
- Target M7: `stm32h747i_disco/stm32h747xx/m7`
- Target M4: `stm32h747i_disco/stm32h747xx/m4`

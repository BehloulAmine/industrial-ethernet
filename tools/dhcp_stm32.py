#!/usr/bin/env python3
"""
DHCP server for direct PC <-> STM32H7 connection (Zephyr RTOS).

Inspired by dhcp_ip_assignment.py (EthModSitara project).
Key difference: Zephyr sends option 61 (client identifier) as
  0x01 + 6-byte MAC, not a UTF-8 string.
  Filtering is done by MAC address (OUI or exact MAC).

Prerequisites:
  pip install scapy psutil

  Set the PC Ethernet interface to a static IP in the same subnet as DEVICE_IP:
    IP      : 192.168.0.10  (or any address in 192.168.0.0/24)
    Mask    : 255.255.255.0
    Gateway : (empty)
  The gateway sent to the board is auto-detected from this interface.

Usage (run as Administrator on Windows):
  python dhcp_stm32.py
  python dhcp_stm32.py --device-ip 192.168.0.3 --mac 02:80:e1:xx:xx:xx
  python dhcp_stm32.py --device-ip 192.168.0.3 --oui ""
"""

import binascii
import os
import socket
import ipaddress
import argparse
import time

import psutil
from scapy.all import Ether, IP, UDP, BOOTP, DHCP, sendp, sniff

# ---------------------------------------------------------------------------
# Configuration (edit here or use CLI args)
# ---------------------------------------------------------------------------
DEVICE_IP    = "192.168.0.3"      # IP to assign to the STM32
SUBNET_MASK  = "255.255.255.0"
# Gateway is auto-detected from the PC interface in the same subnet.

# Filter: accept only packets whose source MAC starts with this OUI.
# Common STM32/Zephyr locally-administered MAC prefix = "02:80:e1".
# Set to None to accept any DHCP client (useful for first-time discovery).
TARGET_OUI   = "02:80:e1"

# Or filter by exact MAC (overrides OUI filter if set):
#   TARGET_MAC = "02:80:e1:ab:cd:ef"
TARGET_MAC   = None
# ---------------------------------------------------------------------------

dhcp_state = False


def get_interface_info(device_ip: str):
    """Auto-detect the PC network interface that shares a /24 subnet with device_ip."""
    device_network = ipaddress.ip_network(device_ip + '/24', strict=False)
    for iface, addrs in psutil.net_if_addrs().items():
        for addr in addrs:
            if addr.family == socket.AF_INET:
                if ipaddress.ip_address(addr.address) in device_network:
                    mac = next(
                        (a.address.replace("-", ":").lower()
                         for a in addrs if a.family == psutil.AF_LINK),
                        None
                    )
                    return iface, mac, addr.address
    return None, None, None


def mac_matches(src_mac: str) -> bool:
    """Return True if src_mac matches the configured filter."""
    src = src_mac.lower()
    if TARGET_MAC:
        return src == TARGET_MAC.lower()
    if TARGET_OUI:
        return src.startswith(TARGET_OUI.lower())
    return True  # no filter: accept all


def normalize_filter(value):
    """Convert empty CLI filter values to None."""
    if value is None:
        return None

    value = value.strip()
    return value or None


def build_offer(raw_mac, xid, device_ip, gateway, subnet, server_ip, server_mac):
    return (
        Ether(src=server_mac, dst="ff:ff:ff:ff:ff:ff") /
        IP(src=server_ip, dst="255.255.255.255") /
        UDP(sport=67, dport=68) /
        BOOTP(op='BOOTREPLY', chaddr=raw_mac,
              yiaddr=device_ip, siaddr=server_ip, xid=xid) /
        DHCP(options=[
            ("message-type", "offer"),
            ("server_id",    server_ip),
            ("subnet_mask",  subnet),
            ("router",       gateway),
            ("lease_time",   172800),
            ("renewal_time", 86400),
            ("rebinding_time", 138240),
            "end"
        ])
    )


def build_ack(raw_mac, xid, device_ip, gateway, subnet, server_ip, server_mac):
    return (
        Ether(src=server_mac, dst="ff:ff:ff:ff:ff:ff") /
        IP(src=server_ip, dst="255.255.255.255") /
        UDP(sport=67, dport=68) /
        BOOTP(op='BOOTREPLY', chaddr=raw_mac,
              yiaddr=device_ip, siaddr=server_ip, xid=xid) /
        DHCP(options=[
            ("message-type", "ack"),
            ("server_id",    server_ip),
            ("subnet_mask",  subnet),
            ("router",       gateway),
            ("lease_time",   172800),
            ("renewal_time", 86400),
            ("rebinding_time", 138240),
            "end"
        ])
    )


def handle_dhcp(pkt, device_ip, gateway, subnet, server_ip, server_mac, iface):
    global dhcp_state

    if not pkt.haslayer(DHCP):
        return

    src_mac = pkt[Ether].src
    if not mac_matches(src_mac):
        return

    msg_type = next(
        (opt[1] for opt in pkt[DHCP].options if opt[0] == "message-type"),
        None
    )
    xid     = pkt[BOOTP].xid
    raw_mac = binascii.unhexlify(src_mac.replace(":", ""))

    if msg_type == 1:  # DISCOVER
        print(f"[DISCOVER] {src_mac}  ->  offering {device_ip}")
        pkt_out = build_offer(raw_mac, xid, device_ip, gateway, subnet,
                              server_ip, server_mac)
        sendp(pkt_out, iface=iface, verbose=False)
        print(f"[OFFER]    sent to {src_mac}")

    elif msg_type == 3:  # REQUEST
        print(f"[REQUEST]  {src_mac}  ->  ACK {device_ip}")
        pkt_out = build_ack(raw_mac, xid, device_ip, gateway, subnet,
                            server_ip, server_mac)
        sendp(pkt_out, iface=iface, verbose=False)
        print(f"[ACK]      lease granted: {device_ip}\n")
        dhcp_state = True


def run(device_ip, subnet, target_mac, target_oui):
    global TARGET_MAC, TARGET_OUI
    TARGET_MAC = normalize_filter(target_mac)
    TARGET_OUI = normalize_filter(target_oui)

    iface, server_mac, server_ip = get_interface_info(device_ip)
    if not iface:
        print(f"[ERROR] No interface found in the same /24 as {device_ip}")
        print(f"        Set a static IP on your Ethernet adapter in that subnet.")
        return

    # Use the auto-detected server IP as the gateway for the board
    gateway = server_ip

    print(f"Interface   : {iface}")
    print(f"Server IP   : {server_ip}  (PC Ethernet adapter, used as gateway)")
    print(f"Server MAC  : {server_mac}")
    print(f"Device IP   : {device_ip}  (will be assigned to the board)")
    print(f"Subnet      : {subnet}")
    filter_desc = TARGET_MAC or (f"OUI {TARGET_OUI}*" if TARGET_OUI else "any")
    print(f"MAC filter  : {filter_desc}")
    print("\nWaiting for DHCP DISCOVER... (Ctrl+C to stop)\n")

    sniff(
        filter="udp and src port 68 and dst port 67",
        prn=lambda pkt: handle_dhcp(pkt, device_ip, gateway, subnet,
                                    server_ip, server_mac, iface),
        iface=iface,
        timeout=15,
        count=4,   # DISCOVER + REQUEST (x2 for retries)
        store=False,
    )

    time.sleep(1)

    if dhcp_state:
        print(f"Verifying connectivity -> ping {device_ip} ...")
        response = os.system(f"ping -n 1 {device_ip}")
        if response == 0:
            print(f"\n[OK] DHCP OK - board is reachable at {device_ip}")
        else:
            print(f"\n[FAIL] Board not responding to ping (may need more time or a reboot)")
    else:
        print("[FAIL] DHCP exchange did not complete (timeout)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="DHCP server for STM32H7 direct connection (Zephyr)")
    parser.add_argument("--device-ip",  default=DEVICE_IP,
                        help=f"IP to assign to the board (default: {DEVICE_IP})")
    parser.add_argument("--subnet",     default=SUBNET_MASK,
                        help=f"Subnet mask (default: {SUBNET_MASK})")
    parser.add_argument("--mac",        default=None,
                        help="Exact MAC address of the board (overrides --oui)")
    parser.add_argument("--oui",        default=TARGET_OUI,
                        help=f"MAC OUI prefix to filter (default: {TARGET_OUI})")
    args = parser.parse_args()

    run(args.device_ip, args.subnet, args.mac, args.oui)

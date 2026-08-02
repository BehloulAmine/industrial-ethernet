#!/usr/bin/env python3
"""Discover the STM32 DPWS device and fetch its WS-Transfer metadata."""

from __future__ import annotations

import argparse
import http.client
import ipaddress
import os
import socket
import struct
import sys
import time
import uuid
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from urllib.parse import urlsplit


WSD_MULTICAST_IPV6 = "ff02::c"
WSD_PORT = 3702
WSD_NS = "http://schemas.xmlsoap.org/ws/2005/04/discovery"
WSA_NS = "http://schemas.xmlsoap.org/ws/2004/08/addressing"
TRANSFER_NS = "http://schemas.xmlsoap.org/ws/2004/09/transfer"
DEVICE_PROFILE_NS = "http://schemas.xmlsoap.org/ws/2006/02/devprof"

METADATA_FIELDS = (
    "Manufacturer",
    "ManufacturerUrl",
    "ModelName",
    "ModelNumber",
    "ModelUrl",
    "PresentationUrl",
    "FriendlyName",
    "FirmwareVersion",
    "SerialNumber",
    "ProductCode",
    "ProductRange",
    "ProductCapability",
    "HardwareRevision",
    "FriendlyNameSource",
    "PhysicalLocation",
    "NodeName",
    "TimeSinceBoot",
    "ServicesSupported",
    "MacAddress",
    "IPv4Address",
    "IPv6Address",
    "DeviceUUID",
)


@dataclass
class DiscoveredDevice:
    endpoint: str
    source_ipv6: str
    scope_id: int
    xaddrs: list[str]
    types: str
    scopes: str


def local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1].rsplit(":", 1)[-1]


def parse_xml(payload: bytes) -> ET.Element:
    start = payload.find(b"<?xml")
    if start >= 0:
        payload = payload[start:]
    return ET.fromstring(payload.decode("utf-8", errors="strict"))


def first_text(root: ET.Element, name: str) -> str:
    for element in root.iter():
        if local_name(element.tag) == name and element.text:
            return element.text.strip()
    return ""


def build_probe(message_id: str) -> bytes:
    return f'''<?xml version="1.0" encoding="utf-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
 xmlns:wsa="{WSA_NS}" xmlns:wsd="{WSD_NS}" xmlns:wsdp="{DEVICE_PROFILE_NS}">
 <soap:Header>
  <wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>
  <wsa:Action>{WSD_NS}/Probe</wsa:Action>
  <wsa:MessageID>{message_id}</wsa:MessageID>
 </soap:Header>
 <soap:Body><wsd:Probe><wsd:Types>wsdp:Device</wsd:Types></wsd:Probe></soap:Body>
</soap:Envelope>'''.encode("utf-8")


def parse_probe_match(
    payload: bytes, source_ipv6: str, scope_id: int, expected_relates_to: str
) -> DiscoveredDevice | None:
    try:
        root = parse_xml(payload)
    except (UnicodeDecodeError, ET.ParseError):
        return None

    action = first_text(root, "Action")
    relates_to = first_text(root, "RelatesTo")
    if not action.endswith("/ProbeMatches"):
        return None
    if relates_to and relates_to != expected_relates_to:
        return None

    endpoint = first_text(root, "Address")
    xaddr_text = first_text(root, "XAddrs")
    if not endpoint or not xaddr_text:
        return None

    return DiscoveredDevice(
        endpoint=endpoint,
        source_ipv6=source_ipv6,
        scope_id=scope_id,
        xaddrs=xaddr_text.split(),
        types=first_text(root, "Types"),
        scopes=first_text(root, "Scopes"),
    )


def build_transfer_get(endpoint: str, message_id: str) -> bytes:
    return f'''<?xml version="1.0" encoding="utf-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
 xmlns:wsa="{WSA_NS}">
 <soap:Header>
  <wsa:To>{endpoint}</wsa:To>
  <wsa:Action>{TRANSFER_NS}/Get</wsa:Action>
  <wsa:MessageID>{message_id}</wsa:MessageID>
 </soap:Header>
 <soap:Body />
</soap:Envelope>'''.encode("utf-8")


def fetch_metadata(
    xaddr: str, endpoint: str, interface_index: int, timeout: float
) -> tuple[dict[str, str], str]:
    parts = urlsplit(xaddr)
    if parts.scheme != "http" or not parts.hostname:
        raise ValueError(f"Unsupported XAddr: {xaddr}")

    host = parts.hostname
    connect_host = host
    if ":" in host and "%" not in host:
        connect_host = f"{host}%{interface_index}"

    path = parts.path or "/"
    if parts.query:
        path += f"?{parts.query}"

    request_id = f"urn:uuid:{uuid.uuid4()}"
    body = build_transfer_get(endpoint, request_id)
    connection = http.client.HTTPConnection(connect_host, parts.port or 80, timeout=timeout)
    try:
        connection.request(
            "POST",
            path,
            body=body,
            headers={
                "Content-Type": "application/soap+xml; charset=utf-8",
                "User-Agent": "Industrial-Ethernet-DPWS-Probe/1.0",
                "Connection": "close",
            },
        )
        response = connection.getresponse()
        payload = response.read()
        if response.status != 200:
            raise RuntimeError(f"HTTP {response.status} {response.reason}")
    finally:
        connection.close()

    root = parse_xml(payload)
    metadata = {field: first_text(root, field) for field in METADATA_FIELDS}
    return metadata, xaddr


def available_interfaces() -> str:
    try:
        return ", ".join(f"{index}:{name}" for index, name in socket.if_nameindex())
    except OSError:
        return "unavailable"


def route_local_ipv4(target_ipv4: str) -> str:
    address = ipaddress.ip_address(target_ipv4)
    if address.version != 4:
        raise ValueError(f"--target-ip must be an IPv4 address: {target_ipv4}")

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect((str(address), 9))
        return sock.getsockname()[0]


def windows_route_interface_index(target_ipv4: str) -> int | None:
    if os.name != "nt":
        return None

    try:
        import ctypes

        class SockaddrIn(ctypes.Structure):
            _fields_ = (
                ("sin_family", ctypes.c_ushort),
                ("sin_port", ctypes.c_ushort),
                ("sin_addr", ctypes.c_ubyte * 4),
                ("sin_zero", ctypes.c_ubyte * 8),
            )

        destination = SockaddrIn()
        destination.sin_family = socket.AF_INET
        destination.sin_addr[:] = socket.inet_aton(target_ipv4)
        interface_index = ctypes.c_ulong()
        get_best_interface = ctypes.windll.iphlpapi.GetBestInterfaceEx
        get_best_interface.argtypes = (
            ctypes.POINTER(SockaddrIn),
            ctypes.POINTER(ctypes.c_ulong),
        )
        get_best_interface.restype = ctypes.c_ulong
        result = get_best_interface(
            ctypes.byref(destination), ctypes.byref(interface_index)
        )
    except (AttributeError, OSError, ValueError):
        return None

    return interface_index.value if result == 0 else None


def resolve_interface_index(
    index: int | None, name: str | None, target_ipv4: str
) -> int:
    if index is not None:
        return index
    if name:
        return socket.if_nametoindex(name)

    candidates = [
        item for item in socket.if_nameindex()
        if item[1].casefold() == "ethernet"
    ]
    if len(candidates) == 1:
        return candidates[0][0]

    local_ipv4 = route_local_ipv4(target_ipv4)
    detected_index = windows_route_interface_index(target_ipv4)
    if detected_index is not None:
        print(
            f"Using interface index {detected_index} "
            f"(local IPv4 {local_ipv4}, route to {target_ipv4})"
        )
        return detected_index

    raise ValueError(
        f"Could not map local IPv4 {local_ipv4} to an IPv6 interface. "
        "Specify --interface-index or --interface. "
        f"Available interfaces: {available_interfaces()}"
    )


def discover(interface_index: int, timeout: float) -> list[DiscoveredDevice]:
    message_id = f"urn:uuid:{uuid.uuid4()}"
    probe = build_probe(message_id)
    devices: dict[str, DiscoveredDevice] = {}

    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as sock:
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF,
                        struct.pack("@I", interface_index))
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_HOPS, 1)
        sock.bind(("::", 0))
        sock.settimeout(0.2)
        source_port = sock.getsockname()[1]

        print(f"Sending DPWS Probe to [{WSD_MULTICAST_IPV6}%{interface_index}]:{WSD_PORT}")
        print(f"Message ID : {message_id}")
        print(f"Source port: {source_port}")
        sock.sendto(probe, (WSD_MULTICAST_IPV6, WSD_PORT, 0, interface_index))

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                payload, peer = sock.recvfrom(8192)
            except socket.timeout:
                continue

            source_ipv6, source_port, _flow, scope_id = peer
            if source_port != WSD_PORT:
                continue
            device = parse_probe_match(
                payload, source_ipv6, scope_id or interface_index, message_id
            )
            if device is not None:
                devices[device.endpoint] = device

    return list(devices.values())


def print_device(device: DiscoveredDevice, metadata: dict[str, str], xaddr: str) -> None:
    print("\nFound DPWS device")
    print(f"  Endpoint UUID : {device.endpoint.removeprefix('urn:uuid:')}")
    print(f"  Source IPv6   : {device.source_ipv6}%{device.scope_id}")
    print(f"  Types         : {device.types}")
    print(f"  XAddr used    : {xaddr}")
    print(f"  Device name   : {metadata.get('FriendlyName', '')}")
    print(f"  Manufacturer  : {metadata.get('Manufacturer', '')}")
    print(f"  Model         : {metadata.get('ModelName', '')}")
    print(f"  FW version    : {metadata.get('FirmwareVersion', '')}")
    print(f"  Hardware ID   : {metadata.get('SerialNumber', '')}")
    print(f"  MAC address   : {metadata.get('MacAddress', '')}")
    print(f"  IPv4 address  : {metadata.get('IPv4Address', '')}")
    print(f"  IPv6 address  : {metadata.get('IPv6Address', '')}")
    print(f"  Node name     : {metadata.get('NodeName', '')}")
    print(f"  Services      : {metadata.get('ServicesSupported', '')}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Discover the Zephyr device over DPWS/WS-Discovery IPv6"
    )
    parser.add_argument("--interface-index", type=int, help="IPv6 interface index, e.g. 11")
    parser.add_argument("--interface", help="Interface name, e.g. Ethernet")
    parser.add_argument(
        "--target-ip",
        default="192.168.0.3",
        help="Device IPv4 used for automatic interface selection (default: 192.168.0.3)",
    )
    parser.add_argument("--timeout", type=float, default=5.0, help="Discovery timeout")
    parser.add_argument("--metadata-timeout", type=float, default=3.0)
    args = parser.parse_args()

    try:
        interface_index = resolve_interface_index(
            args.interface_index, args.interface, args.target_ip
        )
        devices = discover(interface_index, args.timeout)
    except (OSError, ValueError) as error:
        print(f"Discovery setup failed: {error}", file=sys.stderr)
        return 2

    if not devices:
        print("No DPWS ProbeMatch received.")
        return 1

    failed = False
    for device in devices:
        metadata: dict[str, str] = {}
        used_xaddr = ""
        errors: list[str] = []
        for xaddr in device.xaddrs:
            try:
                metadata, used_xaddr = fetch_metadata(
                    xaddr, device.endpoint, interface_index, args.metadata_timeout
                )
                break
            except (OSError, ValueError, RuntimeError, ET.ParseError) as error:
                errors.append(f"{xaddr}: {error}")

        if not metadata:
            failed = True
            print(f"\nFound {device.endpoint}, but metadata retrieval failed:")
            for error in errors:
                print(f"  {error}")
            continue
        print_device(device, metadata, used_xaddr)

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

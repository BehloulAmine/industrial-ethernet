#!/usr/bin/env python3
"""Probe the demo EtherNet/IP adapter without external dependencies."""

import socket
import struct
import sys


EIP_PORT = 44818
ENCAP_HEADER = struct.Struct("<HHII8sI")


def encap(command, payload=b"", session=0, context=b"ZEPEIP00"):
    return ENCAP_HEADER.pack(command, len(payload), session, 0, context, 0) + payload


def parse_encap(packet):
    if len(packet) < ENCAP_HEADER.size:
        raise RuntimeError("short EtherNet/IP response")
    command, length, session, status, context, options = ENCAP_HEADER.unpack_from(packet)
    payload = packet[ENCAP_HEADER.size:]
    if len(payload) != length:
        raise RuntimeError("invalid encapsulation payload length")
    if status != 0:
        raise RuntimeError(f"encapsulation error 0x{status:08x}")
    return command, session, payload


def recv_exact(sock, length):
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise RuntimeError("connection closed")
        data.extend(chunk)
    return bytes(data)


def recv_tcp_encap(sock):
    header = recv_exact(sock, ENCAP_HEADER.size)
    length = struct.unpack_from("<H", header, 2)[0]
    return parse_encap(header + recv_exact(sock, length))


def list_identity(ip):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(2.0)
        sock.sendto(encap(0x0063), (ip, EIP_PORT))
        packet, _ = sock.recvfrom(512)

    command, _, payload = parse_encap(packet)
    if command != 0x0063 or len(payload) < 40:
        raise RuntimeError("invalid ListIdentity response")
    item_length = struct.unpack_from("<H", payload, 4)[0]
    item = payload[6:6 + item_length]
    vendor, device_type, product = struct.unpack_from("<HHH", item, 18)
    serial = struct.unpack_from("<I", item, 28)[0]
    name_length = item[32]
    name = item[33:33 + name_length].decode("ascii", errors="replace")
    print(f"ListIdentity: {name}")
    print(f"  vendor=0x{vendor:04x} type=0x{device_type:04x} "
          f"product=0x{product:04x} serial=0x{serial:08x}")


def get_product_name(ip):
    with socket.create_connection((ip, EIP_PORT), timeout=2.0) as sock:
        sock.sendall(encap(0x0065, struct.pack("<HH", 1, 0)))
        command, session, _ = recv_tcp_encap(sock)
        if command != 0x0065 or session == 0:
            raise RuntimeError("RegisterSession failed")
        print(f"RegisterSession: handle=0x{session:08x}")

        cip = bytes((0x0E, 0x03, 0x20, 0x01, 0x24, 0x01, 0x30, 0x07))
        cpf = struct.pack("<IHHHHHH", 0, 0, 2, 0, 0, 0x00B2, len(cip)) + cip
        sock.sendall(encap(0x006F, cpf, session=session))
        command, _, payload = recv_tcp_encap(sock)
        if command != 0x006F or len(payload) < 21:
            raise RuntimeError("invalid SendRRData response")
        cip_response = payload[16:]
        if cip_response[2] != 0:
            raise RuntimeError(f"CIP error 0x{cip_response[2]:02x}")
        name_length = cip_response[4]
        name = cip_response[5:5 + name_length].decode("ascii", errors="replace")
        print(f"GetAttributeSingle Identity.1.7: {name}")

        sock.sendall(encap(0x0066, session=session))


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <board-ip>")
    list_identity(sys.argv[1])
    get_product_name(sys.argv[1])


if __name__ == "__main__":
    main()

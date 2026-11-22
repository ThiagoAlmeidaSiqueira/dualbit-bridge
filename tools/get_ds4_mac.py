"""
get_ds4_mac.py — le o MAC Bluetooth de um DualShock 4 / DualSense ligado
por USB no Windows, falando direto com o driver libusb-win32 (libusb0).

Como funciona
-------------
O controle PlayStation expoe o proprio endereco Bluetooth no "feature
report" 0x12, acessivel por um control transfer da classe HID:

    bmRequestType = 0xA1  (Device-to-host, Class, Interface)
    bRequest      = 0x01  (GET_REPORT)
    wValue        = 0x0312 (tipo Feature << 8 | report ID 0x12)
    wIndex        = numero da interface
    wLength       = 16

Resposta (16 bytes):
    [0]      = 0x12 (report id)
    [1..6]   = MAC Bluetooth, em ordem INVERSA (LSB primeiro)
    [7..12]  = MAC do "device" (lado USB)
    [13..15] = reservado

Nao requer alterar driver nem elevar privilegios: usa a mesma API que o
Zadig/DS4Windows usa (C:\\Windows\\System32\\libusb0.dll).

Uso:  python tools/get_ds4_mac.py
"""

import ctypes as C
import sys

PATH_MAX = 512

# VID/PID conhecidos de controles PlayStation (modo USB)
KNOWN = {
    (0x054C, 0x05C4): "DualShock 4 v1 (CUH-ZCT1)",
    (0x054C, 0x09CC): "DualShock 4 v2 (CUH-ZCT2)",
    (0x054C, 0x0CE6): "DualSense (CFI-ZCT1)",
    (0x054C, 0x0DF2): "DualSense Edge",
}


class UsbDeviceDescriptor(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("bLength", C.c_uint8),
        ("bDescriptorType", C.c_uint8),
        ("bcdUSB", C.c_uint16),
        ("bDeviceClass", C.c_uint8),
        ("bDeviceSubClass", C.c_uint8),
        ("bDeviceProtocol", C.c_uint8),
        ("bMaxPacketSize0", C.c_uint8),
        ("idVendor", C.c_uint16),
        ("idProduct", C.c_uint16),
        ("bcdDevice", C.c_uint16),
        ("iManufacturer", C.c_uint8),
        ("iProduct", C.c_uint8),
        ("iSerialNumber", C.c_uint8),
        ("bNumConfigurations", C.c_uint8),
    ]


class UsbBus(C.Structure):
    pass


class UsbDevice(C.Structure):
    pass


UsbBus._fields_ = [
    ("next", C.POINTER(UsbBus)),
    ("prev", C.POINTER(UsbBus)),
    ("dirname", C.c_char * PATH_MAX),
    ("devices", C.POINTER(UsbDevice)),
    ("location", C.c_ulong),
]

UsbDevice._fields_ = [
    ("next", C.POINTER(UsbDevice)),
    ("prev", C.POINTER(UsbDevice)),
    ("filename", C.c_char * PATH_MAX),
    ("bus", C.POINTER(UsbBus)),
    ("descriptor", UsbDeviceDescriptor),
    ("config", C.c_void_p),
    ("dev", C.c_void_p),
    ("devnum", C.c_uint8),
    ("num_children", C.c_uint8),
    ("children", C.POINTER(C.POINTER(UsbDevice))),
]


def load_libusb0():
    for name in ("libusb0.dll", r"C:\Windows\System32\libusb0.dll"):
        try:
            lib = C.CDLL(name)
            break
        except OSError:
            continue
    else:
        sys.exit("ERRO: libusb0.dll nao encontrada. O controle esta com o "
                 "driver libusb-win32 instalado?")

    lib.usb_init.restype = None
    lib.usb_init.argtypes = []
    lib.usb_find_busses.restype = C.c_int
    lib.usb_find_busses.argtypes = []
    lib.usb_find_devices.restype = C.c_int
    lib.usb_find_devices.argtypes = []
    lib.usb_get_busses.restype = C.POINTER(UsbBus)
    lib.usb_get_busses.argtypes = []
    lib.usb_open.restype = C.c_void_p
    lib.usb_open.argtypes = [C.POINTER(UsbDevice)]
    lib.usb_close.restype = C.c_int
    lib.usb_close.argtypes = [C.c_void_p]
    lib.usb_claim_interface.restype = C.c_int
    lib.usb_claim_interface.argtypes = [C.c_void_p, C.c_int]
    lib.usb_release_interface.restype = C.c_int
    lib.usb_release_interface.argtypes = [C.c_void_p, C.c_int]
    lib.usb_set_configuration.restype = C.c_int
    lib.usb_set_configuration.argtypes = [C.c_void_p, C.c_int]
    lib.usb_control_msg.restype = C.c_int
    lib.usb_control_msg.argtypes = [
        C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_int,
        C.POINTER(C.c_char), C.c_int, C.c_int,
    ]
    return lib


def format_mac(data):
    return ":".join("%02x" % b for b in data)


def main():
    lib = load_libusb0()
    lib.usb_init()
    lib.usb_find_busses()
    lib.usb_find_devices()

    bus = lib.usb_get_busses()
    found = []
    while bus:
        dev = bus.contents.devices
        while dev:
            d = dev.contents.descriptor
            key = (d.idVendor, d.idProduct)
            if key in KNOWN:
                found.append((dev, d, dev.contents.filename.decode(errors="replace")))
            dev = dev.contents.next
        bus = bus.contents.next

    if not found:
        print("Nenhum controle PlayStation encontrado via libusb0.")
        print("Verifique se o cabo USB esta conectado e se o driver do")
        print("controle e o libusb-win32 (Zadig).")
        return 1

    rc = 0
    for dev, desc, filename in found:
        model = KNOWN[(desc.idVendor, desc.idProduct)]
        print("=" * 62)
        print("Controle : %s" % model)
        print("USB ID   : %04x:%04x" % (desc.idVendor, desc.idProduct))
        print("Dispositivo: %s" % filename)

        h = lib.usb_open(dev)
        if not h:
            print("  ERRO: nao foi possivel abrir (em uso por outro programa?)")
            rc = 2
            continue

        try:
            lib.usb_set_configuration(h, 1)  # ignora falha: ja configurado

            buf = (C.c_char * 16)()
            got = -1
            iface_ok = -1
            for iface in (0, 3):
                if lib.usb_claim_interface(h, iface) != 0:
                    continue
                got = lib.usb_control_msg(
                    h, 0xA1, 0x01, (0x03 << 8) | 0x12, iface, buf, 16, 1000
                )
                if got > 0:
                    iface_ok = iface
                    break
                lib.usb_release_interface(h, iface)

            if got <= 0:
                print("  ERRO: feature report 0x12 falhou (ret=%d)" % got)
                rc = 3
                continue

            raw = bytes(buf[:got])
            print("  Interface: %d   bytes lidos: %d" % (iface_ok, got))
            print("  Bruto    : %s" % " ".join("%02X" % b for b in raw))

            if got < 13:
                print("  ERRO: resposta curta demais")
                rc = 4
                continue

            bt_mac = list(raw[1:7])[::-1]     # ordem inversa no report
            usb_mac = list(raw[7:13])[::-1]

            print("  MAC Bluetooth (DS4 no ESP32): %s" % format_mac(bt_mac))
            print("  MAC do lado USB            : %s" % format_mac(usb_mac))

            if all(b == 0 for b in bt_mac):
                print("  AVISO: MAC Bluetooth zerado. Este firmware pode nao")
                print("         expor o endereco; use o modo pareamento do ESP32.")
        finally:
            lib.usb_close(h)

    return rc


if __name__ == "__main__":
    sys.exit(main())

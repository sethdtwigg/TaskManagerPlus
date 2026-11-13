"""
Checks the generated NetworkLookup tables (plugins/NetworkLookup/country.bin, ports_data.h) with the same
binary-search convention the plugin's C code uses. Run: python tools/tests/networklookup-test.py
"""
import ipaddress
import os
import re
import struct
import sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "plugins", "NetworkLookup")
data = open(os.path.join(root, "country.bin"), "rb").read()
assert data[:4] == b"TCT1", "bad magic"
v4n, v6n, date = struct.unpack_from("<III", data, 4)
assert len(data) == 16 + 6 * (v4n + v6n), "size does not match the header"
v4 = [struct.unpack_from("<IH", data, 16 + 6 * i) for i in range(v4n)]
v6 = [struct.unpack_from("<IH", data, 16 + 6 * (v4n + i)) for i in range(v6n)]
for name, t in (("v4", v4), ("v6", v6)):
    assert all(t[i][0] < t[i + 1][0] for i in range(len(t) - 1)), name + " not strictly sorted"
    assert all(t[i][1] != t[i + 1][1] for i in range(len(t) - 1)), name + " has mergeable neighbours"


def find(t, key):
    lo, hi = 0, len(t)
    while lo < hi:
        mid = (lo + hi) // 2
        if t[mid][0] <= key:
            lo = mid + 1
        else:
            hi = mid
    return t[lo - 1][1] if lo else 0


def cc(packed):
    return "" if not packed else chr(packed >> 8) + chr(packed & 255)


fail = 0


def check(ok, text):
    global fail
    print(("PASS  " if ok else "FAIL  ") + text)
    if not ok:
        fail += 1


for ip, want in (("8.8.8.8", "US"), ("1.1.1.1", "AU"), ("193.0.14.129", "NL"), ("133.1.1.1", "JP"),
                 ("41.0.0.1", "ZA"), ("200.160.2.3", "BR"), ("203.0.113.5", "")):
    got = cc(find(v4, int(ipaddress.IPv4Address(ip))))
    check(got == want, "IPv4 %s -> %r (want %r)" % (ip, got, want))
for ip, want in (("2001:4860:4860::8888", "US"), ("2a00:1450:4001::1", "IE"), ("2606:4700:4700::1111", "US")):
    got = cc(find(v6, int(ipaddress.IPv6Address(ip)) >> 96))
    check(got == want, "IPv6 %s -> %r (want %r)" % (ip, got, want))

ports = {}
for m in re.finditer(r'\{ (\d+), (\d), L"([^"]*)", L"([^"]*)" \},', open(os.path.join(root, "ports_data.h"), encoding="utf-8").read()):
    ports.setdefault(int(m.group(1)), []).append(m.group(3))
for port, want in ((80, "http"), (443, "https"), (22, "ssh"), (53, "domain"), (3389, "ms-wbt-server"), (5353, "mdns"), (25, "smtp")):
    check(want in ports.get(port, []), "port %d -> %s" % (port, ports.get(port)))
check(len(ports) > 5000, "port table has %d distinct ports" % len(ports))

print("delegation file date %d, %d IPv4 + %d IPv6 ranges" % (date, v4n, v6n))
sys.exit(1 if fail else 0)

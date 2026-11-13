#!/usr/bin/env python3
"""
Generates the offline lookup tables of the NetworkLookup plugin (Phase 7 item 2).

This script never touches the network. It reads two files you downloaded once, by hand, from the
public registries (see docs/network-lookup.md for what they are and where they come from):

  --iana  service-names-port-numbers.csv    (IANA Service Name and Transport Protocol Port Number Registry)
  --nro   nro-delegated-stats               (NRO combined extended delegation statistics of the five RIRs)

and writes next to this repository's plugins/NetworkLookup:

  ports_data.h      sorted C table of port -> service name (+ short description for ports below 1024)
  country.bin       compact IPv4 / IPv6 -> country table, embedded into the plugin as an RCDATA resource
  country_info.h    the date of the delegation file and the table sizes

Usage:
  python tools/make-lookup-tables.py --iana C:\\data\\service-names-port-numbers.csv --nro C:\\data\\nro-delegated-stats

country.bin layout (little endian):
  char  magic[4] = 'TCT1'
  u32   v4Count, u32 v6Count, u32 date (yyyymmdd of the delegation file)
  v4Count x { u32 start; u16 country }   sorted by start; an entry covers [start, next start); country 0 = unallocated
  v6Count x { u32 start; u16 country }   start = top 32 bits of the address; same convention
A country is two ASCII letters packed as (first << 8) | second.
"""
import argparse
import csv
import ipaddress
import os
import struct
import sys

COUNTRIES = {
    "AD": "Andorra", "AE": "United Arab Emirates", "AF": "Afghanistan", "AG": "Antigua and Barbuda",
    "AI": "Anguilla", "AL": "Albania", "AM": "Armenia", "AO": "Angola", "AQ": "Antarctica",
    "AR": "Argentina", "AS": "American Samoa", "AT": "Austria", "AU": "Australia", "AW": "Aruba",
    "AX": "Aland Islands", "AZ": "Azerbaijan", "BA": "Bosnia and Herzegovina", "BB": "Barbados",
    "BD": "Bangladesh", "BE": "Belgium", "BF": "Burkina Faso", "BG": "Bulgaria", "BH": "Bahrain",
    "BI": "Burundi", "BJ": "Benin", "BL": "Saint Barthelemy", "BM": "Bermuda", "BN": "Brunei",
    "BO": "Bolivia", "BQ": "Bonaire, Sint Eustatius and Saba", "BR": "Brazil", "BS": "Bahamas",
    "BT": "Bhutan", "BV": "Bouvet Island", "BW": "Botswana", "BY": "Belarus", "BZ": "Belize",
    "CA": "Canada", "CC": "Cocos (Keeling) Islands", "CD": "Congo (DRC)", "CF": "Central African Republic",
    "CG": "Congo", "CH": "Switzerland", "CI": "Cote d'Ivoire", "CK": "Cook Islands", "CL": "Chile",
    "CM": "Cameroon", "CN": "China", "CO": "Colombia", "CR": "Costa Rica", "CU": "Cuba",
    "CV": "Cabo Verde", "CW": "Curacao", "CX": "Christmas Island", "CY": "Cyprus", "CZ": "Czechia",
    "DE": "Germany", "DJ": "Djibouti", "DK": "Denmark", "DM": "Dominica", "DO": "Dominican Republic",
    "DZ": "Algeria", "EC": "Ecuador", "EE": "Estonia", "EG": "Egypt", "EH": "Western Sahara",
    "ER": "Eritrea", "ES": "Spain", "ET": "Ethiopia", "FI": "Finland", "FJ": "Fiji",
    "FK": "Falkland Islands", "FM": "Micronesia", "FO": "Faroe Islands", "FR": "France", "GA": "Gabon",
    "GB": "United Kingdom", "GD": "Grenada", "GE": "Georgia", "GF": "French Guiana", "GG": "Guernsey",
    "GH": "Ghana", "GI": "Gibraltar", "GL": "Greenland", "GM": "Gambia", "GN": "Guinea",
    "GP": "Guadeloupe", "GQ": "Equatorial Guinea", "GR": "Greece", "GS": "South Georgia and the South Sandwich Islands",
    "GT": "Guatemala", "GU": "Guam", "GW": "Guinea-Bissau", "GY": "Guyana", "HK": "Hong Kong",
    "HM": "Heard Island and McDonald Islands", "HN": "Honduras", "HR": "Croatia", "HT": "Haiti",
    "HU": "Hungary", "ID": "Indonesia", "IE": "Ireland", "IL": "Israel", "IM": "Isle of Man",
    "IN": "India", "IO": "British Indian Ocean Territory", "IQ": "Iraq", "IR": "Iran", "IS": "Iceland",
    "IT": "Italy", "JE": "Jersey", "JM": "Jamaica", "JO": "Jordan", "JP": "Japan", "KE": "Kenya",
    "KG": "Kyrgyzstan", "KH": "Cambodia", "KI": "Kiribati", "KM": "Comoros", "KN": "Saint Kitts and Nevis",
    "KP": "North Korea", "KR": "South Korea", "KW": "Kuwait", "KY": "Cayman Islands", "KZ": "Kazakhstan",
    "LA": "Laos", "LB": "Lebanon", "LC": "Saint Lucia", "LI": "Liechtenstein", "LK": "Sri Lanka",
    "LR": "Liberia", "LS": "Lesotho", "LT": "Lithuania", "LU": "Luxembourg", "LV": "Latvia",
    "LY": "Libya", "MA": "Morocco", "MC": "Monaco", "MD": "Moldova", "ME": "Montenegro",
    "MF": "Saint Martin", "MG": "Madagascar", "MH": "Marshall Islands", "MK": "North Macedonia",
    "ML": "Mali", "MM": "Myanmar", "MN": "Mongolia", "MO": "Macao", "MP": "Northern Mariana Islands",
    "MQ": "Martinique", "MR": "Mauritania", "MS": "Montserrat", "MT": "Malta", "MU": "Mauritius",
    "MV": "Maldives", "MW": "Malawi", "MX": "Mexico", "MY": "Malaysia", "MZ": "Mozambique",
    "NA": "Namibia", "NC": "New Caledonia", "NE": "Niger", "NF": "Norfolk Island", "NG": "Nigeria",
    "NI": "Nicaragua", "NL": "Netherlands", "NO": "Norway", "NP": "Nepal", "NR": "Nauru",
    "NU": "Niue", "NZ": "New Zealand", "OM": "Oman", "PA": "Panama", "PE": "Peru",
    "PF": "French Polynesia", "PG": "Papua New Guinea", "PH": "Philippines", "PK": "Pakistan",
    "PL": "Poland", "PM": "Saint Pierre and Miquelon", "PN": "Pitcairn Islands", "PR": "Puerto Rico",
    "PS": "Palestine", "PT": "Portugal", "PW": "Palau", "PY": "Paraguay", "QA": "Qatar",
    "RE": "Reunion", "RO": "Romania", "RS": "Serbia", "RU": "Russia", "RW": "Rwanda",
    "SA": "Saudi Arabia", "SB": "Solomon Islands", "SC": "Seychelles", "SD": "Sudan", "SE": "Sweden",
    "SG": "Singapore", "SH": "Saint Helena", "SI": "Slovenia", "SJ": "Svalbard and Jan Mayen",
    "SK": "Slovakia", "SL": "Sierra Leone", "SM": "San Marino", "SN": "Senegal", "SO": "Somalia",
    "SR": "Suriname", "SS": "South Sudan", "ST": "Sao Tome and Principe", "SV": "El Salvador",
    "SX": "Sint Maarten", "SY": "Syria", "SZ": "Eswatini", "TC": "Turks and Caicos Islands",
    "TD": "Chad", "TF": "French Southern Territories", "TG": "Togo", "TH": "Thailand",
    "TJ": "Tajikistan", "TK": "Tokelau", "TL": "Timor-Leste", "TM": "Turkmenistan", "TN": "Tunisia",
    "TO": "Tonga", "TR": "Turkiye", "TT": "Trinidad and Tobago", "TV": "Tuvalu", "TW": "Taiwan",
    "TZ": "Tanzania", "UA": "Ukraine", "UG": "Uganda", "UM": "US Minor Outlying Islands",
    "US": "United States", "UY": "Uruguay", "UZ": "Uzbekistan", "VA": "Vatican City",
    "VC": "Saint Vincent and the Grenadines", "VE": "Venezuela", "VG": "British Virgin Islands",
    "VI": "US Virgin Islands", "VN": "Vietnam", "VU": "Vanuatu", "WF": "Wallis and Futuna",
    "WS": "Samoa", "YE": "Yemen", "YT": "Mayotte", "ZA": "South Africa", "ZM": "Zambia",
    "ZW": "Zimbabwe",
    # registry pseudo-countries that appear in the delegation files
    "EU": "Europe (region)", "AP": "Asia/Pacific (region)",
}


def cc_pack(code):
    return (ord(code[0]) << 8) | ord(code[1])


def cstr(text):
    out = []
    for ch in text:
        if ch in '"\\':
            out.append("\\" + ch)
        elif ord(ch) < 32 or ord(ch) > 126:
            out.append("?")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def wstr(text):
    return "L" + cstr(text)


def make_ports(iana_path, out_path):
    entries = {}  # (port, proto) -> (name, description)
    with open(iana_path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            name = (row.get("Service Name") or "").strip()
            port = (row.get("Port Number") or "").strip()
            proto = (row.get("Transport Protocol") or "").strip().lower()
            desc = " ".join((row.get("Description") or "").split())
            if not name or not port.isdigit() or proto not in ("tcp", "udp"):
                continue
            port = int(port)
            if port == 0:
                continue
            key = (port, proto)
            if key not in entries:
                entries[key] = (name, desc)

    # merge tcp/udp with the same name into one row (proto bit 1 = tcp, 2 = udp)
    merged = {}
    for (port, proto), (name, desc) in entries.items():
        bit = 1 if proto == "tcp" else 2
        merged.setdefault((port, name), [0, desc])
        merged[(port, name)][0] |= bit

    rows = sorted(merged.items())
    with open(out_path, "w", encoding="utf-8", newline="\n") as o:
        o.write("// Generated by tools/make-lookup-tables.py from the IANA service name and port number registry.\n")
        o.write("// Do not edit. Sorted by port; Protocols: 1 = TCP, 2 = UDP.\n")
        for (port, name), (protos, desc) in rows:
            d = desc if port < 1024 else ""
            if len(d) > 110:
                d = d[:107] + "..."
            o.write("{ %d, %d, %s, %s },\n" % (port, protos, wstr(name), wstr(d)))
    return len(rows)


def make_countries(nro_path, bin_path, info_path, names_path):
    v4 = []
    v6 = []
    date = 0
    with open(nro_path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.rstrip("\n").split("|")
            if len(p) >= 6 and p[0].isdigit() and p[1] == "nro":
                date = int(p[5]) if p[5].isdigit() else date
                continue
            if len(p) < 7 or p[2] not in ("ipv4", "ipv6"):
                continue
            if p[6] not in ("allocated", "assigned") or len(p[1]) != 2 or p[1] not in COUNTRIES:
                continue
            code = p[1]
            if p[2] == "ipv4":
                start = int(ipaddress.IPv4Address(p[3]))
                v4.append((start, start + int(p[4]) - 1, code))
            else:
                prefix = int(p[4])
                if prefix > 32:
                    continue  # longer than the 32 bit resolution of the table
                start = int(ipaddress.IPv6Address(p[3])) >> 96
                v6.append((start, start + (1 << (32 - prefix)) - 1, code))

    def build(ranges, top):
        ranges.sort()
        out = []  # (start, cc)
        pos = 0
        for start, end, code in ranges:
            if start > pos:
                out.append((pos, 0))  # gap
            elif start < pos:
                continue  # overlap, keep the first
            out.append((start, cc_pack(code)))
            pos = end + 1
        if pos <= top:
            out.append((pos, 0))
        merged = []
        for start, cc in out:
            if merged and merged[-1][1] == cc:
                continue
            merged.append((start, cc))
        return merged

    t4 = build(v4, 0xFFFFFFFF)
    t6 = build(v6, 0xFFFFFFFF)

    with open(bin_path, "wb") as b:
        b.write(b"TCT1")
        b.write(struct.pack("<III", len(t4), len(t6), date))
        for start, cc in t4:
            b.write(struct.pack("<IH", start, cc))
        for start, cc in t6:
            b.write(struct.pack("<IH", start, cc))

    with open(info_path, "w", encoding="utf-8", newline="\n") as o:
        o.write("// Generated by tools/make-lookup-tables.py. Do not edit.\n")
        o.write("#define COUNTRY_TABLE_DATE %d\n#define COUNTRY_TABLE_V4_ENTRIES %d\n#define COUNTRY_TABLE_V6_ENTRIES %d\n"
                % (date, len(t4), len(t6)))

    with open(names_path, "w", encoding="utf-8", newline="\n") as o:
        o.write("// Generated by tools/make-lookup-tables.py. Do not edit. Sorted by code.\n")
        for code in sorted(COUNTRIES):
            o.write("{ 0x%04X, %s, %s },\n" % (cc_pack(code), wstr(code), wstr(COUNTRIES[code])))

    return len(t4), len(t6), date


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iana", required=True)
    ap.add_argument("--nro", required=True)
    args = ap.parse_args()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "plugins", "NetworkLookup")
    os.makedirs(out, exist_ok=True)
    n = make_ports(args.iana, os.path.join(out, "ports_data.h"))
    a, b, date = make_countries(args.nro, os.path.join(out, "country.bin"),
                                os.path.join(out, "country_info.h"), os.path.join(out, "countries_data.h"))
    print("ports: %d rows; country: %d IPv4 + %d IPv6 ranges, delegation file date %d" % (n, a, b, date))


if __name__ == "__main__":
    sys.exit(main())

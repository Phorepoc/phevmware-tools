#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: LGPL-2.1-only
r"""pack -- package phevmwsvc as a SIS file for Phorepoc (Python 3; in the pdk's toolchain container, or on a host with the pdk checked out).

    python3 recipes/pack.py --service phevmwsvc.exe --version 0.1.0 --out phevmware-tools-x32.sis

The package carries:
  * `!:\sys\bin\phevmwsvc.exe`       the service (CommDD: it opens the kernel's PheBackdoor channel)
  * `!:\private\10003a3f\import\apps\phevmwsvc_reg.rsc` + `!:\resource\apps\phevmwsvc_loc.rsc` + the icon: the menu entry
    "VMware Tools" (starting it from the menu starts the service; it runs until the device stops)
  * the licences, NOTICE and the written source offer, in `!:\resource\phevmw\licences\`
Nothing is signed (Phorepoc installs unsigned). Needs the pdk's tools/emitter (sis.py, e32image.py, mmp.py; env PHOREPOC_PDK,
default ../pdk) and, for the registration and the icon, the tools of the pdk's toolchain image (rcomp, mifconv).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PMS = os.path.dirname(HERE)
PDK = os.environ.get("PHOREPOC_PDK", os.path.join(os.path.dirname(PMS), "pdk"))
sys.path.insert(0, os.path.join(PDK, "tools", "emitter"))
import e32image as E32   # noqa: E402
import mmp as MMP        # noqa: E402
import sis as SIS        # noqa: E402

APP_UID = 0xE5A0F211     # phevmwsvc.exe (its UID3, the application)
PKG_UID = 0xE5A0F210     # the package
CAPS = ("CommDD",)       # phevmwsvc.mmp


def caps_bits(names):
    return sum(1 << MMP.Mmp.CAPS.index(n.lower()) for n in names)


def with_caps(img, caps):
    b = bytearray(img)
    struct.pack_into("<II", b, 0x88, caps & 0xFFFFFFFF, 0)
    E32.set_header_crc(b)
    return bytes(b)


def compile_rss(text, name, out_rsc):
    d = tempfile.mkdtemp()
    rss = os.path.join(d, name + ".rss")
    with open(rss, "w") as f:
        f.write(text)
    r = subprocess.run([sys.executable, os.path.join(PDK, "tools", "rsc.py"), rss, out_rsc], cwd=PDK, capture_output=True, text=True)
    if r.returncode or not os.path.isfile(out_rsc):
        sys.exit("pack: rcomp failed on %s:\n%s%s" % (name, r.stdout[-800:], r.stderr[-800:]))


def make_mif(svg, out_name):
    out_mif = os.path.join(tempfile.mkdtemp(), out_name)
    r = subprocess.run([sys.executable, os.path.join(PDK, "tools", "app_icon_mif.py"), svg, out_mif], cwd=PDK, capture_output=True, text=True)
    if r.returncode or not os.path.isfile(out_mif):
        sys.exit("pack: app_icon_mif failed: " + (r.stdout + r.stderr)[-400:])
    with open(out_mif, "rb") as f:
        return f.read()


def licences():
    out = []
    names = ["NOTICE", "SOURCE-OFFER.txt", "LICENSE"] + ["LICENSES/" + x for x in sorted(os.listdir(os.path.join(PMS, "LICENSES")))]
    for f in names:
        with open(os.path.join(PMS, f), "rb") as fh:
            out.append((fh.read(), r"!:\resource\phevmw\licences" + "\\" + os.path.basename(f), None))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--service", required=True, help="phevmwsvc.exe built with sis=True")
    ap.add_argument("--icon", default=os.path.join(PMS, "os", "src", "phevmwsvc", "phevmw_icon.svg"))
    ap.add_argument("--version", default="0.1.0")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    version = tuple(int(x) for x in a.version.split("."))
    td = tempfile.mkdtemp()
    with open(a.service, "rb") as f:
        exe = with_caps(f.read(), caps_bits(CAPS))
    files = [(exe, r"!:\sys\bin\phevmwsvc.exe", caps_bits(CAPS))]
    # the boot services list: core's pheshellsel starts every start= line of \resource\phe\services\*.txt before the shell
    svc = b"# started at boot by the shell selector (a copy already running ends at once)\nstart=!:\\sys\\bin\\phevmwsvc.exe\n"
    files.append((svc, r"!:\resource\phe\services\phevmwsvc.txt", None))
    mif = "phevmw_%08x.mif" % APP_UID
    files.append((make_mif(a.icon, mif), r"!:\resource\apps" + "\\" + mif, None))
    reg = ('#include <appinfo.rh>\nUID2 KUidAppRegistrationResourceFile\nUID3 0x%08X\n'
           'RESOURCE APP_REGISTRATION_INFO\n    {\n    app_file = "phevmwsvc";\n'
           '    localisable_resource_file = "\\resource\\apps\\phevmwsvc_loc";\n    }\n') % APP_UID
    loc = ('#include <appinfo.rh>\nRESOURCE LOCALISABLE_APP_INFO\n    {\n    short_caption = "VMware";\n'
           '    caption_and_icon = CAPTION_AND_ICON_INFO\n        {\n        caption = "VMware Tools";\n        number_of_icons = 1;\n'
           '        icon_file = "\\resource\\apps\\%s";\n        };\n    }\n') % mif
    compile_rss(reg, "phevmwsvc_reg", os.path.join(td, "phevmwsvc_reg.rsc"))
    compile_rss(loc, "phevmwsvc_loc", os.path.join(td, "phevmwsvc_loc.rsc"))
    with open(os.path.join(td, "phevmwsvc_reg.rsc"), "rb") as f:
        files.append((f.read(), r"!:\private\10003a3f\import\apps\phevmwsvc_reg.rsc", None))
    with open(os.path.join(td, "phevmwsvc_loc.rsc"), "rb") as f:
        files.append((f.read(), r"!:\resource\apps\phevmwsvc_loc.rsc", None))
    files += licences()
    pkg = SIS.build_package(files, app_uid=PKG_UID, vendor="Phorepoc", app_name="VMware Tools", signer=None, version=version)
    with open(a.out, "wb") as f:
        f.write(pkg)
    print("%s: %d bytes, package uid %#x, %d files" % (a.out, len(pkg), PKG_UID, len(files)))


if __name__ == "__main__":
    main()

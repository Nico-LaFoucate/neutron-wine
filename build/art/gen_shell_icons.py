#!/usr/bin/env python3
"""Generate the shell32 icons the file dialog actually shows, as flat SVG -> .ico.

Wine's stock shell icons are glossy Tango artwork: a gradient blue folder, a CRT monitor for
My Computer, a green recycling bin. They are what fills the tree and list of the Common Item
Dialog every Adobe app raises, and after the toolbar strips they are the loudest dated thing left.

⚠️ SCOPE. shell32 ships 45 icons; this covers the THIRTEEN that appear in a file dialog. The rest
(printer, shield, sleep, log_off, control panel...) are not reachable from an Adobe open/save and
are deliberately left alone rather than redrawn blind.

Each stock .ico carries ten images — 16/32/48 at 4, 8 and 32 bpp, plus 256 at 32. We emit the
32bpp sizes only (16/32/48/256): the palettised entries are DOS-era fallbacks and Wine's icon
cache picks the deepest match available.

Usage:  gen_shell_icons.py <wine-src>/dlls/shell32/resources
"""
import os
import subprocess
import sys

# A flat two-tone language: one body colour, one shade, one accent. Readable at 16px, which is
# where these live most of the time.
C = dict(
    amber="#D9A441", amber_d="#B8862C",
    slate="#7E8896", slate_d="#5A636E",
    paper="#D6DAE0", paper_d="#AEB5BE",
    steel="#8A94A0", steel_d="#646D78",
    line="#3A4049",
)

# viewBox is 48x48 for every glyph; sizes are rendered from it.
ICONS = {
    "folder": f'''
      <path d="M5 13.5a2.5 2.5 0 0 1 2.5-2.5h11l4 4h18a2.5 2.5 0 0 1 2.5 2.5v20a2.5 2.5 0 0 1-2.5 2.5h-33A2.5 2.5 0 0 1 5 37.5z" fill="{C['amber_d']}"/>
      <path d="M5 19.5A2.5 2.5 0 0 1 7.5 17h33a2.5 2.5 0 0 1 2.5 2.5v18a2.5 2.5 0 0 1-2.5 2.5h-33A2.5 2.5 0 0 1 5 37.5z" fill="{C['amber']}"/>''',
    "folder_open": f'''
      <path d="M5 13.5a2.5 2.5 0 0 1 2.5-2.5h11l4 4h18a2.5 2.5 0 0 1 2.5 2.5v6h-38z" fill="{C['amber_d']}"/>
      <path d="M5 38l5-17a2.5 2.5 0 0 1 2.4-1.8h32.1a2 2 0 0 1 1.9 2.6l-4.6 15.6a2.5 2.5 0 0 1-2.4 1.8H7.4A2.5 2.5 0 0 1 5 38z" fill="{C['amber']}"/>''',
    # ⚠️ A TOWER, not a monitor. The first draft drew this as a monitor and Desktop is also a
    # monitor; side by side in the tree at 16px they were indistinguishable. Silhouette is the
    # only thing that survives at that size, so the two must not share one.
    "mycomputer": f'''
      <rect x="13" y="5" width="22" height="38" rx="3" fill="{C['slate_d']}"/>
      <rect x="16" y="8" width="16" height="32" rx="2" fill="{C['slate']}"/>
      <rect x="19" y="12" width="10" height="2.5" rx="1.25" fill="{C['slate_d']}"/>
      <rect x="19" y="17" width="10" height="2.5" rx="1.25" fill="{C['slate_d']}"/>
      <circle cx="24" cy="34" r="2.5" fill="{C['amber']}"/>''',
    "desktop": f'''
      <rect x="5" y="10" width="38" height="22" rx="2.5" fill="{C['slate_d']}"/>
      <rect x="8" y="13" width="32" height="16" rx="1" fill="{C['slate']}"/>
      <rect x="10" y="36" width="28" height="4" rx="2" fill="{C['slate_d']}"/>''',
    "drive": f'''
      <rect x="5" y="14" width="38" height="20" rx="3" fill="{C['steel_d']}"/>
      <rect x="5" y="14" width="38" height="12" rx="3" fill="{C['steel']}"/>
      <circle cx="36" cy="30" r="2.5" fill="{C['amber']}"/>
      <rect x="10" y="29" width="14" height="2.5" rx="1.25" fill="{C['steel']}"/>''',
    "document": f'''
      <path d="M11 6h18l9 9v27a2 2 0 0 1-2 2H11a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2z" fill="{C['paper']}"/>
      <path d="M29 6l9 9h-9z" fill="{C['paper_d']}"/>
      <g fill="{C['slate_d']}"><rect x="14" y="22" width="20" height="2.5" rx="1.25"/>
      <rect x="14" y="28" width="20" height="2.5" rx="1.25"/>
      <rect x="14" y="34" width="13" height="2.5" rx="1.25"/></g>''',
    "trash_file": f'''
      <path d="M11 15h26l-2.2 25a2.5 2.5 0 0 1-2.5 2.3H15.7a2.5 2.5 0 0 1-2.5-2.3z" fill="{C['steel']}"/>
      <rect x="8" y="10" width="32" height="5" rx="2.5" fill="{C['steel_d']}"/>
      <rect x="19" y="6" width="10" height="4" rx="2" fill="{C['steel_d']}"/>''',
    "trash_full": f'''
      <path d="M11 15h26l-2.2 25a2.5 2.5 0 0 1-2.5 2.3H15.7a2.5 2.5 0 0 1-2.5-2.3z" fill="{C['steel']}"/>
      <rect x="8" y="10" width="32" height="5" rx="2.5" fill="{C['steel_d']}"/>
      <rect x="19" y="6" width="10" height="4" rx="2" fill="{C['steel_d']}"/>
      <g fill="{C['steel_d']}"><rect x="16" y="21" width="3" height="16" rx="1.5"/>
      <rect x="22.5" y="21" width="3" height="16" rx="1.5"/>
      <rect x="29" y="21" width="3" height="16" rx="1.5"/></g>''',
    # ⚠️ Added 2026-09-11 after seeing the dialog on 11.10-84: Documents kept its stock glossy
    # icon and stood out precisely BECAUSE everything around it had changed. Grepping the shell
    # namespace (shfldr_*.c, pidl.c, classes.c) for reachable IDI_SHELL_* ids found these five as
    # the rest of what an Adobe open/save can actually surface.
    "mydocs": f'''
      <path d="M5 13.5a2.5 2.5 0 0 1 2.5-2.5h11l4 4h18a2.5 2.5 0 0 1 2.5 2.5v20a2.5 2.5 0 0 1-2.5 2.5h-33A2.5 2.5 0 0 1 5 37.5z" fill="{C['amber_d']}"/>
      <path d="M19 15h13a1.5 1.5 0 0 1 1.5 1.5v14a1.5 1.5 0 0 1-1.5 1.5H19a1.5 1.5 0 0 1-1.5-1.5v-14A1.5 1.5 0 0 1 19 15z" fill="{C['paper']}"/>
      <g fill="{C['slate_d']}"><rect x="21" y="19" width="9" height="2" rx="1"/>
      <rect x="21" y="23" width="9" height="2" rx="1"/><rect x="21" y="27" width="6" height="2" rx="1"/></g>
      <path d="M5 22.5A2.5 2.5 0 0 1 7.5 20h33a2.5 2.5 0 0 1 2.5 2.5v15a2.5 2.5 0 0 1-2.5 2.5h-33A2.5 2.5 0 0 1 5 37.5z" fill="{C['amber']}"/>''',
    "favorites": f'''
      <path d="M5 13.5a2.5 2.5 0 0 1 2.5-2.5h11l4 4h18a2.5 2.5 0 0 1 2.5 2.5v20a2.5 2.5 0 0 1-2.5 2.5h-33A2.5 2.5 0 0 1 5 37.5z" fill="{C['amber_d']}"/>
      <path d="M5 19.5A2.5 2.5 0 0 1 7.5 17h33a2.5 2.5 0 0 1 2.5 2.5v18a2.5 2.5 0 0 1-2.5 2.5h-33A2.5 2.5 0 0 1 5 37.5z" fill="{C['amber']}"/>
      <path d="M24 21l3.1 6.3 6.9 1-5 4.9 1.2 6.9L24 36.8l-6.2 3.3 1.2-6.9-5-4.9 6.9-1z" fill="#FFFFFF" opacity="0.9"/>''',
    "shortcut": f'''
      <rect x="4" y="26" width="18" height="18" rx="3" fill="{C['paper']}"/>
      <path d="M9 39l9-9M18 30h-6M18 30v6" stroke="{C['slate_d']}" stroke-width="3" fill="none" stroke-linecap="round" stroke-linejoin="round"/>''',
    "optical_drive": f'''
      <circle cx="24" cy="24" r="18" fill="{C['steel_d']}"/>
      <circle cx="24" cy="24" r="14" fill="{C['steel']}"/>
      <circle cx="24" cy="24" r="4.5" fill="{C['line']}"/>
      <path d="M24 10a14 14 0 0 1 12.1 7" stroke="{C['paper']}" stroke-width="2.5" fill="none" stroke-linecap="round" opacity="0.55"/>''',
    "netdrive": f'''
      <rect x="5" y="10" width="38" height="16" rx="3" fill="{C['steel_d']}"/>
      <rect x="5" y="10" width="38" height="10" rx="3" fill="{C['steel']}"/>
      <circle cx="36" cy="22" r="2.2" fill="{C['amber']}"/>
      <path d="M24 26v6M14 38h20" stroke="{C['slate']}" stroke-width="3" fill="none" stroke-linecap="round"/>
      <circle cx="14" cy="38" r="3.5" fill="{C['slate_d']}"/><circle cx="34" cy="38" r="3.5" fill="{C['slate_d']}"/>''',
}

SIZES = (16, 32, 48, 256)


def svg(body):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<!-- Generated by build/art/gen_shell_icons.py — do not hand-edit. -->\n'
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48" width="48" height="48">'
            f'{body}\n</svg>\n')


def main(dest):
    for name, body in ICONS.items():
        sp = os.path.join(dest, name + ".svg")
        with open(sp, "w", encoding="utf-8") as f:
            f.write(svg(body))
        pngs = []
        for s in SIZES:
            p = os.path.join(dest, f".{name}-{s}.png")
            subprocess.run(["rsvg-convert", "-w", str(s), "-h", str(s), sp, "-o", p], check=True)
            pngs.append(p)
        ico = os.path.join(dest, name + ".ico")
        # ⚠️ The 256 entry goes in PNG-COMPRESSED (--raw), which is how every stock Wine icon
        # stores it. icotool defaults to a raw bitmap: that alone took folder.ico from 17 KB to
        # 285 KB, and across a full icon set it would add megabytes to shell32 for pixels almost
        # nothing displays.
        subprocess.run(["icotool", "-c", "-o", ico] + pngs[:-1] + [f"--raw={pngs[-1]}"], check=True)
        for p in pngs:
            os.remove(p)
        print(f"  {name + '.ico':<20} {' '.join(str(s) for s in SIZES)}  "
              f"{os.path.getsize(ico)} bytes")
    print(f"{len(ICONS)} icons -> {dest}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")

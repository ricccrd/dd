#!/usr/bin/env python3
# Render dd terminal-demo GIFs (+ static posters for click-to-play). No Chromium needed.
#   nix profile install nixpkgs#imagemagick nixpkgs#dejavu_fonts nixpkgs#ffmpeg
#   python3 gen_demo.py     -> dd-run.gif/.png, dd-inside.gif/.png, dd-docker.gif/.png
# Content is real (captured from `docker --context dd ...`).
import subprocess, os, shutil

FONT, SZ, BG = "DejaVu Sans Mono", "13.5", "#1e1e2e"
W, H, PAD, FPS = 1040, 470, 28, 8
HERE = os.path.dirname(os.path.abspath(__file__))
green, txt, grey, yel, mut = "#a6e3a1", "#cdd6f4", "#6c7086", "#f9e2af", "#9399b2"
def s(t, c): return f'<span foreground="{c}">{t}</span>'
def cmd(c): return s("$", green) + s(" " + c, txt)
P = lambda body: f'<span font="{FONT} {SZ}">{body}</span>'

DEMOS = [
    ("dd-run", [
        (s("# dd — Docker on macOS, no VM.", grey), 4),
        (cmd("docker context use dd"), 4),
        (s('Current context is now "dd"', mut), 3),
        (cmd("docker run --rm alpine uname -sm"), 4),
        (s("Linux aarch64", yel) + s("   # a real Linux container", grey), 5),
        (cmd("time docker run --rm alpine true"), 4),
        (s("real    0m0.023s", yel) + s("   # a process spawn, not a boot", grey), 8),
    ]),
    ("dd-inside", [
        (s("# it's a real Alpine Linux userland:", grey), 4),
        (cmd("docker run --rm alpine cat /etc/os-release"), 4),
        (s('NAME="Alpine Linux"', mut), 2),
        (s("ID=alpine", mut), 2),
        (s('PRETTY_NAME="Alpine Linux v3.24"', mut), 3),
        (cmd('docker run --rm alpine echo "hello from a container, no VM"'), 4),
        (s("hello from a container, no VM", yel), 8),
    ]),
    ("dd-docker", [
        (s("# the Docker you already know — just a context:", grey), 4),
        (cmd("docker pull alpine"), 4),
        (s("Status: Downloaded newer image for alpine:latest", mut), 3),
        (cmd("docker images"), 4),
        (s("IMAGE           ID             SIZE", mut), 2),
        (s("alpine:latest   635c7fd1e2d7   8.41MB", mut), 3),
        (cmd("docker run --rm alpine echo it just works"), 4),
        (s("it just works", yel), 8),
    ]),
]

FR = "/tmp/ddframes"
def render(name, lines):
    shutil.rmtree(FR, ignore_errors=True); os.makedirs(FR)
    seq, last = 0, None
    for i in range(1, len(lines) + 1):
        markup = P("\n".join(l for l, _ in lines[:i]))
        st = f"{FR}/s{i:02d}.png"
        subprocess.run(["magick", "-background", BG, "-bordercolor", BG, "pango:" + markup,
                        "-border", str(PAD), "-gravity", "NorthWest", "-background", BG,
                        "-extent", f"{W}x{H}", st], check=True)
        last = st
        for _ in range(lines[i - 1][1]):
            shutil.copy(st, f"{FR}/g{seq:04d}.png"); seq += 1
    gif = os.path.join(HERE, f"{name}.gif")
    subprocess.run(["ffmpeg", "-y", "-framerate", str(FPS), "-i", f"{FR}/g%04d.png",
                    "-vf", "split[a][b];[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=bayer",
                    "-loop", "0", gif], check=True)
    shutil.copy(last, os.path.join(HERE, f"{name}-poster.png"))  # static poster = final frame
    print("wrote", name, os.path.getsize(gif), "bytes")

for name, lines in DEMOS:
    render(name, lines)

# Making the launch screencasts

Two assets, two methods. Both must be recorded on the Mac (the app + Activity Monitor
can't be captured from CI).

## 1. Terminal GIF (deterministic — no live recording)
The strongest *technical* proof: one engine, three runtimes, near-instant start.

```bash
brew install vhs
cd website/assets
vhs demo.tape          # runs the commands for real -> writes dd-demo.gif
```
Edit `demo.tape` to taste (timing, theme, size). The output GIF is real (VHS executes each line).
Prereq: the `dd` daemon running and the `dd` docker context set up (the app does both).

## 2. The "no VM" GUI gif (the money shot — record by hand)
This is the one that converts: a container runs, yet there's **no VM** in the process list.

Tool: **Kap** (free, https://getkap.co) or **Gifox** — both record a screen region straight to GIF.

Shot list (~10–15s, keep it tight):
1. dd app open on the **Home/Overview** dashboard (containers/images/disk).
2. Click **Run hello-world** (or run `docker run --rm hello-dd` in a terminal beside it).
3. Cut to **Activity Monitor** → filter "vm"/"linux"/"qemu" → **nothing** — just the `ddjit`
   process. (The whole point: the container is a process, not a VM.)
4. Optional end card: the container appears in the app's Containers list.

Tips: record at 2× / Retina, crop tight, ≤ ~8 MB so it embeds on GitHub/HN/Reddit, keep < 20s.
Export both a GIF (for README/Reddit) and an MP4 (smaller; for X/Twitter).

## Where each goes
- Terminal `dd-demo.gif` → website + README ("Is it actually fast?" / top).
- GUI "no VM" gif → top of the README and the launch posts (HN/Reddit/X).

# dd terminal demo — deterministic GIF. Render on the Mac (vhs + dd daemon running):
#   cd website/assets && vhs demo.tape
# VHS runs each command for real, headless — the output in the GIF is genuine.

Output dd-demo.gif

Set Shell "bash"
Set FontSize 18
Set Width 1200
Set Height 600
Set Padding 24
Set TypingSpeed 55ms
Set Theme "Catppuccin Mocha"

Type "# dd — run Linux containers on macOS, with no VM."
Enter
Sleep 900ms

Type "docker context use dd"
Enter
Sleep 1100ms

Type "# a real Linux container, running as a plain macOS process:"
Enter
Sleep 700ms

Type "docker run --rm alpine uname -sm"
Enter
Sleep 1900ms

Type "# start is a process spawn, not a boot:"
Enter
Sleep 700ms

Type "time docker run --rm alpine true"
Enter
Sleep 2200ms

Sleep 2200ms

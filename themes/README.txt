TILT-A-TRON THEMES
==================

Each folder inside "Theme" is one theme. "Default" is the built-in look.
To make your own: copy the Default folder, rename it, and change what you like.
Then on the Tilt-a-tron pick it in Settings > THEME.

Anything you leave out falls back to the Default look, so a theme can be as
small as a theme.json with one color changed.

  Theme/
    Default/
      theme.json          colors (see below)
      background.png      466 x 466, shown behind the home screen and menus
      icons/
        breakout.png      210 x 210 app icons (transparent corners are fine)
        maze.png          (or "Marble Maze.png" - the app's name works too)
        racer.png         (or "Grand Prix.png")
        jump.png          (or "Sky Jump.png")
        settings.png

theme.json colors are "#RRGGBB":
  background   screen color when there is no background.png
  text         titles and main text
  label        menu row labels
  dim          hints and secondary text
  panel        solid fill behind menu rows and buttons (keeps text readable)
  box          menu row outlines
  value        menu row values
  accent       highlights ("GO", selected values)
  go           buttons and success
  danger       errors

Design templates
- The Guide folder on this drive shows exactly where things land:
    guide-home.png    home screen: app icon, app name, dots, hint
    guide-menus.png   settings / pause menus: title, rows, buttons
    guide-icon.png    one app icon, 210 x 210
  Open one in any image editor, draw your art on a layer underneath, hide the
  guide layer, and export 466 x 466 (210 x 210 for icons) as PNG.

Tips
- The screen is round: keep important art inside a 466 px circle.
- Dark backgrounds look best on the AMOLED screen (black pixels are off and save battery).
- PNG only. Bigger pictures are shrunk to fit; smaller ones are centered.
- The background can be called background.png, or just be the biggest PNG in the folder.
- Icons can sit next to theme.json instead of in icons/ if you prefer.
- Eject the drive before unplugging; the new files load when you do.

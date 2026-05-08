<picture>  
<source media="(prefers-color-scheme: dark)" srcset="https://i.imgur.com/8qsV6MH.png">  
<source media="(prefers-color-scheme: light)" srcset="https://i.imgur.com/4cpzGnB.png">  
<img src="https://i.imgur.com/8qsV6MH.png" width="200">  
</picture>  
*Part of the Tico ecosystem* — https://www.ticoverse.com

**Dolphin** is a widely used emulator for the Nintendo GameCube and Wii, known for its accuracy, compatibility, and active development.

This port is exclusive to Tico, adapted to work with its frontend and runtime, and provided as a standalone build for the Nintendo Switch. It focuses on integration, consistency, and predictable behavior within the Tico ecosystem.

----------

## Summary

This port focuses on making Dolphin fit naturally within Tico, rather than behaving as a separate application.

It adds:

-   Custom overlay matching Tico design, including time, date, user avatar, and game title
-   Explicit control over display (integer scaling and aspect ratios)
-   Runtime-selectable rendering filters
-   Built-in save and load state support
-   Wiimote and GameCube controller mapping aligned with Tico input conventions

----------

## Credits

This port is built on top of the official Dolphin emulator project.

All core emulation work belongs to the Dolphin team and its contributors.

- **Official Dolphin repository** — https://github.com/dolphin-emu/dolphin
- **Dolphin website** — https://dolphin-emu.org

----------

## A Note

A lot of work in this scene disappears over time — not because it lacked value, but because it was never shared.

If you are building something, consider releasing it. Even small contributions can help others move forward.

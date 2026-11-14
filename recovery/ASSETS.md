# Recovery UI assets

Files in `root/res/images/` are copied over the recovery ramdisk's `/res/images`
by the build (`TARGET_RECOVERY_DEVICE_DIRS` / `recovery/root`). They are used by
the themed recovery UI in `bootable/recovery/recovery_ui/screen_ui_theme.cpp`
(suqing114/platform_bootable_recovery, branch `Cinderella`).

| Files | Source | License |
|---|---|---|
| `font.png`, `font_menu.png`, `font_small.png`, `font_title.png` | Cascadia Mono, rendered to minui bitmap fonts (96 glyphs x regular/bold rows) | SIL Open Font License 1.1, https://github.com/microsoft/cascadia-code |
| `ui_<icon>_<size>.png` | AOSP Material vector drawables (frameworks/base core and SystemUI, SettingsLib, packages/apps/Settings), rasterized to alpha masks | Apache License 2.0 |
| `ui_banner.png` | `hero_visual_bg_tile.png`, `hero_visual_miku.png`, `hero_title.png` from `packages/apps/MikuSettings`, composited on the UI background | MikuUI project artwork |
| `ui_chibi.png` | `miku_device_info_character.png` from `packages/apps/MikuSettings` | MikuUI project artwork |
| `ui_head.png` | `icon_miku.png` from `packages/apps/Settings` (MikuUI) | MikuUI project artwork |
| `ui_logo.png` | the stock recovery `logo_image.png`, rescaled (fallback when the banner is missing) | as bootable/recovery |

The fonts, icons and composites were generated with a small System.Drawing
tool (C# via PowerShell, no third-party code): vector paths are parsed and
filled with anti-aliasing, fonts are drawn glyph by glyph into the minui grid,
and everything is written as 8-bit gray (masks/fonts) or RGB PNGs.

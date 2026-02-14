# UAPMD Resources

This directory contains application resources including icons and branding assets.

## Icon Files

### Source
- **`logo.svg`** - Source SVG logo (512×512 viewBox)
  - Minimal design with DAW track-list background
  - Purple gradient with U-shaped connector representing audio/MIDI connectivity

### Generated Platform Icons

#### macOS
- **`uapmd.icns`** - macOS icon bundle
  - Contains multiple resolutions from 16×16 to 1024×1024
  - Includes @2x retina variants
  - Referenced in `Info.plist` as `CFBundleIconFile`
  - Automatically copied to app bundle during build

#### Windows
- **`uapmd.ico`** - Windows icon file
  - Multi-resolution icon (16×16 to 256×256)
  - Embedded via `uapmd.rc` resource script
  - Compiled into executable during build

#### Linux
- **`flatpak/dev.atsushieno.uapmd.svg`** - Symlinked/copied from logo.svg
  - Installed to `${DATAROOTDIR}/icons/hicolor/scalable/apps/`
  - Referenced in `.desktop` file as `Icon=dev.atsushieno.uapmd`

#### PNG Exports (Fallback/Multi-resolution)
Located in `icons/` subdirectory:
- `icon_16x16.png`
- `icon_32x32.png`
- `icon_48x48.png`
- `icon_64x64.png`
- `icon_128x128.png`
- `icon_256x256.png`
- `icon_512x512.png`
- `icon_1024x1024.png`

## Regenerating Icons

If you update `logo.svg`, regenerate platform icons with:

```bash
cd resources

# Generate PNG sizes
for size in 16 32 48 64 128 256 512 1024; do
  rsvg-convert -w $size -h $size logo.svg -o icons/icon_${size}x${size}.png
done

# Create macOS .icns
mkdir -p uapmd.iconset
for size in 16 32 128 256 512; do
  cp icons/icon_${size}x${size}.png uapmd.iconset/icon_${size}x${size}.png
done
for size in 32 64 256 512 1024; do
  half=$((size/2))
  cp icons/icon_${size}x${size}.png uapmd.iconset/icon_${half}x${half}@2x.png
done
iconutil -c icns uapmd.iconset -o uapmd.icns
rm -rf uapmd.iconset

# Create Windows .ico
magick icons/icon_{16x16,32x32,48x48,64x64,128x128,256x256}.png uapmd.ico

# Update Linux icon
cp logo.svg ../flatpak/dev.atsushieno.uapmd.svg
```

## Requirements
- **rsvg-convert** (librsvg) - SVG to PNG conversion
- **iconutil** (macOS) - Creating .icns files
- **ImageMagick** (magick/convert) - Creating .ico files

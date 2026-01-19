#!/usr/bin/env python3
"""
Generate a C++ header file with embedded ZIP font data.

This script creates a .zip of the input TTF file and embeds it as a uint8_t array.
The ZIP format provides compression while the app can extract it at runtime.

Usage: python3 generate_font_header.py input.ttf output.hpp
"""
import sys
import zipfile
import io

def generate_header(input_ttf, output_header):
    # Create ZIP in memory
    zip_buffer = io.BytesIO()
    with zipfile.ZipFile(zip_buffer, 'w', zipfile.ZIP_DEFLATED) as zf:
        with open(input_ttf, 'rb') as f:
            ttf_data = f.read()
        import os
        zf.writestr(os.path.basename(input_ttf), ttf_data)

    zip_data = zip_buffer.getvalue()

    with open(output_header, 'w') as f:
        f.write('#pragma once\n\n')
        f.write('#include <cstdint>\n')
        f.write('#include <cstddef>\n\n')
        f.write('namespace uapmd::app {\n\n')
        f.write('inline constexpr uint8_t kEmbeddedFontData[] = {\n')

        for i, byte in enumerate(zip_data):
            if i % 16 == 0:
                f.write('    ')
            f.write(f'0x{byte:02x}')
            if i < len(zip_data) - 1:
                f.write(',')
                if (i + 1) % 16 == 0:
                    f.write('\n')
                else:
                    f.write(' ')
            else:
                f.write('\n')

        f.write('};\n\n')
        f.write(f'inline constexpr size_t kEmbeddedFontSize = {len(zip_data)};\n\n')
        f.write('} // namespace uapmd::app\n')

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f'Usage: {sys.argv[0]} <input_ttf> <output_header>')
        sys.exit(1)

    generate_header(sys.argv[1], sys.argv[2])
    print(f'Generated {sys.argv[2]} with embedded ZIP font data')

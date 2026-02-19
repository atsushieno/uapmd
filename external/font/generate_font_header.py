#!/usr/bin/env python3
"""
Generate a C++ header file with embedded ZIP font data.

This script creates a .zip of the input TTF file and embeds it as a uint8_t array.
The ZIP format provides compression while the app can extract it at runtime.

Usage: python3 generate_font_header.py [options] input.ttf output.hpp
"""
import argparse
import io
import sys
import zipfile


def generate_header(input_ttf, output_header, array_name, size_name, namespace):
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
        f.write(f'namespace {namespace} {{\n\n')
        f.write(f'inline constexpr uint8_t {array_name}[] = {{\n')

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
        f.write(f'inline constexpr size_t {size_name} = {len(zip_data)};\n\n')
        f.write(f'}} // namespace {namespace}\n')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Embed a TTF file as compressed C++ header data.")
    parser.add_argument('input_ttf', help='Input TTF file')
    parser.add_argument('output_header', help='Output header path')
    parser.add_argument('--array-name', default='kEmbeddedFontData', help='Name of the byte array symbol')
    parser.add_argument('--size-name', default='kEmbeddedFontSize', help='Name of the size symbol')
    parser.add_argument('--namespace', default='uapmd::app', help='Namespace for the generated symbols')

    args = parser.parse_args()

    generate_header(args.input_ttf, args.output_header, args.array_name, args.size_name, args.namespace)
    print(f'Generated {args.output_header} with embedded ZIP font data')

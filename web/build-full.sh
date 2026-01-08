#!/bin/bash

set -e

echo "=== Building uapmd-app for WebAssembly (Full) ==="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Clean previous builds
rm -rf build
mkdir -p build
cd build

echo "Configuring with Emscripten..."
emcmake cmake ..

if [ $? -ne 0 ]; then
    echo "✗ CMake configuration failed"
    exit 1
fi

echo ""
echo "Building..."
cmake --build . --config Release

if [ $? -ne 0 ]; then
    echo "✗ Build failed"
    exit 1
fi

# Generate index.html
cat > index.html <<'EOF'
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>uapmd-app (WebAssembly)</title>
    <style>
        body { 
            margin: 0; 
            padding: 0; 
            background: #1a1a1a; 
            overflow: hidden; 
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
        }
        #loading {
            position: absolute;
            top: 50%;
            left: 50%;
            transform: translate(-50%, -50%);
            color: #ffffff;
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            font-size: 24px;
            text-align: center;
        }
        canvas { 
            display: block; 
            box-shadow: 0 0 20px rgba(0,0,0,0.5);
        }
    </style>
</head>
<body>
    <div id="loading">
        Loading uapmd-app...
    </div>
    <canvas id="canvas"></canvas>
    
    <script>
        var Module = {
            canvas: document.getElementById('canvas'),
            preRun: [
                function() {
                    document.getElementById('loading').style.display = 'none';
                }
            ],
            printErr: function(text) {
                console.error('uapmd-app error:', text);
            },
            print: function(text) {
                console.log('uapmd-app:', text);
            }
        };
    </script>
    <script src="uapmd-app.js" type="text/javascript" async></script>
</body>
</html>
EOF

if [ -f "uapmd-app.js" ] && [ -f "uapmd-app.wasm" ]; then
    echo ""
    echo "✓ Build complete!"
    echo "  - uapmd-app.js ($(du -h uapmd-app.js | cut -f1))"
    echo "  - uapmd-app.wasm ($(du -h uapmd-app.wasm | cut -f1))"
    echo "  - index.html"
    echo ""
    echo "To test:"
    echo "  npx serve -s build -l 8080"
    echo ""
    echo "Then open: http://localhost:8080"
else
    echo "✗ Build failed - missing output files"
    exit 1
fi

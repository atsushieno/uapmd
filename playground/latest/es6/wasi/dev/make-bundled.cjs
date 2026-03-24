let fs = require('fs');

let wasmBytes = fs.readFileSync('../wasi.wasm');
let jsCode = fs.readFileSync('../wasi.mjs', 'utf8');

jsCode = jsCode.replace(/\/\/ inline WASM start.*?\/\/ inline WASM replace: /s, '');
jsCode = jsCode.replace("WASM_BASE64_STRING", JSON.stringify(wasmBytes.toString('base64')));

fs.writeFileSync("../wasi-bundled.mjs", jsCode);

// UAPMD Web Application
// Main JavaScript application logic

class UapmdWebApp {
    constructor() {
        this.wasmModule = null;
        this.wasmExports = null;
        this.uapmdInstance = null;
        this.audioContext = null;
        this.audioWorklet = null;
        this.midiAccess = null;
        this.selectedPluginIndex = -1;
        this.isAudioStarted = false;
        this.volume = 0.8;
        
        this.init();
    }

    async init() {
        try {
            await this.loadWasmModule();
            this.createUapmdInstance();
            this.setupEventListeners();
            this.setupMIDI();
            this.updateStatus('success', 'UAPMD loaded successfully');
            document.getElementById('scanBtn').disabled = false;
        } catch (error) {
            console.error('Initialization error:', error);
            this.updateStatus('error', `Failed to load: ${error.message}`);
        }
    }

    async loadWasmModule() {
        this.updateStatus('loading', 'Loading WebAssembly module...');
        
        const response = await fetch('dist/uapmd.js');
        if (!response.ok) {
            throw new Error('Failed to fetch uapmd.js');
        }
        
        const scriptText = await response.text();
        eval(scriptText);
        
        if (typeof createUapmd === 'undefined') {
            throw new Error('createUapmd function not found');
        }
        
        this.wasmModule = await createUapmd();
        this.updateStatus('loading', 'WebAssembly module loaded');
    }

    createUapmdInstance() {
        if (!this.wasmModule) {
            throw new Error('Wasm module not loaded');
        }
        
        this.wasmExports = this.wasmModule;
        this.uapmdInstance = this.wasmExports._uapmd_create();
        
        if (!this.uapmdInstance) {
            throw new Error('Failed to create UAPMD instance');
        }
    }

    setupEventListeners() {
        document.getElementById('scanBtn').addEventListener('click', () => this.scanPlugins());
        document.getElementById('startAudioBtn').addEventListener('click', () => this.startAudio());
        document.getElementById('stopAudioBtn').addEventListener('click', () => this.stopAudio());
        document.getElementById('volumeSlider').addEventListener('input', (e) => {
            this.volume = e.target.value / 100;
        });
        document.getElementById('midiConnectBtn').addEventListener('click', () => this.connectMIDI());
    }

    async scanPlugins() {
        const result = this.wasmExports._uapmd_scan_plugins(this.uapmdInstance);
        if (result !== 0) {
            alert('Failed to scan plugins');
            return;
        }
        
        const count = this.wasmExports._uapmd_get_plugin_count(this.uapmdInstance);
        this.displayPluginList(count);
    }

    displayPluginList(count) {
        const pluginList = document.getElementById('pluginList');
        pluginList.innerHTML = '';
        
        if (count === 0) {
            pluginList.innerHTML = '<div style="padding: 20px; text-align: center; color: #999;">No plugins found</div>';
            return;
        }
        
        for (let i = 0; i < count; i++) {
            const name = this.wasmExports._uapmd_get_plugin_name(this.uapmdInstance, i);
            const vendor = this.wasmExports._uapmd_get_plugin_vendor(this.uapmdInstance, i);
            const format = this.wasmExports._uapmd_get_plugin_format(this.uapmdInstance, i);
            
            const item = document.createElement('div');
            item.className = 'plugin-item';
            item.innerHTML = `
                <div class="plugin-name">${name}</div>
                <div class="plugin-vendor">${vendor}</div>
                <span class="plugin-format">${format}</span>
            `;
            
            item.addEventListener('click', () => this.selectPlugin(i, item));
            pluginList.appendChild(item);
        }
    }

    selectPlugin(index, element) {
        this.selectedPluginIndex = index;
        
        document.querySelectorAll('.plugin-item').forEach(item => {
            item.classList.remove('selected');
        });
        element.classList.add('selected');
        
        this.loadPlugin(index);
    }

    async loadPlugin(index) {
        const result = this.wasmExports._uapmd_load_plugin(this.uapmdInstance, index);
        if (result !== 0) {
            alert('Failed to load plugin');
            return;
        }
        
        this.displayPluginEditor(index);
    }

    displayPluginEditor(index) {
        const editorContent = document.getElementById('editorContent');
        const name = this.wasmExports._uapmd_get_plugin_name(this.uapmdInstance, index);
        const paramCount = this.wasmExports._uapmd_get_parameter_count(this.uapmdInstance);
        
        let html = `<h3>${name}</h3>`;
        
        if (paramCount > 0) {
            html += '<div class="parameters">';
            for (let i = 0; i < paramCount; i++) {
                const paramName = this.wasmExports._uapmd_get_parameter_name(this.uapmdInstance, i);
                const paramValue = this.wasmExports._uapmd_get_parameter_value(this.uapmdInstance, i);
                
                html += `
                    <div class="parameter">
                        <label>${paramName}</label>
                        <input type="range" min="0" max="1" step="0.01" value="${paramValue}" 
                               data-param-index="${i}" class="parameter-slider">
                        <div class="value">${paramValue.toFixed(4)}</div>
                    </div>
                `;
            }
            html += '</div>';
        } else {
            html += '<p style="padding: 20px; text-align: center; color: #999;">No parameters available</p>';
        }
        
        editorContent.innerHTML = html;
        
        document.querySelectorAll('.parameter-slider').forEach(slider => {
            slider.addEventListener('input', (e) => {
                const paramIndex = parseInt(e.target.dataset.paramIndex);
                const value = parseFloat(e.target.value);
                this.wasmExports._uapmd_set_parameter_value(this.uapmdInstance, paramIndex, value);
                e.target.nextElementSibling.textContent = value.toFixed(4);
            });
        });
        
        document.getElementById('startAudioBtn').disabled = false;
    }

    async startAudio() {
        if (this.isAudioStarted) return;
        
        try {
            this.audioContext = new (window.AudioContext || window.webkitAudioContext)({ 
                sampleRate: 48000 
            });
            
            await this.audioContext.resume();
            
            const bufferSize = 512;
            const result = this.wasmExports._uapmd_start_audio(
                this.uapmdInstance,
                this.audioContext.sampleRate,
                bufferSize
            );
            
            if (result !== 0) {
                throw new Error('Failed to start audio');
            }
            
            this.createAudioWorklet(bufferSize);
            this.isAudioStarted = true;
            document.getElementById('startAudioBtn').disabled = true;
            document.getElementById('stopAudioBtn').disabled = false;
            
        } catch (error) {
            console.error('Audio start error:', error);
            alert(`Failed to start audio: ${error.message}`);
        }
    }

    createAudioWorklet(bufferSize) {
        const script = `
            class UapmdProcessor extends AudioWorkletProcessor {
                constructor() {
                    super();
                    this.bufferSize = ${bufferSize};
                    this.inputPtr = 0;
                    this.outputPtr = 0;
                }
                
                process(inputs, outputs, parameters) {
                    const input = inputs[0];
                    const output = outputs[0];
                    
                    if (!input || !output || input[0].length === 0) {
                        return true;
                    }
                    
                    const inputChannel = input[0];
                    const outputChannel = output[0];
                    
                    for (let i = 0; i < inputChannel.length; i++) {
                        outputChannel[i] = inputChannel[i];
                    }
                    
                    return true;
                }
            }
            
            registerProcessor('uapmd-processor', UapmdProcessor);
        `;
        
        const blob = new Blob([script], { type: 'application/javascript' });
        const url = URL.createObjectURL(blob);
        
        this.audioContext.audioWorklet.addModule(url).then(() => {
            const workletNode = new AudioWorkletNode(this.audioContext, 'uapmd-processor');
            workletNode.connect(this.audioContext.destination);
            this.audioWorklet = workletNode;
        }).catch(error => {
            console.error('Worklet error:', error);
        });
    }

    stopAudio() {
        if (!this.isAudioStarted) return;
        
        if (this.audioWorklet) {
            this.audioWorklet.disconnect();
            this.audioWorklet = null;
        }
        
        if (this.audioContext) {
            this.audioContext.close();
            this.audioContext = null;
        }
        
        this.wasmExports._uapmd_stop_audio(this.uapmdInstance);
        this.isAudioStarted = false;
        document.getElementById('startAudioBtn').disabled = false;
        document.getElementById('stopAudioBtn').disabled = true;
    }

    setupMIDI() {
        if (navigator.requestMIDIAccess) {
            navigator.requestMIDIAccess().then(
                (access) => {
                    this.midiAccess = access;
                    this.midiAccess.onstatechange = () => this.handleMIDIStateChange();
                },
                (err) => {
                    console.log('MIDI access denied:', err);
                }
            );
        } else {
            console.log('Web MIDI API not supported');
        }
    }

    async connectMIDI() {
        try {
            const access = await navigator.requestMIDIAccess();
            const inputs = access.inputs;
            
            document.getElementById('midiMonitor').innerHTML = '';
            
            if (inputs.size === 0) {
                document.getElementById('midiMonitor').innerHTML = 
                    '<div style="padding: 10px; color: #999;">No MIDI devices found</div>';
                return;
            }
            
            inputs.forEach((input) => {
                input.onmidimessage = (event) => this.handleMIDIMessage(event);
            });
            
        } catch (error) {
            alert(`Failed to connect MIDI: ${error.message}`);
        }
    }

    handleMIDIMessage(event) {
        const monitor = document.getElementById('midiMonitor');
        const data = event.data;
        const hex = Array.from(data).map(b => b.toString(16).padStart(2, '0')).join(' ');
        
        const message = document.createElement('div');
        message.className = 'midi-message';
        message.textContent = `[${new Date().toLocaleTimeString()}] ${hex}`;
        
        if (monitor.firstChild) {
            monitor.insertBefore(message, monitor.firstChild);
        } else {
            monitor.appendChild(message);
        }
        
        const inputPtr = this.wasmModule._malloc(data.length);
        const inputArray = new Uint8Array(this.wasmModule.HEAPU8.buffer, inputPtr, data.length);
        inputArray.set(data);
        
        this.wasmExports._uapmd_send_midi(this.uapmdInstance, inputPtr, data.length);
        this.wasmModule._free(inputPtr);
    }

    handleMIDIStateChange() {
        console.log('MIDI device state changed');
    }

    updateStatus(type, message) {
        const statusEl = document.getElementById('status');
        statusEl.className = `status ${type}`;
        statusEl.textContent = message;
    }
}

let app;

document.addEventListener('DOMContentLoaded', () => {
    app = new UapmdWebApp();
});

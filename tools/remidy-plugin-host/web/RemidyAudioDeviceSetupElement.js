// To see all registered functions on `window`, see "WebView Facade stubs".
// To see all custom events on `window`, see "Window events".

// WebView facade stubs.
async function remidy_getDevices_stub() {
    return JSON.stringify({
        "snapshotTime": 0,
        "audioIn": [
            { id: "0_0", name: "stub audio in JS 0" },
            { id: "1_0", name: "stub audio in JS 1" }
        ],
        "audioOut": [
            { id: "0_1", name: "stub audio out JS 0" },
            { id: "1_1", name: "stub audio out JS 1" }
        ],
        "midiIn": [
            { id: "0_0", name: "stub MIDI in JS 0" }
        ],
        "midiOut": [
            { id: "0_1", name: "stub MIDI out JS 0" }
        ]
    });
}

// WebView Facades. They are supposed to be defined by host WebView.
if (typeof(remidy_getDevices) === "undefined")
    globalThis.remidy_getDevices = remidy_getDevices_stub;

// Events that are fired by WebView.
class RemidyDevicesUpdatedListener {
    owner = {};

    constructor(element) {
        this.owner = element;
    }

    async handleEvent(evt) {
        await this.owner.loadDeviceList();
    }
}

class RemidyAudioDeviceSetupElement extends HTMLElement {

    // Window events that UI listens to the host state changes.
    remidyDeviceUpdatedListener = new RemidyDevicesUpdatedListener(this);

    // part of WebComponents Standard
    async connectedCallback() {
        // JS event registry
        window.addEventListener("remidyDevicesUpdated", this.remidyDeviceUpdatedListener);

        this.innerHTML = `
            <sl-details summary="Device Settings">
                <sl-select class="audioInSelector" hoist="hoist">
                    <sl-option>stub audio in</sl-option>
                </sl-select>
                <sl-select class="audioOutSelector" hoist="hoist">
                    <sl-option>stub audio out</sl-option>
                </sl-select>
                <sl-select class="midiInSelector" hoist="hoist">
                    <sl-option>stub midi in</sl-option>
                </sl-select>
                <sl-select class="midiOutSelector" hoist="hoist">
                    <sl-option>stub midi out</sl-option>
                </sl-select>
            </sl-details>
`;

        await this.loadDeviceList();
    }

    async loadDeviceList() {
        const devicesJSON = await remidy_getDevices();
        const devices = JSON.parse(devicesJSON);
        const audioIn = document.querySelector(".audioInSelector");
        const audioOut = document.querySelector(".audioOutSelector");
        const midiIn = document.querySelector(".midiInSelector");
        const midiOut = document.querySelector(".midiOutSelector");
        const arr = [
            [audioIn, devices.audioIn],
            [audioOut, devices.audioOut],
            [midiIn, devices.midiIn],
            [midiOut, devices.midiOut]
        ];
        for (const p in arr) {
            const node = arr[p][0];
            const deviceList = arr[p][1];
            node.innerText = "";
            for (const i in deviceList) {
                const d = deviceList[i];
                if (d.id === "")
                    continue;
                const el = document.createElement("sl-option");
                el.setAttribute("value", d.id);
                el.innerText = d.name;
                node.appendChild(el);
                if (node.getAttribute("value") == null) {
                    node.setAttribute("value", d.id);
                }
            }
        }
    }
}

customElements.define("remidy-audio-device-setup", RemidyAudioDeviceSetupElement);

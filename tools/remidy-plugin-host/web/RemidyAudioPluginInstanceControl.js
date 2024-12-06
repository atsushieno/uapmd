// To see all registered functions on `window`, see "WebView Facade stubs".
// To see all custom events on `window`, see "Window events".

// WebView facade stubs.

function remidy_instantiatePlugin_stub(instancingId, format, pluginId) {
    console.log(`Instantiated plugin(${instancingId}, ${format}, ${pluginId})`);
}

async function remidy_getPluginParameterList_stub(jsonArgs) {
    return JSON.stringify([
        { "id": "1", "name": "para1", "initialValue": 0 },
        { "id": "2", "name": "para2", "initialValue": 1 },
        { "id": "3", "name": "para3", "initialValue": 0.5 },
    ]);
}
async function remidy_sendNoteOn_stub(jsonArgs) {
    const args = JSON.parse(jsonArgs)
    console.log(`note on: instance ${args.instanceId}: note ${args.note}`)
}
async function remidy_sendNoteOff_stub(jsonArgs) {
    const args = JSON.parse(jsonArgs)
    console.log(`note off: instance ${args.instanceId}: note ${args.note}`)
}

// WebView Facades. They are supposed to be defined by host WebView.
if (typeof(remidy_instantiatePlugin) === "undefined")
    globalThis.remidy_instantiatePlugin = remidy_instantiatePlugin_stub;
if (typeof(remidy_getPluginParameterList) === "undefined")
    globalThis.remidy_getPluginParameterList = remidy_getPluginParameterList_stub;
if (typeof(remidy_sendNoteOn) === "undefined")
    globalThis.remidy_sendNoteOn = remidy_sendNoteOn_stub;
if (typeof(remidy_sendNoteOff) === "undefined")
    globalThis.remidy_sendNoteOff = remidy_sendNoteOff_stub;


// Events that are fired by native.
class RemidyInstancingCompletedEvent extends Event {
    constructor(instancingId, instanceId) {
        super('RemidyInstancingCompleted');
        this.instancingId = instancingId
        this.instanceId = instanceId;
    }
}

class RemidyAudioPluginInstanceControlElement extends HTMLElement {
    constructor() {
        super();
    }

    instanceId = -1;
    instancingCount = 0;
    waitingForInstancing = false;

    async sendNoteOn(instanceId, note) {
        await remidy_sendNoteOn(JSON.stringify({instanceId: instanceId, note: note}));
    }
    async sendNoteOff(instanceId, note) {
        await remidy_sendNoteOff(JSON.stringify({instanceId: instanceId, note: note}));
    }

    // part of WebComponents Standard
    async connectedCallback() {
        const self = this;

        // JS event registry
        window.addEventListener("RemidyInstancingCompleted", async (evt) => {
            console.log(`InstancingId ${evt.instancingId} is done as instanceId ${evt.instanceId}.`);
            self.instanceId = evt.instanceId;
            self.waitingForInstancing = false;

            const list = this.querySelector("remidy-audio-plugin-list");
            const jsonReq = JSON.stringify({instanceId: this.instanceId, format: list.format, pluginId: list.pluginId})
            const parameterListJSON = await remidy_getPluginParameterList(jsonReq);
            this.parameterList = JSON.parse(parameterListJSON);
            await this.renderPluginParameterList();
        });

        this.innerHTML = `
            <sl-details summary="Instance Control">
                <sl-dialog label="Select a plugin to instantiate" style="--width: 90vw">
                    <div style="overflow: auto; height: 500px">
                        <remidy-audio-plugin-list></remidy-audio-plugin-list>
                        <sl-button slot="footer" variant="primary">Close</sl-button>
                    </div>
                </sl-dialog>
                <sl-button class="plugin-list-launcher">Select a Plugin</sl-button>
                
                <div>
                    <webaudio-keyboard class="plugin-midi-keyboard" width="640" height="80" keys="80"></webaudio-keyboard>
                </div>

                <sl-details summary="Parameters" class="parameters">
                </sl-details>
            </sl-details>
`;
        this.querySelector("sl-button.plugin-list-launcher").addEventListener("click", () => {
            this.pluginListLauncherClicked();
        });

        this.querySelector("sl-button[slot='footer']").addEventListener("click", () => {
            this.querySelector("sl-dialog").hide();
        });

        this.querySelector("webaudio-keyboard.plugin-midi-keyboard").addEventListener("change", (e) => {
            const n = e.note;
            if (n[0])
                this.sendNoteOn(this.instanceId, n[1]);
            else
                this.sendNoteOff(this.instanceId, n[1]);
        });

        this.parameterList = JSON.parse(await remidy_getPluginParameterList_stub(""));

        this.querySelector("remidy-audio-plugin-list").addEventListener("RemidyAudioPluginListSelectionChanged", (evt) => {
            if (this.waitingForInstancing) {
                console.log("Another instancing is ongoing.");
                window.alert("Another instancing is ongoing.");
                return;
            }
            this.querySelector("sl-dialog").hide();
            this.waitingForInstancing = true;
            this.instantiatePlugin(this.instancingCount++, evt.format, evt.pluginId);
        });

        await this.renderPluginParameterList();
    }

    pluginListLauncherClicked() {
        this.querySelector("sl-dialog").show();
    }

    instantiatePlugin(instancingId, format, pluginId) {
        const obj = {instancingId: instancingId, format: format, pluginId: pluginId};
        const json = JSON.stringify(obj);
        console.log(`instantiatePlugin(${json})`);
        remidy_instantiatePlugin(json);
    }

    async renderPluginParameterList() {
        const parameters = this.parameterList;
        const node = document.querySelector(".parameters");
        node.innerHTML = "";

        const actionTable = document.createElement("action-table");
        actionTable.setAttribute("store", "store");
        actionTable.innerHTML = `
            <action-table-filters class="flex flex-col">
                <div>Filter: <input id="name-search" name="action-table" type="search" placeholder="Search" size="30"/>
                </div>
            </action-table-filters>
        `;

        const table = document.createElement("table")
        table.innerHTML = `
            <thead>
                <tr>
                    <th>Name</th>
                    <th>Value</th>
                </tr>
            </thead>
            <tbody>
            </tbody>
        `;

        const tbody = table.querySelector("tbody");
        let listHtml = "";
        // FIXME: it's too slow. Can we make it a lazy list?
        for (const i in parameters) {
            const d = parameters[i];
            if (d.hidden)
                continue;
            listHtml += `
            <tr>
                <td>
                    <sl-visually-hidden>${d.id}</sl-visually-hidden>
                    <span>${d.name}</span>
                </td>
                <td>
                    <sl-range value="${d.initialValue}" min="0.0" max="1.0" step="0.01"></sl-range>
                </td>
            </tr>
            `
        }
        tbody.innerHTML = listHtml;
        actionTable.appendChild(table);
        node.appendChild(actionTable);
    }
}

customElements.define("remidy-audio-plugin-instance-control", RemidyAudioPluginInstanceControlElement);

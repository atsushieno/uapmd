// To see all registered functions on `window`, see "WebView Facade stubs".
// To see all custom events on `window`, see "Window events".

// WebView facade stubs.

function remidy_instantiatePlugin_stub(instancingId, format, pluginId) {
    console.log(`Instantiated plugin(${instancingId}, ${format}, ${pluginId})`);
}

async function remidy_getPluginParameterList_stub() {
    return JSON.stringify([
        { "id": "1", "name": "para1" },
        { "id": "2", "name": "para2" },
        { "id": "3", "name": "para3" },
    ]);
}

// WebView Facades. They are supposed to be defined by host WebView.
if (typeof(remidy_instantiatePlugin) === "undefined")
    globalThis.remidy_instantiatePlugin = remidy_instantiatePlugin_stub;
if (typeof(remidy_getPluginParameterList) === "undefined")
    globalThis.remidy_getPluginParameterList = remidy_getPluginParameterList_stub;

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

    // part of WebComponents Standard
    async connectedCallback() {
        const self = this;

        // JS event registry
        window.addEventListener("RemidyInstancingCompleted", (evt) => {
            console.log(`InstancingId ${evt.instancingId} is done as instanceId ${evt.instanceId}.`);
            self.instanceId = evt.instanceId;
            self.waitingForInstancing = false;
            const list = self.querySelector("remidy-audio-plugin-list");
            remidy_getPluginParameterList(self.instanceId, list.format, list.pluginId);
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

        let instanceId = -1;
        let instancingCount = 0;
        let waitingForInstancing = false;

        this.querySelector("remidy-audio-plugin-list").addEventListener("RemidyAudioPluginListSelectionChanged", (evt) => {
            if (this.waitingForInstancing) {
                console.log("Another instancing is ongoing.");
                window.alert("Another instancing is ongoing.");
                return;
            }
            this.querySelector("sl-dialog").hide();
            this.waitingForInstancing = true;
            this.instantiatePlugin(instancingCount++, evt.format, evt.pluginId);
        });

        await this.getPluginParameterList();
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

    async getPluginParameterList() {
        const parameterListJSON = await remidy_getPluginParameterList();
        const parameters = JSON.parse(parameterListJSON);
        const node = document.querySelector(".parameters");

        const wrapper = document.createElement("div");
        wrapper.innerHTML = `
            <action-table store="store" class="parameters-action-table"
                <action-table-filters class="flex flex-col">
                    <div>Filter: <input id="name-search" name="action-table" type="search" placeholder="Search" size="30"/>
                    </div>
                </action-table-filters>
            </action-table>
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
        for (const i in parameters) {
            const d = parameters[i];
            const tr = document.createElement("tr");
            tr.innerHTML = `
                <td>
                    <sl-visually-hidden>${d.id}</sl-visually-hidden>
                    <span>${d.name}</span>
                </td>
                <td>
                    <sl-range value="${d.value}"></sl-range>
                </td>
            `
            tbody.appendChild(tr);
        }
        let actionTable = wrapper.removeChild(wrapper.querySelector("action-table"));
        actionTable.appendChild(table);
        node.appendChild(actionTable);
    }
}

customElements.define("remidy-audio-plugin-instance-control", RemidyAudioPluginInstanceControlElement);

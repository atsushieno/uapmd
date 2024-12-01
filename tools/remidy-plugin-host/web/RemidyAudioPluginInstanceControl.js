// To see all registered functions on `window`, see "WebView Facade stubs".
// To see all custom events on `window`, see "Window events".

// WebView facade stubs.

function remidy_instantiatePlugin_stub(format, pluginId) {
    console.log(`Instantiated plugin(${format}, ${pluginId})`);
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

// Events that are fired by WebView.
class RemidyAudioPluginInstanceControlListener {
    owner = {};

    constructor(element) {
        this.owner = element;
    }

    async handleEvent(evt) {
        await this.owner.unhandledEvent();
    }
}

class RemidyAudioPluginInstanceControlElement extends HTMLElement {
    constructor() {
        super();
    }

    // Window events that UI listens to the host state changes.
    remidyInstanceControlListener = new RemidyAudioPluginInstanceControlListener(this);

    // part of WebComponents Standard
    async connectedCallback() {
        // JS event registry
        window.addEventListener("RemidyInstanceControl", this.remidyInstanceControlListener);

        this.innerHTML = `
            <sl-details summary="Parameters" class="parameters">
            </sl-details>
`;

        await this.getPluginParameterList();
    }

    instantiatePlugin(format, pluginId) {
        const obj = {format: format, pluginId: pluginId};
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

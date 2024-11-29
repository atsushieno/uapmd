// To see all registered functions on `window`, see "WebView Facade stubs".
// To see all custom events on `window`, see "Window events".

// WebView facade stubs.

var plugin_scan_count = 1;
async function remidy_getAudioPluginEntryList_stub() {
    return JSON.stringify({
        "entries": [
            {
                "format": "VST3",
                "id": "dummy_id",
                "name": "non-existent VST3 plugin " + plugin_scan_count,
                "vendor": "ABC"
            },
            {
                "format": "LV2",
                "id": "dummy_id",
                "name": "non-existent LV2 plugin " + plugin_scan_count,
                "vendor": "DEF"
            },
            {
                "format": "AU",
                "id": "dummy_id",
                "name": "non-existent AU plugin " + plugin_scan_count,
                "vendor": "XYZ"
            }
        ],
        "denyList": []
    });
}

async function remidy_performPluginScanning_stub(forceRescan/*string*/) {
    plugin_scan_count++;
}

// WebView Facades. They are supposed to be defined by host WebView.
if (typeof(remidy_getAudioPluginEntryList) === "undefined")
    remidy_getAudioPluginEntryList = remidy_getAudioPluginEntryList_stub;
if (typeof(remidy_performPluginScanning) === "undefined")
    remidy_performPluginScanning = remidy_performPluginScanning_stub;

// Events that are fired by WebView.
class RemidyAudioPluginListUpdatedListener {
    owner = {};

    constructor(element) {
        this.owner = element;
    }

    async handleEvent(evt) {
        await this.owner.loadAudioPluginEntryList();
    }
}

class RemidyAudioPluginEntryListElement extends HTMLElement {
    constructor() {
        super();
    }

    // Window events that UI listens to the host state changes.
    remidyAudioPluginListUpdatedListener = new RemidyAudioPluginListUpdatedListener(this);

    // part of WebComponents Standard
    async connectedCallback() {
        // JS event registry
        window.addEventListener("remidyAudioPluginListUpdated", this.remidyAudioPluginListUpdatedListener);

        this.innerHTML = `
            <sl-details summary="Audio Plugin List" class="entries">
            </sl-details>
            <sl-details summary="DenyList" class="denyList">
            </sl-details>
            <sl-button onclick="this.parentElement.performPluginScanning()">Perform Plugin Scanning</sl-button>
            <sl-checkbox id="checkbox-force-rescan"></sl-checkbox> Force Rescan
`;

        await this.loadAudioPluginEntryList();
    }

    async loadAudioPluginEntryList() {
        const pluginListJSON = await remidy_getAudioPluginEntryList();
        const pluginList = JSON.parse(pluginListJSON);
        const node = document.querySelector(".entries");
        node.innerText = "";
        const actionTable = document.createElement("action-table");
        actionTable.setAttribute("store", "store");
        actionTable.innerHTML = `
            <action-table-filters class="flex flex-col">
                <div>Filter: <input id="name-search" name="action-table" type="search" placeholder="Search" size="30" /></div>
            </action-table-filters>
        `;
        const table = document.createElement("table");
        table.innerHTML = `
            <thead>
                <tr>
                    <th>Format</th>
                    <th>Name</th>
                    <th>Vendor</th>
                </tr>
            </thead>
        `;
        const tbody = document.createElement("tbody");
        for (const i in pluginList.entries) {
            const d = pluginList.entries[i];
            const el = document.createElement("tr");
            el.setAttribute("value", i);
            el.innerHTML = `
                    <td>${d.format}</td>
                    <td>${d.name}</td>
                    <td>${d.vendor}</td>
            `;
            tbody.appendChild(el);
            if (node.getAttribute("value") == null) {
                node.setAttribute("value", i);
            }
        }
        // It needs to be added after we filled all rows, otherwise action-table validates the table and rejects tr-less tables.
        table.appendChild(tbody);
        actionTable.appendChild(table);
        node.appendChild(actionTable);
    }

    async performPluginScanning() {
        const rescan = this.querySelector("sl-checkbox");
        await remidy_performPluginScanning(rescan.checked.toString());
    }
}

customElements.define("remidy-audio-plugin-list", RemidyAudioPluginEntryListElement);

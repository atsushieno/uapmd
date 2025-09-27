// To see all registered functions on `window`, see "WebView Facade stubs".
// To see all custom events on `window`, see "Window events".

// WebView facade stubs.

let plugin_scan_count = 1;

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
    globalThis.remidy_getAudioPluginEntryList = remidy_getAudioPluginEntryList_stub;
if (typeof(remidy_performPluginScanning) === "undefined")
    globalThis.remidy_performPluginScanning = remidy_performPluginScanning_stub;

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

class RemidyAudioPluginListSelectionChangedEvent extends Event {
    constructor(format, pluginId) {
        super("RemidyAudioPluginListSelectionChanged");

        this.format = format;
        this.pluginId = pluginId;
    }
}

class RemidyAudioPluginEntryListElement extends HTMLElement {
    constructor() {
        super();
    }

    selectedFormat = "";
    selectedPluginId = "";

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
            <sl-checkbox id="checkbox-force-rescan"></sl-checkbox>
            <label for="checkbox-force-rescan">Rescan</label>
`;

        await this.loadAudioPluginEntryList();
    }

    async loadAudioPluginEntryList() {
        const me = this;

        const pluginListJSON = await remidy_getAudioPluginEntryList();
        const pluginList = JSON.parse(pluginListJSON);
        const node = me.querySelector(".entries");
        node.innerText = "";
        const wrapper = document.createElement("div");
        wrapper.setAttribute("style", "overflow: auto; height: 500px");
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
                    <th>Select</th>
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
            el.innerHTML = `
                    <td><sl-button pluginId="${d.id}" format="${d.format}" class="plugin-list-item-selector">Select</sl-button></td>
                    <td>${d.format}</td>
                    <td>${d.name}</td>
                    <td>${d.vendor}</td>
            `;
            tbody.appendChild(el);
        }
        // It needs to be added after we filled all rows, otherwise action-table validates the table and rejects tr-less tables.
        table.appendChild(tbody);
        actionTable.appendChild(table);
        wrapper.appendChild(actionTable);
        node.appendChild(wrapper);
        me.querySelectorAll(".plugin-list-item-selector").forEach(e => {
            e.addEventListener("click", () => {
                const format = e.getAttribute("format");
                const pluginId = e.getAttribute("pluginId");
                const name = e.getAttribute("name");
                this.selectPlugin(format, pluginId);
            });
        });
    }

    async performPluginScanning() {
        const rescan = this.querySelector("sl-checkbox");
        await remidy_performPluginScanning(rescan.checked.toString());
    }

    selectPlugin(format, pluginId) {
        const me = this;
        this.selectedFormat = format;
        this.selectedPluginId = pluginId;
        this.dispatchEvent(new RemidyAudioPluginListSelectionChangedEvent(format, pluginId));

        this.querySelectorAll("tr").forEach(e => {
            e.removeAttribute("class");
            const td = e.querySelector("td");
            if (td == null) // header row
                return;
            const b = td.querySelector("sl-button");
            if (b.getAttribute("format") === me.selectedFormat &&
                b.getAttribute("pluginId") === me.selectedPluginId)
                e.setAttribute("class", "selected");
        });
    }
}

customElements.define("remidy-audio-plugin-list", RemidyAudioPluginEntryListElement);

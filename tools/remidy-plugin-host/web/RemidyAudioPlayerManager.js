// WebView facade stubs.

async function remidy_startAudio_stub() {
    return true;
}
async function remidy_stopAudio_stub() {
    return true;
}
async function remidy_isAudioPlaying_stub() {
    return false;
}
async function remidy_getSampleRate_stub() {
    return 44100;
}
async function remidy_setSampleRate_stub(sampleRate) {
    return true;
}

// WebView Facades. They are supposed to be defined by host WebView.
if (typeof(remidy_startAudio) === "undefined")
    globalThis.remidy_startAudio = remidy_startAudio_stub;
if (typeof(remidy_stopAudio) === "undefined")
    globalThis.remidy_stopAudio = remidy_stopAudio_stub;
if (typeof(remidy_isAudioPlaying) === "undefined")
    globalThis.remidy_isAudioPlaying = remidy_isAudioPlaying_stub;
if (typeof(remidy_getSampleRate) === "undefined")
    globalThis.remidy_getSampleRate = remidy_getSampleRate_stub;
if (typeof(remidy_setSampleRate) === "undefined")
    globalThis.remidy_setSampleRate = remidy_setSampleRate_stub;

class RemidyAudioPlayerManagerElement extends HTMLElement {

    startAudio() {
        return remidy_startAudio();
    }
    stopAudio() {
        return remidy_stopAudio();
    }
    isAudioPlaying() {
        return remidy_isAudioPlaying();
    }
    async getSampleRate() {
        return remidy_getSampleRate();
    }
    async setSampleRate(sampleRate) {
        return remidy_setSampleRate(sampleRate);
    }

    // part of WebComponents Standard
    async connectedCallback() {
        const me = this;

        this.innerHTML = `
            <sl-details summary="Player Settings">
                <sl-select class="remidy-sample-rate-selector" value="44100" hoist="true" label="Sample Rate">
                    <sl-option value="11025">11025</sl-option>
                    <sl-option value="22050">22050</sl-option>
                    <sl-option value="44100">44100</sl-option>
                    <sl-option value="48000">48000</sl-option>
                </sl-select>
                <sl-switch class="remidy-player-audio-switch">Play</sl-switch>
            </sl-details>
        `;

        const sampleRateSelector = this.querySelector("sl-select.remidy-sample-rate-selector");
        if (me.isAudioPlaying())
            sampleRateSelector.value = await me.getSampleRate();
        sampleRateSelector.addEventListener("sl-change", () => {
            me.setSampleRate(sampleRateSelector.value);
        });

        const playerSwitch = this.querySelector("sl-switch.remidy-player-audio-switch")
        playerSwitch.addEventListener("sl-change", () =>{
            if (playerSwitch.checked)
                me.startAudio();
            else
                me.stopAudio();
        });
    }
}

customElements.define("remidy-audio-player-manager", RemidyAudioPlayerManagerElement);

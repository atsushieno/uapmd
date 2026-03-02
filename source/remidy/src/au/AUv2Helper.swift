// FIXME: we should not need this import, but removing this causes undefined symbols for AVFoundation functions.
import AVFoundation
import AudioToolbox

public func scanAllAvailableAudioUnits() -> [AVAudioUnitComponent] {
    var ret: [AVAudioUnitComponent] = []
    let manager = AVAudioUnitComponentManager.shared()
    let components = manager.components(passingTest: { _, _ in true })
    for component in components {
        let desc = component.audioComponentDescription
        switch (desc.componentType) {
            case kAudioUnitType_MusicDevice,
             kAudioUnitType_Effect,
             kAudioUnitType_Generator,
             kAudioUnitType_MusicEffect,
             kAudioUnitType_MIDIProcessor:
                break;
            default:
                continue;
        }
        let name = component.name;
        if let firstColon = name.first(where: { $0 == ":" }) {
        /*
            let vendor = name.Substring(0, firstColon);
            let pluginName = name.Substring(firstColon + 2); // remaining after ": "
            let id = String(format: "%02x %02x %02x", desc.manufacturer, desc.componentType, desc.componentSubType)
            */
            //ret.append(AUPluginEntry(component, desc.componentFlags, id.str(), pluginName, vendor))
            ret.append(component)
        }
    }
    return ret
}

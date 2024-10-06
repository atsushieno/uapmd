// FIXME: we should not need this import, but removing this causes undefined symbols for AVFoundation functions.
import AVFoundation

/*
public func scanAllAvailableAudioUnits() {
    print("Test interop function")
    print(AVAudioUnitTypeMusicEffect)
    var manager = AVAudioUnitComponentManager.shared()
    var components = manager.components(passingTest: { _, _ in true })
    for component in components {
        print(component.name)
    }
}
*/
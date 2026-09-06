package dev.celtera.libremidi;

import android.media.midi.MidiDevice;
import android.media.midi.MidiManager;
import android.util.Log;

// The package, constructor and native method signatures are used by libremidi's JNI backend.
public final class MidiDeviceCallback implements MidiManager.OnDeviceOpenedListener {
    private final long nativePtr;
    private final boolean isOutput;

    public MidiDeviceCallback(long nativePtr, boolean isOutput) {
        this.nativePtr = nativePtr;
        this.isOutput = isOutput;
    }

    @Override
    public void onDeviceOpened(MidiDevice device) {
        if (device == null) {
            Log.e("libremidi", "Failed to open MIDI device");
            return;
        }
        onDeviceOpened(device, nativePtr, isOutput);
    }

    // SDL loads libmain (including libremidi's JNI implementation) before MIDI ports are opened.
    private native void onDeviceOpened(MidiDevice device, long nativePtr, boolean isOutput);
}

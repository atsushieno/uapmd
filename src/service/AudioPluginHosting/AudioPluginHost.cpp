#include "AudioPluginHost.hpp"
#include "AudioPluginHost.hpp"

class AudioPluginHost::Impl {
  std::vector<AudioPluginTrack*> tracks{};
public:
  AudioPluginTrack* getTrack(int32_t index);

  int32_t processAudio(AudioBufferList * audio_buffers, MidiSequence * midi_sequence);
};

AudioPluginHost::AudioPluginHost() {
  impl = new Impl();
}

AudioPluginHost::~AudioPluginHost() {
  delete impl;
}

AudioPluginTrack * AudioPluginHost::getTrack(int32_t index) {
  return impl->getTrack(index);
}

uapmd_status_t AudioPluginHost::processAudio(AudioBufferList *audioBufferList, MidiSequence *midiSequence) {
  // FIXME: we should actually implement UMP dispatching to each track
  return impl->processAudio(audioBufferList, midiSequence);
}

AudioPluginTrack* AudioPluginHost::Impl::getTrack(int32_t index) {
  return tracks[index];
}

int32_t AudioPluginHost::Impl::processAudio(AudioBufferList *audio_buffers, MidiSequence *midi_sequence) {
}

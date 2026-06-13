#ifndef FLUTTER_WEBRTC_BASE_HXX
#define FLUTTER_WEBRTC_BASE_HXX

#include "flutter_common.h"

#include <string.h>
#include <list>
#include <map>
#include <memory>
#include <mutex>

#include "libwebrtc.h"

#include "rtc_audio_device.h"
#include "rtc_audio_processing.h"
#include "rtc_desktop_device.h"
#include "rtc_dtmf_sender.h"
#include "rtc_frame_cryptor.h"
#include "rtc_media_stream.h"
#include "rtc_media_track.h"
#include "rtc_mediaconstraints.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_video_device.h"

#include "rnnoise_processor.h"  // [chatpapol] supresor de ruido RNNoise
#include "per_user_eq.h"        // [chatpapol] EQ por-usuario (sinks por pista)

namespace flutter_webrtc_plugin {

using namespace libwebrtc;

class FlutterVideoRenderer;
class FlutterRTCDataChannelObserver;
class FlutterPeerConnectionObserver;
class MicCapturer;  // [chatpapol 48k] capturador de micro nativo (mic_capturer.h)

class FlutterWebRTCBase {
 public:
  friend class FlutterMediaStream;
  friend class FlutterPeerConnection;
  friend class FlutterVideoRendererManager;
  friend class FlutterDataChannel;
  friend class FlutterPeerConnectionObserver;
  friend class FlutterScreenCapture;
  friend class FlutterFrameCryptor;
  friend class FlutterDataPacketCryptor;
  enum ParseConstraintType { kMandatory, kOptional };

 public:
  FlutterWebRTCBase(BinaryMessenger* messenger,
                    TextureRegistrar* textures,
                    TaskRunner* task_runner);
  ~FlutterWebRTCBase();

  virtual scoped_refptr<RTCAudioProcessing> audio_processing() {
    return audio_processing_;
  }

  // [chatpapol] supresor de ruido RNNoise (capture post-processing). El toggle
  // lo conecta/desconecta via SetCapturePostProcessing; aquí solo vive su dueño.
  chatpapol::RnnoiseProcessor* rnnoise_processor() {
    return rnnoise_processor_.get();
  }

  // [chatpapol] Router de EQ por-usuario: dueño de los sinks (uno por pista
  // remota con EQ activo). Vacío salvo que haya algún EQ activo → cero coste y
  // cero riesgo en el caso normal. Ver per_user_eq.h / flutter_webrtc.cc.
  chatpapol::PerUserEqRouter* per_user_eq() { return per_user_eq_.get(); }

  virtual scoped_refptr<RTCMediaTrack> MediaTrackForId(const std::string& id);

  std::string GenerateUUID();

  RTCPeerConnection* PeerConnectionForId(const std::string& id);

  void RemovePeerConnectionForId(const std::string& id);

  void RemoveMediaTrackForId(const std::string& id);

  FlutterPeerConnectionObserver* PeerConnectionObserversForId(
      const std::string& id);

  void RemovePeerConnectionObserversForId(const std::string& id);

  scoped_refptr<RTCMediaStream> MediaStreamForId(
      const std::string& id,
      std::string ownerTag = std::string());

  void RemoveStreamForId(const std::string& id);

  bool ParseConstraints(const EncodableMap& constraints,
                        RTCConfiguration* configuration);

  scoped_refptr<RTCMediaConstraints> ParseMediaConstraints(
      const EncodableMap& constraints);

  bool ParseRTCConfiguration(const EncodableMap& map,
                             RTCConfiguration& configuration);

  scoped_refptr<RTCMediaTrack> MediaTracksForId(const std::string& id);

  void RemoveTracksForId(const std::string& id);

  EventChannelProxy* event_channel();

  libwebrtc::scoped_refptr<libwebrtc::RTCRtpSender> GetRtpSenderById(
      RTCPeerConnection* pc,
      std::string id);

  libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> GetRtpReceiverById(
      RTCPeerConnection* pc,
      std::string id);

  libwebrtc::scoped_refptr<libwebrtc::KeyProvider> GetKeyProviderForId(
      const std::string& keyProviderId);

 private:
  void ParseConstraints(const EncodableMap& src,
                        scoped_refptr<RTCMediaConstraints> mediaConstraints,
                        ParseConstraintType type = kMandatory);

  bool CreateIceServers(const EncodableList& iceServersArray,
                        IceServer* ice_servers);

 protected:
  scoped_refptr<RTCPeerConnectionFactory> factory_;
  scoped_refptr<RTCAudioDevice> audio_device_;
  scoped_refptr<RTCVideoDevice> video_device_;
  scoped_refptr<RTCDesktopDevice> desktop_device_;
  scoped_refptr<RTCAudioProcessing> audio_processing_;
  std::unique_ptr<chatpapol::RnnoiseProcessor> rnnoise_processor_;  // [chatpapol]
  std::unique_ptr<chatpapol::PerUserEqRouter> per_user_eq_;         // [chatpapol]
  RTCConfiguration configuration_;

  std::map<std::string, scoped_refptr<libwebrtc::KeyProvider>> key_providers_;
  std::map<std::string, scoped_refptr<RTCPeerConnection>> peerconnections_;
  std::map<std::string, scoped_refptr<RTCMediaStream>> local_streams_;
  std::map<std::string, scoped_refptr<RTCMediaTrack>> local_tracks_;
  // [chatpapol 48k] Fuentes de audio kCustom por track_id: una pista de micro
  // a la que un capturador nativo (Stage 2) le inyecta PCM a 48k vía
  // CaptureFrame, sin pasar por el APM que baja a 16k. Ver flutter_media_stream.
  std::map<std::string, scoped_refptr<RTCAudioSource>> custom_audio_sources_;
  // [chatpapol 48k — Stage 2] Capturadores de micro nativos vivos por track_id.
  // Cada uno alimenta custom_audio_sources_[track_id] a 48k vía CaptureFrame.
  // Se crea en startCustomMicCapture y se destruye en stopCustomMicCapture / al
  // disponer el track. unique_ptr de tipo incompleto: el dtor de FlutterWebRTCBase
  // (en flutter_webrtc_base.cc, que ahora incluye mic_capturer.h) ve el tipo.
  std::map<std::string, std::unique_ptr<MicCapturer>> custom_mic_capturers_;
  std::map<std::string, scoped_refptr<RTCVideoCapturer>> video_capturers_;
  std::map<int64_t, std::shared_ptr<FlutterVideoRenderer>> renders_;
  std::map<std::string, std::shared_ptr<FlutterRTCDataChannelObserver>>
      data_channel_observers_;
  std::map<std::string, std::shared_ptr<FlutterPeerConnectionObserver>>
      peerconnection_observers_;
  mutable std::mutex mutex_;

  void lock() { mutex_.lock(); }
  void unlock() { mutex_.unlock(); }

 protected:
  BinaryMessenger* messenger_;
  TaskRunner* task_runner_;
  TextureRegistrar* textures_;
  std::unique_ptr<EventChannelProxy> event_channel_;
};

}  // namespace flutter_webrtc_plugin

#endif  // !FLUTTER_WEBRTC_BASE_HXX

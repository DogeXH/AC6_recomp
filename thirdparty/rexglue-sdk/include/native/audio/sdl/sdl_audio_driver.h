// Native audio runtime
// Part of the AC6 Recompilation native foundation

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <stack>

#include <SDL3/SDL_audio.h>

#include <native/audio/audio_driver.h>

namespace rex::audio {
class AudioRuntime;
}

namespace rex::audio::sdl {

class SdlAudioDriver : public AudioDriver {
 public:
  SdlAudioDriver(memory::Memory* memory, AudioRuntime* runtime, size_t client_index);
  ~SdlAudioDriver() override;

  bool Initialize() override;
  void Shutdown() override;
  void SubmitFrame(uint32_t frame_ptr) override;
  void SubmitSilenceFrame() override;
  AudioDriverTelemetry GetTelemetry() const override;
  const char* backend_name() const override { return "sdl"; }
  uint32_t queue_low_water_frames() const override { return 2; }
  uint32_t queue_target_frames() const override { return 3; }

 private:
  static void SDLCALL StreamCallback(void* userdata, SDL_AudioStream* stream,
                                     int additional_amount, int total_amount);
  void FillStream(SDL_AudioStream* stream, int bytes_needed);

  AudioRuntime* runtime_{nullptr};
  size_t client_index_{0};
  SDL_AudioStream* stream_{nullptr};

  std::mutex frames_mutex_{};
  std::queue<float*> frames_queued_{};
  std::stack<float*> frames_unused_{};
  std::array<float, kRenderDriverTicSamplesPerFrame * 2> pending_output_frame_{};
  size_t pending_output_float_count_{0};
  size_t pending_output_float_offset_{0};

  std::atomic<bool> shutting_down_{false};
  std::atomic<uint32_t> submitted_frames_{0};
  std::atomic<uint32_t> consumed_frames_{0};
  std::atomic<uint32_t> underrun_count_{0};
  std::atomic<uint32_t> silence_injections_{0};
  uint64_t last_underrun_log_ms_ = 0;
  std::atomic<uint32_t> queued_depth_{0};
  std::atomic<uint32_t> peak_queued_depth_{0};
  std::atomic<uint32_t> explicit_silence_submissions_{0};
  std::atomic<uint32_t> zero_output_frames_{0};
  std::atomic<uint32_t> nonzero_output_frames_{0};
};

}  // namespace rex::audio::sdl

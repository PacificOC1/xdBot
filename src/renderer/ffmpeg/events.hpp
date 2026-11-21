#pragma once

// (port) The eclipse.ffmpeg-api mod's event wrapper was written for Geode 4's event system. This fork renders
// with ffmpeg.exe only, so these keep the same API but always report that the API isn't there.

#include "render_settings.hpp"

#include <Geode/Result.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace ffmpeg::events {

class Recorder {
public:
    bool isValid() const { return false; }
    geode::Result<> init(RenderSettings const&) { return geode::Err("FFmpeg API isn't supported in this fork: set ffmpeg.exe in the settings"); }
    void stop() {}
    geode::Result<> writeFrame(const std::vector<uint8_t>&) { return geode::Err("FFmpeg API isn't supported in this fork"); }
    static std::vector<std::string> getAvailableCodecs() { return {}; }
};

class AudioMixer {
public:
    AudioMixer() = delete;
    static geode::Result<> mixVideoAudio(std::filesystem::path const&, std::filesystem::path const&, std::filesystem::path const&) {
        return geode::Err("FFmpeg API isn't supported in this fork");
    }
    static geode::Result<> mixVideoRaw(std::filesystem::path const&, const std::vector<float>&, std::filesystem::path const&) {
        return geode::Err("FFmpeg API isn't supported in this fork");
    }
};

}

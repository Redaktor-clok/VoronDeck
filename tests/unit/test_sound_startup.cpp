// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sound_manager.h"
#include "audio_settings_manager.h"
#include "config.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/sound_manager_test_access.h"
#include <atomic>
#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

TEST_CASE("VoronDeck startup is silent in production and mock mode", "[sound][startup]") {
    CHECK_FALSE(helix::SoundManager::plays_startup_sound(false));
    CHECK_FALSE(helix::SoundManager::plays_startup_sound(true));
}

namespace {
class CompletionBackend : public SoundBackend {
  public:
    std::atomic<int> tones{0};
    void set_tone(float, float amplitude, float) override {
        if (amplitude > 0) ++tones;
    }
    void silence() override {}
};
}

TEST_CASE_METHOD(LVGLTestFixture, "Only opted-in completion plays audio", "[sound][startup]") {
    auto& audio = helix::AudioSettingsManager::instance();
    auto& sound = helix::SoundManager::instance();
    sound.shutdown();
    helix::Config::get_instance()->set<bool>("/sounds_enabled", true); // Old installs must stay quiet.
    helix::Config::get_instance()->set<bool>("/print_completion_sound", false);
    audio.init_subjects();
    audio.reload_from_config();
    struct Cleanup {
        ~Cleanup() {
            helix::SoundManager::instance().shutdown();
            helix::AudioSettingsManager::instance().set_sounds_enabled(false);
            helix::AudioSettingsManager::instance().deinit_subjects();
        }
    } cleanup;
    REQUIRE_FALSE(audio.get_sounds_enabled());
    auto backend = std::make_shared<CompletionBackend>();
    helix::SoundManagerTestAccess::install_backend(sound, backend);
    helix::SoundManagerTestAccess::finalize(sound);
    sound.play_print_complete();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(backend->tones == 0);
    audio.set_sounds_enabled(true);
    audio.set_ui_sounds_enabled(true); // Even a stale UI preference cannot enable taps.
    for (auto name : {"startup", "button_tap", "toggle_on", "nav_forward", "dropdown_open", "error_alert", "print_cancelled"}) {
        sound.play(name);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(backend->tones == 0);
    sound.play_print_complete();
    for (int i = 0; i < 100 && backend->tones == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(backend->tones > 0);
    audio.set_ui_sounds_enabled(false);
}

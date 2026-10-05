# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LinkAudioPlayTest(unittest.TestCase):
    def test_command_is_advertised_and_dispatched_with_the_feature(self):
        noise = (ROOT / "main/noise_control.cpp").read_text(encoding="utf-8")
        start = noise.index("#if CONFIG_HOMEHUB_AUDIO_PLAY_COMMAND")
        block = noise[start:noise.index("#endif", start)]
        self.assertIn('add_command(commands, "audio.play_url"', block)
        self.assertIn('"timeout_ms"', block)
        app = (ROOT / "main/app.c").read_text(encoding="utf-8")
        # The definition is the last occurrence: an earlier prototype would
        # pull in the play_url context block above it.
        start = app.rindex("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        block = dispatch[dispatch.index("#if CONFIG_HOMEHUB_AUDIO_PLAY_COMMAND"):]
        block = block[:block.index("#endif")]
        self.assertIn('"audio.play_url"', block)
        # The handler answers asynchronously like display.draw_url: the Muse
        # waits for the task's noise_ctrl_send_command_result.
        self.assertIn('"_async"', block)
        self.assertIn("audio_play_start", block)

    def test_module_gates_on_the_same_option(self):
        source = (ROOT / "main/audio_play.c").read_text(encoding="utf-8")
        self.assertIn("#if CONFIG_HOMEHUB_AUDIO_PLAY_COMMAND", source)
        header = (ROOT / "main/audio_play.h").read_text(encoding="utf-8")
        self.assertIn("#if CONFIG_HOMEHUB_AUDIO_PLAY_COMMAND", header)
        self.assertIn("unsupported", header)

    def test_kconfig_defaults_with_full_ui_and_psram(self):
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        start = kconfig.index("config HOMEHUB_AUDIO_PLAY_COMMAND")
        block = kconfig[start:kconfig.index("config ", start + 10)]
        self.assertIn("default y if HOMEHUB_LED_BACKEND_MUSE && SPIRAM", block)


if __name__ == "__main__":
    unittest.main()

<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../PlatformLayout.vue'
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)

// TeknoParrot Sunshine intentionally locks desktop input to game-only mode.
// Force the value when this page is loaded so existing configs that previously
// used full_control are migrated the next time configuration is saved.
config.value.desktop_input_mode = 'game_only'
</script>

<template>
  <div id="input" class="config-page">
    <!-- Enable Gamepad Input -->
    <Checkbox class="mb-3"
              id="controller"
              locale-prefix="config"
              v-model="config.controller"
              default="true"
    ></Checkbox>

    <!-- Emulated Gamepad Type -->
    <div class="mb-3" v-if="config.controller === 'enabled' && platform !== 'macos'">
      <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
      <select id="gamepad" class="form-select" v-model="config.gamepad">
        <option value="auto">{{ $t('_common.auto') }}</option>

        <PlatformLayout :platform="platform">
          <template #freebsd>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
          </template>

          <template #linux>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
          </template>

          <template #windows>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.gamepad_desc') }}</div>
    </div>

    <!-- Additional options based on gamepad type -->
    <template v-if="config.controller === 'enabled'">
      <template v-if="config.gamepad === 'ds4' || config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform !== 'macos')">
        <div class="mb-3 accordion">
          <div class="accordion-item">
            <h2 class="accordion-header">
              <button class="accordion-button" type="button" data-bs-toggle="collapse"
                      data-bs-target="#panelsStayOpen-collapseOne">
                {{ $t(config.gamepad === 'ds4' ? 'config.gamepad_ds4_manual' : (config.gamepad === 'ds5' ? 'config.gamepad_ds5_manual' : 'config.gamepad_auto')) }}
              </button>
            </h2>
            <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
                 aria-labelledby="panelsStayOpen-headingOne">
              <div class="accordion-body">
                <template v-if="config.gamepad === 'auto' && (platform === 'windows' || platform === 'linux')">
                  <Checkbox class="mb-3" id="motion_as_ds4" locale-prefix="config" v-model="config.motion_as_ds4" default="true"></Checkbox>
                  <Checkbox class="mb-3" id="touchpad_as_ds4" locale-prefix="config" v-model="config.touchpad_as_ds4" default="true"></Checkbox>
                </template>
                <template v-if="config.gamepad === 'ds4' || (config.gamepad === 'auto' && platform === 'windows')">
                  <Checkbox class="mb-3" id="ds4_back_as_touchpad_click" locale-prefix="config" v-model="config.ds4_back_as_touchpad_click" default="true"></Checkbox>
                </template>
                <template v-if="config.gamepad === 'ds5' || (config.gamepad === 'auto' && platform === 'linux')">
                  <Checkbox class="mb-3" id="ds5_inputtino_randomize_mac" locale-prefix="config" v-model="config.ds5_inputtino_randomize_mac" default="true"></Checkbox>
                </template>
              </div>
            </div>
          </div>
        </div>
      </template>
    </template>

    <div class="mb-3" v-if="config.controller === 'enabled'">
      <label for="back_button_timeout" class="form-label">{{ $t('config.back_button_timeout') }}</label>
      <input type="text" class="form-control" id="back_button_timeout" placeholder="-1" v-model="config.back_button_timeout" />
      <div class="form-text">{{ $t('config.back_button_timeout_desc') }}</div>
    </div>

    <hr>
    <Checkbox class="mb-3" id="keyboard" locale-prefix="config" v-model="config.keyboard" default="true"></Checkbox>

    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_delay" class="form-label">{{ $t('config.key_repeat_delay') }}</label>
      <input type="text" class="form-control" id="key_repeat_delay" placeholder="500" v-model="config.key_repeat_delay" />
      <div class="form-text">{{ $t('config.key_repeat_delay_desc') }}</div>
    </div>

    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_frequency" class="form-label">{{ $t('config.key_repeat_frequency') }}</label>
      <input type="text" class="form-control" id="key_repeat_frequency" placeholder="24.9" v-model="config.key_repeat_frequency" />
      <div class="form-text">{{ $t('config.key_repeat_frequency_desc') }}</div>
    </div>

    <Checkbox v-if="config.keyboard === 'enabled' && platform === 'windows'" class="mb-3" id="always_send_scancodes" locale-prefix="config" v-model="config.always_send_scancodes" default="true"></Checkbox>
    <Checkbox v-if="config.keyboard === 'enabled'" class="mb-3" id="key_rightalt_to_key_win" locale-prefix="config" v-model="config.key_rightalt_to_key_win" default="false"></Checkbox>

    <hr>
    <Checkbox class="mb-3" id="mouse" locale-prefix="config" v-model="config.mouse" default="true"></Checkbox>
    <Checkbox v-if="config.mouse === 'enabled'" class="mb-3" id="high_resolution_scrolling" locale-prefix="config" v-model="config.high_resolution_scrolling" default="true"></Checkbox>
    <Checkbox v-if="config.mouse === 'enabled'" class="mb-3" id="native_pen_touch" locale-prefix="config" v-model="config.native_pen_touch" default="true"></Checkbox>

    <!-- Desktop Input Mode: intentionally locked for the TeknoParrot build -->
    <div class="mb-3">
      <label for="desktop_input_mode" class="form-label">{{ $t('config.desktop_input_mode') }}</label>
      <select id="desktop_input_mode" class="form-select" v-model="config.desktop_input_mode" disabled>
        <option value="game_only">{{ $t('config.desktop_input_mode_game_only') }}</option>
      </select>
      <div class="form-text">{{ $t('config.desktop_input_mode_desc') }}</div>
    </div>
  </div>
</template>

<style scoped>
</style>

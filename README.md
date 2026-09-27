# Live Volume Balancer

An OBS audio filter for riding level changes on a live audio feed. It gently raises quieter material, reduces loud peaks, and includes a monitor dock and timed calibration workflow.

## Church service setup

Place the filter on the single combined feed from the mixing desk. Switch its **Service mode** during the service:

| Feed at the moment | Mode |
| --- | --- |
| Worship with a full band | **Worship with full band** — gentler gain range and slower response to retain musical dynamics. |
| Band stops, acoustic worship, or a quieter set | **Worship without band / acoustic** — allows more lift for quieter material. |
| Sermon or spoken announcements | **Sermon / speech** — more active level control for speech. |

The filter does not depend on OBS scenes; change the mode from the filter properties whenever the feed changes. If a scene change reliably follows the service section, it can be used as an operator cue. **Auto Assist** is optional: its simple voice-like activity detector can mistake singing or instruments for speech, so it only gates upward gain and never switches the selected service mode.

## Features

- Linked-channel, K-weighted loudness riding with a 400 ms momentary detector and 3 s short-term meter.
- Separate saved output trim for each service mode. The monitor dock measures the processed output for 10 seconds, shows its LUFS and a bounded correction suggestion, and lets the operator apply or reset that mode's trim.
- Oversampled output peak guard using the 4x FIR coefficients in ITU-R BS.1770-5 Annex 2, with a fixed six-sample lookahead delay. This is not claimed as a conformance-tested true-peak limiter.
- Noise-floor protection, configurable maximum boost/reduction, smooth attack/release, and linked stereo/multichannel gain.
- No allocation or locking in the audio callback. Settings are snapshotted atomically; UI and calibration operations run outside the callback.

The displayed LUFS values are K-weighted BS.1770-style momentary and short-term **input** estimates. They are not integrated loudness measurements or a programme loudness normalization workflow. Channel weighting follows the common OBS layouts; mono/stereo and 5.1/7.1 are supported. The voice detector is a lightweight voice-like activity aid, not speech recognition or worship/sermon classification.

The peak guard adds **six samples** of fixed delay in every mode, including bypass: about **0.125 ms at 48 kHz** and **0.136 ms at 44.1 kHz**. Bypass disables gain riding and peak protection while keeping this delay unchanged. The output meter runs the same 4x FIR estimator over processed audio; the sample ceiling is also enforced as a fallback. The detector and limiter have not been certified against the full BS.1770 conformance suite.

## Monitor and calibration

Open **View → Docks → Live Volume Balancer**. Select the audio source in the dock if more than one filter instance is active. The dock refreshes in real time with input momentary/short-term loudness, output peak estimate, signed applied gain, and voice-like activity.

1. Choose the current service mode in the filter properties.
2. Select **Measure 10 s** during representative live audio for that mode.
3. Review the measured output LUFS and suggested correction, then select **Apply trim**. The trim is saved for the mode where measurement started, even if you change the selected service mode before applying it. It acts as output makeup after the level rider and before the peak guard, so a quiet sermon can still move toward its target when the rider has reached its maximum boost. The correction is limited to ±12 dB; peak protection remains active. **Reset trim** returns the currently selected service mode to zero.

Run calibration separately for full-band worship, acoustic worship, and sermon if you want a starting trim for each. Calibration measures the actual processed output and suggests `target LUFS − measured output LUFS`, clamped to ±12 dB. Because the suggestion is makeup gain, not a target change hidden behind the rider's boost limit, applying it changes the output level directly; the peak guard still protects the ceiling. Use a representative section at normal mixer levels. The suggestion is an operator starting point, not a guarantee of a particular streaming loudness.

## Build

This project uses the [official OBS plugin template](https://github.com/obsproject/obs-plugintemplate). The macOS and Windows presets bootstrap OBS sources, OBS development libraries, and Qt 6 under `.deps/`; first-time configuration downloads substantial dependencies and can take several minutes. The live dock requires `obs-frontend-api` and Qt 6 Widgets. Linux builds need development packages for `libobs`, `obs-frontend-api`, and Qt 6 Widgets in addition to CMake/Ninja and a C/C++ compiler.

macOS:

```sh
cmake --preset macos
cmake --build --preset macos
ctest --test-dir build_macos -C RelWithDebInfo --output-on-failure
```

Windows x64:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure
```

Ubuntu x86_64:

```sh
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
ctest --test-dir build_x86_64 --output-on-failure
```

The macOS development bundle is placed at `build_macos/rundir/RelWithDebInfo/live-volume-balances.plugin`. To install from another build, use `cmake --install <build-directory> --config RelWithDebInfo`.

To run DSP tests without the OBS and Qt SDKs:

```sh
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -Isrc \
  src/leveler-dsp.c tests/leveler-dsp-test.c -lm -o /tmp/lvb-dsp-test
/tmp/lvb-dsp-test
```

## 建置與快速使用（繁體中文）

將濾鏡加在混音台輸出的**單一總音訊來源**上，依聚會段落手動切換「聚會模式」：

- **敬拜（全樂團）**：較溫和、保留音樂動態。
- **敬拜（無樂團／木吉他）**：在樂團停止或音量較小時，可較多提升安靜內容。
- **講道／語音**：提供較積極的語音音量平衡。

不需要不同 OBS 場景才能切換。自動輔助只用簡單的類語音活動偵測控制向上增益，可能把唱歌或樂器當成語音，不會自動把模式切成講道。

在「檢視 → 面板 → 即時音量平衡器」開啟監看面板；若有多個濾鏡實例，先在面板選擇要監看的音訊來源。選好模式後按「測量 10 秒」，面板會測量處理後輸出並顯示 LUFS 與建議校正值；確認後按「套用校正」。校正值會套用到開始測量時的模式，即使測量途中切換模式也不會寫錯。它是音量平衡後、峰值保護前的輸出補償，能在講道已達最大增益時仍調整輸出；建議值限制在 ±12 dB，峰值保護仍有效。每個模式分開保存，「重設校正」只清除目前選取的模式。建議分別校正全樂團敬拜、無樂團敬拜與講道。

響度表顯示 400 毫秒瞬時與 3 秒短期的 K 加權 LUFS 估算，不是整合響度正規化。峰值保護採 4 倍 FIR 估算，固定增加 6 個取樣延遲：48 kHz 約 0.125 毫秒、44.1 kHz 約 0.136 毫秒；旁通時仍保留相同延遲，但不做增益與峰值保護。此實作尚未通過完整的 BS.1770 符合性測試。

本專案沿用 [OBS 官方外掛範本](https://github.com/obsproject/obs-plugintemplate)。macOS 與 Windows 第一次設定會下載 OBS 原始碼、開發函式庫與 Qt 6 到 `.deps/`，需要一些時間。即時監看面板需要 `obs-frontend-api` 和 Qt 6 Widgets。完整建置指令請參考上方 Build 區段；不需 OBS／Qt SDK 的 DSP 單元測試指令也列於上方。

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

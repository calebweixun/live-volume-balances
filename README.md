# Live Volume Balancer

OBS Studio audio filter that automatically raises quiet audio and reduces loud audio for a steadier listening level.

## 功能

- 以每個 OBS 音訊區塊的短窗 RMS 音量估算音量，並平滑調整增益。
- 左右聲道共用增益，保留立體聲平衡；支援 OBS 的多聲道浮點音訊平面。
- 設定最大增益、最大衰減、攻擊／恢復時間、噪音底線與樣本峰值上限。
- 低於噪音底線時不會增加音量；數位靜音保持靜音。
- 旁通模式會原樣傳遞音訊，且不增加緩衝延遲。

這是以短時間 RMS 為基礎的即時音量平衡器，不是整合響度 LUFS 正規化器。峰值上限依樣本峰值限制，不是 true-peak 限制器。

## OBS 使用方式

1. 將外掛安裝到 OBS 能載入的外掛資料夾，並重新啟動 OBS。
2. 在混音器中選取麥克風或其他音訊來源，打開「濾鏡」。
3. 在「音訊濾鏡」區域新增「即時音量平衡器」。
4. 先使用預設值；如果底噪被抬高，降低「最大增益」或提高「噪音底線」。

預設目標為 -18 dBFS RMS、最大增益 +12 dB、最大衰減 18 dB、攻擊 20 ms、恢復 350 ms、噪音底線 -55 dBFS、樣本峰值上限 -1 dBFS。

## 建置

此專案沿用 [OBS 官方外掛範本](https://github.com/obsproject/obs-plugintemplate) 的 CMake 建置系統。請先安裝對應平台的 CMake/Ninja 或 Xcode/Visual Studio 工具。macOS 與 Windows preset 會依 `buildspec.json` 下載 OBS 31.1.1 原始碼和預先建置的 OBS 依賴，並在 `.deps/` 建置 OBS 開發函式庫；第一次設定會下載大量檔案並花一些時間。Linux 建置則需要系統提供 `libobs` 開發套件。

macOS：

```sh
cmake --preset macos
cmake --build --preset macos
ctest --test-dir build_macos -C RelWithDebInfo --output-on-failure
```

Windows x64：

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure
```

Ubuntu x86_64：

```sh
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
ctest --test-dir build_x86_64 --output-on-failure
```

macOS 開發建置會將外掛放在 `build_macos/rundir/RelWithDebInfo/live-volume-balances.plugin`。Linux/Windows 安裝位置依 CMake 安裝設定而定；可用 `cmake --install <build-directory> --config RelWithDebInfo` 安裝。

若只要執行不依賴 OBS SDK 的 DSP 測試：

```sh
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -Isrc \
  src/leveler-dsp.c tests/leveler-dsp-test.c -lm -o /tmp/lvb-dsp-test
/tmp/lvb-dsp-test
```

## Build

This project uses the [official OBS plugin template](https://github.com/obsproject/obs-plugintemplate) and its CMake build system. Install the platform's CMake/Ninja or Xcode/Visual Studio tools. The macOS and Windows presets download OBS 31.1.1 source and the prebuilt OBS dependencies listed in `buildspec.json`, then build the OBS development libraries under `.deps/`; first-time setup downloads a large amount of data and takes a while. Linux builds require the system `libobs` development package.

Use the platform presets shown above. Run `ctest` from the matching build directory to check the DSP tests. On macOS, the development plugin bundle is placed in `build_macos/rundir/RelWithDebInfo/live-volume-balances.plugin`.

To run only the DSP tests without an OBS SDK, compile and run `src/leveler-dsp.c` with `tests/leveler-dsp-test.c` as shown in the Traditional Chinese section above.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

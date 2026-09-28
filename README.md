# Live Volume Balancer

An OBS audio filter that automatically rides the level of a live mixed feed. Add it to the single audio output from the mixing desk; it raises quieter speech, reduces louder music, and avoids lifting room noise during pauses. There are no service modes or pre-show calibration steps.

## Quick setup

1. Add **Live Volume Balancer** to the mixing desk's combined audio source.
2. Start with the default **−18 LUFS rolling target** and **−1 dBTP estimated peak guard ceiling**.
3. Open **View → Docks → Live Volume Balancer** to watch input/output levels, rolling loudness, applied gain, and peak hold while audio is running. Each filter instance has its own card; use its gear button to open that filter's settings.
4. If room noise rises in pauses, raise the activity floor (for example, from −46 to −42 dBFS). If quiet speech does not open the gate, lower it carefully.

The filter does not identify speech versus singing and does not switch settings by scene. It treats the feed as one continuous program, so music and speech share the same target. The gain rider smooths changes to retain some internal dynamics; source levels may remain different when the maximum compensation or peak ceiling is reached.

When an existing installation is upgraded from the service-mode version, its saved mode settings are migrated automatically the next time OBS loads the filter. The selected target, bypass, and peak ceiling are retained; old mode-specific compensation, timing, and noise-floor tuning are replaced with the new automatic defaults.

## Controls

| Control | What it does |
| --- | --- |
| Target rolling loudness | Average level the rider moves toward. Range: −36 to −6 LUFS; default: −18 LUFS. |
| 4x FIR peak guard ceiling | Separate output safety limit for estimated intersample peaks. Range: −24 to 0 dBTP est.; default: −1 dBTP. |
| Bypass | Turns off gain riding and peak protection. The fixed six-sample delay remains. |
| Advanced: maximum upward compensation | Range: 0 to 36 dB; default: 18 dB. |
| Advanced: maximum downward reduction | Range: 0 to 36 dB; default: 18 dB. |
| Advanced: activity floor | Absolute and relative gate used to keep pauses and room noise from being raised. Range: −100 to −6 dBFS; default: −46 dBFS. |
| Advanced: reduction attack / gain recovery | Sets smoothing speed for lowering and raising gain. Ranges: 10 to 3000 ms / 50 to 10000 ms; defaults: 180 ms / 1800 ms. |

The compact dock shows each filter instance separately. Its fast IN/OUT bars show channel-linked RMS with 30 ms attack and 150 ms decay. The Qt display refreshes on a precise 16 ms timer; numeric readings retain their K-weighted 400 ms and 3 s rolling windows. Those readings are not integrated-program loudness or ReplayGain normalization. The target and peak ceiling do different jobs: the target guides average level over time, while the peak guard can lower a transient to protect the output.

The room-noise gate uses a fast activity detector, a configurable absolute floor, and a relative reference that follows sustained quieter content. A signal well below the absolute floor cannot open the gate; speech near −35 dBFS can still be lifted. Short transitions can take a fraction of a second for the rolling detector and gain envelope to settle.

The dock reports this filter's input-to-output delay as **IN 0.000 ms** and **OUT +0.125 ms at 48 kHz** or **+0.136 ms at 44.1 kHz** (six samples), calculated from the active OBS audio sample rate. It reports `—` before audio is flowing. This is only the delay added inside this filter; it excludes the audio interface, other OBS buffering or processing, encoding, streaming, and viewer playback. The peak guard uses the same fixed six-sample output delay, a 4x FIR intersample estimate, and a sample ceiling fallback. This implementation has not been certified against the full BS.1770 conformance suite, so the peak reading is an estimate rather than a standards compliance claim.

The audio callback uses fixed-size state, performs no allocation, and does not wait on a lock. Settings are copied through atomics; the dock refreshes its complete stats snapshot on the UI thread.

## Build

This repository starts from the [official OBS plugin template](https://github.com/obsproject/obs-plugintemplate). macOS and Windows presets bootstrap OBS sources/development libraries and Qt 6 under `.deps/`; first-time configuration downloads substantial dependencies and can take several minutes. The live dock requires `obs-frontend-api` and Qt 6 Widgets. Linux builds need development packages for `libobs`, `obs-frontend-api`, and Qt 6 Widgets in addition to CMake/Ninja and a C/C++ compiler.

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

The macOS development bundle is placed at `build_macos/rundir/RelWithDebInfo/live-volume-balances.plugin`. For another build, use `cmake --install <build-directory> --config RelWithDebInfo`.

To run the DSP tests without the OBS and Qt SDKs:

```sh
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -Isrc \
  src/leveler-dsp.c tests/leveler-dsp-test.c -lm -o /tmp/lvb-dsp-test
/tmp/lvb-dsp-test
```

The per-instance monitor snapshot test can also run without OBS:

```sh
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -Isrc \
  src/monitor-telemetry.c tests/monitor-telemetry-test.c -o /tmp/lvb-telemetry-test
/tmp/lvb-telemetry-test
```

## 繁體中文

這是 OBS 即時音訊音量平衡濾鏡。請將它加在混音台輸出的**單一總音訊來源**上；濾鏡會自動提高較小聲的講道、降低較大聲的音樂，並避免在停頓時把環境底噪拉高。不需要選擇敬拜／講道模式，也不需要事前校正。

### 快速設定

1. 將 **Live Volume Balancer（即時音量平衡器）** 加到混音台的總音訊來源。
2. 先使用預設的 **−18 LUFS 滾動目標**與 **−1 dBTP 估算峰值保護上限**。
3. 在「檢視 → 面板 → 即時音量平衡器」開啟監看面板，每組音訊會有獨立卡片；按卡片的齒輪可直接開啟該濾鏡設定。
4. 若停頓時環境聲被抬高，可提高活動底線（例如從 −46 調到 −42 dBFS）；若小聲講道無法開門，則小心降低底線。

濾鏡不會判斷語音或唱歌，也不會依場景切換設定。它將混音台總訊號視為一條連續節目，音樂與講道共用同一目標。增益變化經過平滑處理以保留部分音樂動態；若最大補償或峰值上限已達限制，輸入差異可能無法完全消除。

從舊版「聚會模式」升級時，OBS 下次載入濾鏡會自動遷移已儲存設定。保留原目標、旁通與峰值上限；舊版依模式設定的補償、反應速度與噪音底線會改用新的自動預設。

### 控制項

| 控制項 | 用途 |
| --- | --- |
| 滾動目標響度 | 自動增益追近的平均電平，範圍 −36 至 −6 LUFS，預設 −18 LUFS。 |
| 4 倍 FIR 峰值保護上限 | 獨立的輸出安全上限，限制估算的取樣間峰值；範圍 −24 至 0 dBTP 估算值，預設 −1 dBTP。 |
| 旁通 | 關閉自動增益與峰值保護，但固定 6 個取樣的延遲仍保留。 |
| 進階：最大向上補償 | 限制安靜且有活動的內容最多能提高多少；範圍 0 至 36 dB，預設 18 dB。 |
| 進階：最大向下衰減 | 限制大聲內容最多能降低多少；範圍 0 至 36 dB，預設 18 dB。 |
| 進階：活動底線 | 絕對底線與相對門檻，避免停頓和環境底噪被拉高；範圍 −100 至 −6 dBFS，預設 −46 dBFS。 |
| 進階：衰減反應／增益恢復 | 設定降低與提高增益的平滑速度；範圍 10–3000／50–10000 毫秒，預設 180／1800 毫秒。 |

緊湊監看面板會為每個濾鏡實例顯示獨立卡片，齒輪按鈕可開啟該濾鏡的設定。IN／OUT 快速跨聲道 RMS 音量表使用 30 毫秒起音與 150 毫秒衰減，Qt 約每 16 毫秒更新；音量表會跟隨即時電平變化。數字仍顯示 K 加權的 400 毫秒與 3 秒滾動響度估算，這些不是整段節目的整合響度，也不是 ReplayGain 正規化。目標響度與峰值上限用途不同：前者引導一段時間內的平均電平，後者在瞬間峰值接近上限時降低輸出。

環境噪音門使用快速活動偵測、可調絕對底線與會隨持續較小聲內容下移的相對參考。遠低於絕對底線的訊號不會開門；約 −35 dBFS 的小聲講道仍可提升。滾動偵測與增益平滑需要一小段時間才能完成轉換。

面板依實際 OBS 音訊取樣率顯示此濾鏡入口到出口的延遲：輸入 **0.000 毫秒**；輸出在 48 kHz 為 **+0.125 毫秒**，在 44.1 kHz 約 **+0.136 毫秒**（6 個取樣）。尚無音訊流動時顯示 `—`。此數值只計算濾鏡內部延遲，不含音訊介面、OBS 其他緩衝或處理、編碼、串流與觀眾播放。峰值保護使用同一個固定 6 個取樣延遲與 4 倍 FIR 取樣間估算，另以樣本峰值上限作為保護。此實作尚未通過完整 BS.1770 符合性測試，因此峰值讀值是估算，不代表已符合標準。

音訊回呼只使用固定大小狀態、不配置記憶體，也不等待鎖；設定以原子值傳入，監看面板在 UI 執行緒讀取完整統計快照。

建置方式與 DSP 單元測試指令請參考上方 **Build** 區段。macOS 與 Windows 第一次設定會在 `.deps/` 下載 OBS 開發相依套件與 Qt 6，可能需要數分鐘；Linux 則需先安裝 libobs、obs-frontend-api 與 Qt 6 Widgets 開發套件。

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

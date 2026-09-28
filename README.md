# Live Volume Balancer

An OBS audio filter that automatically rides the level of a live mixed feed. Add it to the single audio output from the mixing desk; it raises quieter speech, reduces louder music, and avoids lifting room noise during pauses. There are no service modes or pre-show calibration steps.

## Quick setup

1. Add **Live Volume Balancer** to the mixing desk's combined audio source.
2. Start with the default **−18 LUFS rolling target** and **−1 dBTP estimated peak guard ceiling**.
3. Open **View → Docks → Live Volume Balancer** to watch input/output levels, rolling loudness, applied gain, and peak hold while audio is running. Each filter instance has its own card; use its gear button to open that filter's settings.
4. If room noise rises in pauses, raise the activity floor (for example, from −46 to −42 dBFS). If quiet speech does not open the gate, lower it carefully.

The filter does not identify speech versus singing and does not switch settings by scene. It treats the feed as one continuous program, so music and speech share the same target. The gain rider smooths changes to retain some internal dynamics; source levels may remain different when the maximum compensation or peak ceiling is reached. A high target such as 0 LUFS is still only a loudness goal: maximum upward compensation and the separate estimated peak ceiling may prevent output from reaching it.

When an existing installation is upgraded from the service-mode version, its saved mode settings are migrated automatically the next time OBS loads the filter. The selected target, bypass, and peak ceiling are retained; old mode-specific compensation, timing, and noise-floor tuning are replaced with the new automatic defaults.

## Controls

| Control | What it does |
| --- | --- |
| Target rolling loudness | Average level the rider moves toward. Range: −36 to 0 LUFS; default: −18 LUFS. This is a goal, not an output ceiling; maximum upward compensation or the peak guard ceiling can keep output below it. |
| 4x FIR peak guard ceiling | Separate output safety limit for estimated intersample peaks. Range: −24 to 0 dBTP est.; default: −1 dBTP. |
| Bypass | Turns off gain riding and peak protection. The fixed six-sample delay remains. |
| Advanced: maximum upward compensation | Range: 0 to 36 dB; default: 18 dB. |
| Advanced: maximum downward reduction | Range: 0 to 36 dB; default: 18 dB. |
| Advanced: activity floor | Absolute and relative gate used to keep pauses and room noise from being raised. Range: −100 to −6 dBFS; default: −46 dBFS. |
| Fader response / smoothness | Main gain control; range 0 to 100, default 85. At 85, maximum dB movement is about 3.8 dB/s down and 4.5 dB/s up. From slew alone, a 12 dB move takes at least about 3.1 s down / 2.7 s up; detector smoothing and eased acceleration can make it longer. It continuously blends 85% of the 3 s reading with the 400 ms reading. If sustained active audio remains more than 8 LU below the 3 s reading for 0.8 s, recovery temporarily favors the 400 ms reading. |
| Advanced: activity re-entry speed | Range 0 to 100, default 75. After the activity gate has opened and then closed, a new signal must stay above the opening threshold for 40 ms. The rider then favors the 400 ms reading and allows faster upward movement for up to 2 seconds while the gate remains open, including its hold period. 0 uses the normal smoothness response; 100 is fastest. Downward movement, the activity gate, maximum boost and peak ceiling remain in effect. This is activity detection, not speech/music recognition. |
| Advanced: quiet-period attenuation | Range: 0 to 12 dB; default: 0 dB (off). After 1.2 s without a fresh activity candidate, the same fader moves toward this quiet endpoint, then eases back when activity resumes. During the hold, it freezes at its actual position, even if the fader was still moving when a short phrase ended. |

The compact dock shows each filter instance separately. Its IN/OUT readings are measured at this filter's input and output, before OBS applies that source's mixer volume fader. That downstream fader can change what is heard or streamed without changing these readings. The input fast dBFS rail spans −100 to 0 dBFS and marks the configured absolute activity floor; its numeric floor is shown below the title. The relative activity gate can still remain closed above that marker. Output has separate fast dBFS and 400 ms K-weighted LUFS rails, each with its own scale; the rolling target marker and its numeric value appear only on the LUFS rail. A **PEAK** tag means the peak-aware rider or FIR guard has reached the available peak headroom; **BOOST** means the main fader is near its Maximum Compensation cap. Either can make a target unreachable; the tooltip gives the reason, and both may apply at once. For example, −33.9 LUFS input with a +12 dB boost cap can reach only about −21.9 LUFS before peak-headroom effects, not −14 LUFS. A single 400 ms input reading and a paired 3 s input/output reading sit below the rails. These are not integrated-program loudness or ReplayGain normalization. The target, maximum compensation and peak ceiling do different jobs: the target guides average level over time, compensation caps steady boost, and the peak guard protects the output from peaks.

The gain bar is labeled **NET GAIN**. It uses a fixed −36 to +36 dB scale centered at zero and shows this filter's applied gain after peak protection, before the downstream OBS mixer fader. End labels and marker lines show the active maximum reduction and boost. Hover the gain bar to see each source's current fader smoothness, activity re-entry speed and quiet-period attenuation values. Peak hold and ceiling remain in a compact summary. Fast IN/OUT bars use channel-linked RMS with 30 ms attack and 150 ms decay. The Qt display refreshes on a precise 16 ms timer, and the rails only repaint when their readings or markers change. When bypass is enabled, the card says **BYPASS** and dims the reference markers; the gain readout stays at its actual 0 dB value.

Fader smoothness controls the feel of the main gain rider; it does not classify speech and music. At the default 85, dB movement is limited to about 3.8 dB/s down and 4.5 dB/s up, with eased acceleration. From the slew limits alone, a 12 dB move takes at least about 3.1 seconds down / 2.7 seconds up; rolling windows and target smoothing can make actual transitions longer. The rider continuously blends 85% of the 3 s loudness reading with the 400 ms reading. If fresh active audio remains more than 8 LU below the 3 s reading for 0.8 seconds, recovery temporarily favors the 400 ms reading; a closed pause cannot trigger this assist. Activity re-entry speed is a separate advanced control for the first words after the gate closes: after 40 ms of a fresh signal above the opening threshold, it favors the 400 ms reading and raises the upward rate for up to 2 seconds while the gate remains open, including its hold period. It does not raise the target beyond maximum compensation or the peak-aware ceiling, and it does not affect downward gain changes. A musical bed that keeps the activity gate open continues to use the ordinary smoothness curve.

Between words, the activity gate holds the actual main-fader position for about 1.2 seconds. This matters when a short phrase ends before the gain ramp has finished: the fader stops at its current position instead of continuing to amplify room tone. After the gate closes, quiet-period attenuation gradually moves that same fader toward its configured endpoint. When activity returns, it glides back toward the rolling target. A recent input-peak envelope also lets the main fader take over sustained peak-ceiling work with about 0.5 dB of headroom; the separate fast 4x FIR guard remains active for sudden peaks. A smoother curve reduces pumping but takes longer to fully correct a sustained level change.

The room-noise gate uses a fast activity detector, a configurable absolute floor, and a relative reference that follows sustained quieter content. A signal well below the absolute floor cannot open the gate; speech near −35 dBFS can still be lifted. Short transitions can take a fraction of a second for the rolling detector and gain envelope to settle. Quiet-period attenuation begins only after the hold expires and this gate closes; with its default 0 dB setting, it does not lower the long-pause fader endpoint.

This filter processes one combined mix. Background noise that occurs with active music or speech receives the same gain as that wanted audio; neither the fader curve nor quiet-period attenuation can isolate the voice or remove in-band noise while the gate is open. For separate voice and band control, provide separate OBS sources or adjust the mix upstream.

The dock reports this filter's input-to-output delay as **IN 0.000 ms** and **OUT +0.125 ms at 48 kHz** or **+0.136 ms at 44.1 kHz** (six samples), calculated from the active OBS audio sample rate. It reports `—` before audio is flowing. This is only the delay added inside this filter; it excludes the audio interface, other OBS buffering or processing, encoding, streaming, and viewer playback. The peak guard uses the same fixed six-sample output delay, a 4x FIR intersample estimate, and a sample ceiling fallback. This implementation has not been certified against the full BS.1770 conformance suite, so the peak reading is an estimate rather than a standards compliance claim.

The audio callback uses fixed-size state, performs no allocation, and does not wait on a lock. Settings are copied through atomics; each filter publishes its meter readings and effective settings together in one coherent telemetry snapshot for the dock.

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

濾鏡不會判斷語音或唱歌，也不會依場景切換設定。它將混音台總訊號視為一條連續節目，音樂與講道共用同一目標。增益變化經過平滑處理以保留部分音樂動態；若最大補償或峰值上限已達限制，輸入差異可能無法完全消除。即使設定 0 LUFS，這仍是響度目標而非輸出上限；最大向上補償與獨立的估算峰值上限可能使輸出無法達到目標。

從舊版「聚會模式」升級時，OBS 下次載入濾鏡會自動遷移已儲存設定。保留原目標、旁通與峰值上限；舊版依模式設定的補償、反應速度與噪音底線會改用新的自動預設。新增的「活動恢復速度」使用預設 75。

### 控制項

| 控制項 | 用途 |
| --- | --- |
| 滾動目標響度 | 自動增益追近的平均電平，範圍 −36 至 0 LUFS，預設 −18 LUFS。這是響度目標，不是輸出上限；最大向上補償或峰值保護上限可能使輸出低於目標。 |
| 4 倍 FIR 峰值保護上限 | 獨立的輸出安全上限，限制估算的取樣間峰值；範圍 −24 至 0 dBTP 估算值，預設 −1 dBTP。 |
| 旁通 | 關閉自動增益與峰值保護，但固定 6 個取樣的延遲仍保留。 |
| 進階：最大向上補償 | 限制安靜且有活動的內容最多能提高多少；範圍 0 至 36 dB，預設 18 dB。 |
| 進階：最大向下衰減 | 限制大聲內容最多能降低多少；範圍 0 至 36 dB，預設 18 dB。 |
| 進階：活動底線 | 絕對底線與相對門檻，避免停頓和環境底噪被拉高；範圍 −100 至 −6 dBFS，預設 −46 dBFS。 |
| 推桿反應／平順度 | 主增益控制，範圍 0–100，預設 85；0 反應快、100 移動平順。設定 85 時，最大速率約每秒向下 3.8 dB、向上 4.5 dB。光看速率，12 dB 至少需約 3.1 秒向下或 2.7 秒向上；偵測平滑與緩入加速度會讓實際轉換更久。它持續混合 85% 的 3 秒讀值與 400 毫秒讀值。 |
| 進階：活動恢復速度 | 範圍 0–100，預設 75。活動門曾開啟並完全關閉後，新訊號須連續高於開啟門檻 40 毫秒，才會在活動門保持開啟期間最多 2 秒偏向 400 毫秒讀值並加快向上回升。它不加速初次啟動、不改變向下處理，也不略過活動門、最大補償或峰值上限；不會判斷人聲或音樂。 |
| 進階：安靜時衰減 | 範圍 0–12 dB，預設 0 dB。沒有新的活動約 1.2 秒後，同一條推桿才移向此安靜終點；活動恢復時平順回升。短句結束後的保留期間，推桿會停在當下實際位置，即使短句結束時增益尚未走完，也不會繼續拉高底噪。 |

升級時會保留目標響度、峰值上限、最大補償／衰減、活動底線、推桿平順度與安靜時衰減。舊版的「衰減反應」和「增益恢復」值仍可能留在 OBS 設定資料，但新版不再讀取；兩種方向都由唯一的推桿反應控制。舊版的安靜時衰減值會保留，並成為同一條推桿在長安靜時的終點。

緊湊監看面板會為每個濾鏡實例顯示獨立卡片，齒輪按鈕可開啟該濾鏡的設定。輸入／輸出讀值是在此濾鏡入口與出口測量，位於 OBS 套用來源混音器音量推桿之前；下游推桿會改變實際聽到或串流出去的音量，但不會改變面板讀值。輸入快速 dBFS 軌使用 −100 至 0 dBFS 刻度，並在標題下方直接顯示設定的絕對活動底線；即使高過此標記，相對活動門仍可能保持關閉。輸出分成快速 dBFS 軌與 400 毫秒 K 加權 LUFS 軌，各自使用不同刻度；滾動目標標記與數值只出現在 LUFS 軌。當主推桿接近最大補償，或峰值感知推桿／快速保護碰到可用峰值空間，且滾動輸出仍低於目標時，目標旁會顯示「補償」或「峰值」標記。軌下只顯示一次輸入 400 毫秒數值，並保留輸入／輸出並列的 3 秒讀值。這些不是整段節目的整合響度，也不是 ReplayGain 正規化。目標響度與峰值上限用途不同：前者引導一段時間內的平均電平，後者在瞬間峰值接近上限時降低輸出。

增益條顯示「實際增益」，使用固定 −36 至 +36 dB 刻度，中線為零；填色從中線延伸，表示此濾鏡在峰值保護後實際套用的增益，不含下游 OBS 混音器推桿。兩端文字與標記線顯示實際最大衰減和補償設定；推桿標籤顯示此來源自己的平順度。將游標停在增益條上可查看推桿平順度、活動恢復速度與安靜時終點。峰值保持與上限維持簡短摘要。IN／OUT 快速表使用跨聲道 RMS、30 毫秒起音與 150 毫秒衰減。Qt 約每 16 毫秒更新，數值或標記改變時才重繪音量軌。開啟旁通時，卡片會顯示「旁通」並淡化參考標記；增益讀值仍顯示實際的 0 dB。

推桿平順度不是人聲／音樂分類器。預設 85 時，最大增益變化速率約每秒向下 3.8 dB、向上 4.5 dB；光看速率，12 dB 至少需約 3.1 秒向下或 2.7 秒向上，實際可能更久。它持續混合 85% 的 3 秒響度讀值與 400 毫秒讀值。活動門在字句間約 1.2 秒保持當下實際推桿位置；短句結束時若增益還沒走完，會停在當前位置，不會在底噪上繼續往上爬。較長停頓才沿同一條推桿曲線移向安靜衰減設定。活動恢復速度只在活動門曾開啟並完全關閉後生效：新訊號連續 40 毫秒高於開啟門檻時，在活動門保持開啟期間最多 2 秒偏向 400 毫秒讀值並加快向上回升；它不加速初次啟動或向下增益，也不略過活動門、最大補償與峰值上限。近期輸入峰值也會引導慢速主推桿提早承接持續的峰值控制，快速 4 倍 FIR 保護則繼續處理突發峰值。曲線越平順，越能降低 pumping，但持續電平改變需要較久才能完全校正。

環境噪音門使用快速活動偵測、可調絕對底線與相對參考。活動至少要高於底線 6 dB 才會開門；只有高於底線 3 dB 的電平會更新相對參考。門檻附近的房間噪音不會重設 1.2 秒保留時間，也不會重新開門。遠低於底線的訊號不會開門；約 −35 dBFS 的小聲講道仍可提升。沒有新的活動候選時，短詞間約 1.2 秒保持當下實際推桿位置，避免尚未完成的增益斜坡在停頓中繼續抬高底噪。較長安靜時才沿推桿曲線走向設定的安靜衰減終點。

此濾鏡處理的是一條混音總訊號。只要音樂或語音仍使活動門開啟，同時存在的底噪就會跟著節目共用增益；推桿曲線與安靜時衰減都無法分離人聲或移除開門期間的帶內噪音。若要分別控制人聲與樂團，需提供不同 OBS 音訊來源，或在混音台上游調整。

面板依實際 OBS 音訊取樣率顯示此濾鏡入口到出口的延遲：輸入 **0.000 毫秒**；輸出在 48 kHz 為 **+0.125 毫秒**，在 44.1 kHz 約 **+0.136 毫秒**（6 個取樣）。尚無音訊流動時顯示 `—`。此數值只計算濾鏡內部延遲，不含音訊介面、OBS 其他緩衝或處理、編碼、串流與觀眾播放。峰值保護使用同一個固定 6 個取樣延遲與 4 倍 FIR 取樣間估算，另以樣本峰值上限作為保護。此實作尚未通過完整 BS.1770 符合性測試，因此峰值讀值是估算，不代表已符合標準。

音訊回呼只使用固定大小狀態、不配置記憶體，也不等待鎖；設定以原子值傳入。每個濾鏡實例會將表頭讀值與實際生效設定一起發布為一致的統計快照。

建置方式與 DSP 單元測試指令請參考上方 **Build** 區段。macOS 與 Windows 第一次設定會在 `.deps/` 下載 OBS 開發相依套件與 Qt 6，可能需要數分鐘；Linux 則需先安裝 libobs、obs-frontend-api 與 Qt 6 Widgets 開發套件。

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

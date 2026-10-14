# Verified device protocol

## 2026-10-14 Continuous diagnostic segments (current)

USB read cadence/commands remain unchanged. Build4B98DFBD is installed and running: reaching8MiB creates a new log segment and continues the triggering record and the same sequence. Old run17760 stopped recording at15:44:06, before the17:38:01 USB1010 incident confirmed by the user; missing query evidence cannot be reconstructed. Source transport/UI/autostart hashes unchanged. Small-cap concurrent rollover verified; a new natural disconnect has not yet been captured. Full incident/implementation evidence: verification/usb-log-continuous-20261014/REPORT.md.

## 2026-10-14 Original USB cadence and diagnostics (current)

The user requested restoring the pre-pressure strategy for diagnosis: 3s configuration checks with enumeration on every refresh; original power retry delays 5/15/60/300s and configuration retries 15/60/300s. The pressure endpoint cache is removed. Async Feature/shared 150ms budget/cancellation drain and targeted PnP coalescing remain. No new device command or settings write is introduced.

Diagnostic build587F0EFE is installed/running at the original path. Its normal startup log confirms82(identity)/87(power)/84(configuration) and3156–3172ms configuration intervals, with no errors in this short sample. This verifies instrumentation, not disconnect cause or stability. The following60s pressure section is historical and superseded. Log schema and limits: verification/usb-diagnostics-20261014/REPORT.md.

## 2026-10-14 USB read-pressure revision

USB fallback configuration reads now run every60s alongside the existing60s power cadence; wireless remains3s. USB failures retreat30s/60s/300s, including failed first identification when a USB endpoint was attempted. Valid USB notifications retain1s settling/2s burst cap/4 extra reads per minute; explicit refresh remains immediate. Without usable notifications, visible external changes may wait about60s. No report freshness extension or new device command is introduced.

Notification configuration reads can reuse a verified USB endpoint descriptor only for the same connection generation. Each newly opened handle still validates identity and complete replies; failure/generation change/stop invalidates the descriptor. Settings continue to enumerate/verify and fully read-modify-write/readback. Fake-HID worker and native UI regressions plus Release build passed; combined648CD073 was installed and later explicitly started with USB/official-driver-exited confirmation. Actual USB stability is unverified; see verification/usb-read-pressure-20261014/REPORT.md. Later historical3s USB statements do not describe the current revision.

Observed on 2026-10-10. Only the exact devices listed in README are allowed. HID paths/serials and vendor binary assets are not retained.

## Rapoo VT3S MAX Master V2

Wireless VID:PID 24AE:146E; USB 24AE:466E. Vendor UsagePage FF00, status Usage2/input19, control Usage14/output33, feature Usage15/feature33. Endpoints from different PIDs must never be combined.

Captured wireless status:
`07 40 01 20 03 20 03 01 64 00 04 01 01 6E 46 10 02 04 B4`

Captured USB/charging status:
`07 42 01 20 03 20 03 02 64 00 04 01 01 6E 46 10 02 04 00`

Battery byte8 is 0x64=100 in both captures. Marker high nibble 0x40 is specific to the observed Master V2 variant; bit1 changes with observed charging state. USB/wireless is determined from verified endpoint PID, because the marker high nibble is identical in both modes. DPI fields match 800. The old 4000Hz reading came from a fixed inactive configuration slot, so it does not establish the active mouse rate. 0–100 is accepted; invalid battery data is rejected rather than converted to 100.

### Active configuration (2026-10-10 fix)

The official status decoder exposes `rpt` and `cfgSel` at payload offsets10/11; including Windows report ID these are bytes11/12. The initial capture above therefore already contained active polling01=1000Hz and slot1. After reboot, passive-only capture was `07 40 00 20 03 20 03 01 64 00 04 84 01 6E 46 10 00 02 BB`: polling84=2000Hz, slot1, battery100. Runtime now reads polling from this input report, rather than querying an inactive bank. The captured bytes are preserved above; historical probe logs were removed at the user's request.

Official command prefix is FF for USB and A5 for wireless. Read opcode A4 and write opcode A5 follow this prefix. Endpoint PID determines the prefix, not status high nibble40. Builder and fake-transport tests cover both formats; the new USB prefix has not been physically retested this session.

Official [A Hub](https://hub.rapoo.cn/) public SDK describes current slot at bank1/address0C and slot bases `0x600 + slot*0x400` (0..7). Polling offset `0x280` and sleep offset `0x2C2` therefore use bank `8 + slot*4`, addresses80/C2. Both reads and manual writes must follow the active slot. Startup performs no setting writes. Slot is rechecked after reading to reject a concurrent profile switch.

Observed target: current slot1; bank08 rate82 / sleep05 (inactive); bank0C rate84 / sleep07 (active). User confirmed both polling and key scanning were set to2000Hz. Active rate84 maps to2000Hz in the official UI; sleep07 is7 minutes, matching the screenshot. The initial statement of1000Hz was superseded by the user's confirmation of current2000Hz; do not claim a physical1000Hz validation. These observations are historical hardware results, not a fresh run.

Protocol facts were inspected in official script `https://hid.rapoo.cn/AHUB_WEB/dist/assets/js/index-BSl-zvvI.js`; no vendor source or assets are included in the application.

The inspected SDK write helper `ja` calls `sendAndWait` and validates ACK1 before completing a write; busy ACK2 is polled for the same command. Runtime now consumes the write ACK before issuing readback. The prior fire-and-read sequence could overlap an unfinished write, affecting both sleep and polling settings. The corrected sequence passed a bounded same-value hardware write: active slot1, sleep07, write ACK1, sleep07, slot1. No keyboard query or firmware command was sent in this check.

SDK SHA256: `E5CBF516BADE682FB3A747A41F4FD7E5F56BBF0B0E5ED05316506297736A2B8F`. Opcode A3 is a firmware-version query, not an unlock operation; runtime no longer sends it. Three-second polling/configuration heartbeats were removed. Sleep is read once following the first report and at most once per minute on user refresh; a failed control transaction blocks automatic requests until endpoint reopening or a successful explicit manual recovery. Manual recovery drains any unresolved response before sending a new command, and failed manual attempts are limited to one per two seconds. Unrelated device notifications do not reopen an unchanged mouse endpoint.

The official driver temporarily lost recognition during earlier continuous read probes; exact cause is unproven. User reboot restored it. After the fix, a single current-profile sleep transaction returned slot1, sleep07, slot1, with no settings written; runtime reported mouse100%/2000Hz/7min and keyboard100%. User then confirmed the official driver still recognized the mouse. This is one successful coexistence check, not proof of all long-running driver interactions. The original diagnostic logs were removed at the user's request; the observed outcome is summarized here.

The input report snapshot API returned a different report (`02 ...`) on this hardware and is not used by the runtime. Battery comes from passive report7. Original control/feature communication is retained; pending Windows overlapped I/O is canceled and drained before buffers/events are released.

## LEOBOG HI98C Pro

Identity signature: `03 00 00 00 01 21`. Matching VID/PID alone is insufficient because OEM keyboards reuse those IDs.

USB 258A:010C, UsagePage FF00/Usage1, FeatureReportByteLength 520. Overlapped IOCTL_HID_SET_FEATURE sends a query, wait up to25ms within the remaining budget, then IOCTL_HID_GET_FEATURE reads report6. Send and receive share one150ms deadline; timeout/shutdown cancels and drains the pending operation. Driver cancellation completion itself is not a hard real-time guarantee. Identity request command82/parameter1/length6; battery command87/parameter2/length2. Headers and payload lengths are validated.

Captured identity: `06 82 01 00 01 00 06 00 03 00 00 00 01 21 00 00`

Captured power: `06 87 02 00 01 00 02 00 64 10 00 00 00 00 00 00`

Battery byte8=100; flags byte9=0x10 indicate charging. Charging is recognized when the high nibble is nonzero and low nibble is zero, consistent with vendor ReadPower logic inspected during planning.

2.4G 3554:FA09, UsagePage FF02/Usage2, Input/OutputReportByteLength20. Read requests have report13, command, pages=1, page=0, length=0, checksum at byte19 (sum bytes0–18 modulo256). Identity/power queries are05/0B; lighting query44 is described below.

Captured identity: `13 05 01 00 0A 03 00 00 00 01 21 01 00 00 00 00 00 00 00 49`

Captured status: `13 0A 01 00 04 05 64 01 00 00 00 00 00 00 00 00 00 00 00 8C`

Battery byte6=100, valid flag byte7=1. Status reply command is0A, not outgoing0B. Reply length/page fields and checksum must validate. Wireless charge flags are not established, so no charging claim is made in this mode. Unchecksummed/unframed requests from a different keyboard reference did not produce a response on this unit.

### Lighting (USB and 2.4G physically verified)

Installed vendor profile `H:\LEOBOG\Dev\kb\Hi98 3-Mode\KB.ini` matches both IDs and identity signature, and lists Light/LightHW and Speed/SpeedHW as0..4. No driver binaries or assets were copied. USB read is command84/parameter0/length128, write04/parameter0/length128. The page trailer is5A A5 and offsets56/57 areFF FF. Active effect at10 selects a pair at `58 + 2*(effect-1)` for effects1..18: first byte brightness; second byte high nibble speed, low nibble color. Other layouts are rejected. Brightness percentage is a display mapping of hardware levels, not a measured physical luminance.

User hotkeys lowered brightness and speed from4 to3; the complete128-byte before/after captures differ only at offsets76/77 for active effect10. USB read/modify/write/readback tests lowered each setting3→2 and restored3; full-page equality was required after every write. Current effect/color and other fields were preserved. Evidence: `verification/keyboard-lighting/initial-read.txt`, `after-shortcuts.txt`, `wired-roundtrip.txt`.

Wireless read44 returns10 indexed20-byte frames, payload14 bytes except last2, checksum as above. Frames may be out of order or duplicated; each checksum/page/length must validate, conflicting duplicates and incomplete pages fail. Write04 sends the same10-page layout and validates a complete echo of each exact request before advancing. Actual first ACK was `13 04 0A 00 0E 00 03 03 00 00 00 04 04 07 00 0A 20 01 00 6F`; an initial assumed empty ACK was rejected, without a false success. Corrected same-value write and then brightness/speed3→2→3 roundtrips succeeded with complete page readback; final effect10, brightness3, speed3. Evidence: `wireless-read.txt`, `wireless-same.txt`, `wireless-same-confirmed.txt`, `wireless-roundtrip.txt` in that folder. No automatic write retry is sent after failure.

Each change reopens the exact endpoint, verifies model identity, freshly reads its own page (USB/radio offset14 differs), changes only the requested field and rereads all128 bytes. Worker refresh and manual transactions share an I/O mutex; failed attempted writes invalidate the cached light values. Lighting uses the existing keyboard refresh cadence and freshness limit. These tests cover this user's active effect10; other effects, full sleep/reconnect cycles and long-term driver coexistence remain unverified. Static effects may ignore speed, and custom layouts must be researched before support.

### Sleep and intermittent USB response recovery (later update)

The same128-byte page stores wireless idle timeout at offset24, in30-second units. The user reported5 minutes; earlier captured page24=10. After the user applied10 minutes in the official driver, a fresh USB page24=20 was read. Current active effect11 (changed independently by the user), brightness4/speed4. Local vendor profile contains SleepTime=120; the application offers a1–120 minute slider and5/10/15/30/60 minute presets and displays any valid30-second multiple up to120 minutes. The vendor's full visual range was not independently checked; actual tests cover5/10 minutes. Page read validates framing/trailer independently of effect support, so sleep does not require a recognized lighting effect. Only byte24 is changed for sleep; all128 bytes must match on readback.

First USB sleep roundtrip with the vendor keyboard driver open failed at initial configuration read before any setting write; restore attempt could not verify identity, final query was unavailable. No automatic retry was performed. After the user completely exited that driver and kept USB connected, the same bounded test10→5→10 minutes succeeded, returning600 seconds and preserving effect11, brightness4/speed4. This supports shared-interface contention; the exact rejected packet was not captured and the cause is not conclusively proven. Evidence: `verification/keyboard-settings/sleep-driver-10min.txt`, `sleep-usb-roundtrip.txt`, `sleep-usb-driver-closed.txt`. Radio sleep byte preservation/ACK/readback is covered by fake-HID tests using the previously physically verified10-page transport; a fresh radio sleep roundtrip and elapsed physical idle/wake cycle were not run this scope.

Previously one battery failure discarded the current valid report and delayed automatic recovery5 minutes, while a right-click forced an early query. Worker now retries after5/15/60/300 seconds, uses the existing60-second interval on success, and preserves only an unexpired same-mode report without advancing its timestamp. First-ever unknown, expiration, disconnection and mode changes do not synthesize valid percentages. USB query sends once and polls feature replies within one150ms send/receive budget to reject stale, late or truncated replies; no extra setting writes are retried. Actual Worker loop under fake HID passed initial battery failure→timer expiry→valid battery publication without ForceRefresh/right-click. Extended real-device contention/recovery remains a runtime limit.

## Research provenance

- [Original rapoo-tray fixed baseline](https://github.com/Iris-0109/rapoo-tray/tree/20ff56a96b94e7acfcb43f53851c5daa75aa73e0): native mouse communication and UI.
- [ClickSync protocol source](https://github.com/Nuitfanee/ClickSync/blob/main/src/protocols/protocol_api_rapoo.js): passive mouse battery behavior; Master V2 marker verified from this user's hardware, not inferred from this reference.
- [AulaSoftwareLinux wired protocol](https://github.com/yhuikzdtguioaert/AulaSofwareLinux/blob/803d73eed3675c845c737bf392ab7a29eacbd0a5/src/proto.rs): read command82/87 format, independently checked on HI98C Pro.
- [AulaSoftwareLinux configuration page](https://github.com/yhuikzdtguioaert/AulaSofwareLinux/blob/803d73eed3675c845c737bf392ab7a29eacbd0a5/src/config_page.rs): candidate lighting fields, confirmed against this keyboard's full-page hotkey changes and roundtrips; independently implemented.
- [AULA wireless reference](https://github.com/dnviti/aula-x99-pro-driver-linux/blob/7ec12ad1f2bb642a46ae91b0c3bce327de96f4ce/aula_f99pro.py): query opcode discovery only; implementation is independent, and exact request framing was established by hardware tests.

Only lighting configuration commands now physically verified on this exact keyboard are used; no firmware/reset/keymap commands are sent. Different real battery values and extended sleep/reconnect cycles remain required hardware checks.

## Shortcut feedback follow-up (2026-10-11)

User clarified that Fn+Up/Down and Fn+Left/Right changes should show brightness/speed popups. The previous result popup was connected only to menu actions; worker reads did not report external configuration differences. No unsolicited shortcut-report protocol has been established. User explicitly selected3-second read-only configuration checks, with failure backoff, after the device-communication and vendor-driver contention tradeoff was explained.

The existing keyboard worker now schedules power polls independently (60seconds on success, existing5/15/60/300second power-failure recovery) and lighting checks (3seconds on success,15/60/300second failures). Fast checks re-verify the exact keyboard identity and read the config page only; no power query, command04 config write, firmware, reset, keymap or mouse request. Regular battery polls skip config reads while that deadline is deferred. They retain only the current same-mode configuration with unchanged original timestamps; getters still enforce3-minute freshness. Disconnect suspends fast checks. Query/write serialization is retained.

Only fresh, same-mode, same-effect brightness/speed differences generate a typed background event; initial reads, reconnection, effect switches, stale-baseline recovery and same-value menu readbacks do not. Main window checks current reading revision before showing one2.2second percentage card, combining both changed fields if necessary. It identifies actual configuration changes, not individual keypresses, so changes reversed between polls can be missed, and official-driver changes also qualify. Runtime latency can exceed3seconds due to transaction duration/backoff. No key hooks or keystroke logging.

Evidence: verification/keyboard-shortcuts/REPORT.md, tests.txt, event-results.txt, runtime.json and performance.json. Fake-HID tests cover USB/radio config-only reads, unchanged battery report/timestamp, independent deadlines, failed reads at simulated0/15/75/375seconds despite60second power polls, zero setting writes and typed change publication. Native hidden/offscreen OSD tests reject invalid/outdated events without setting calls. Actual Fn shortcut presses and long-term vendor coexistence/wireless idle/power impact have not been physically retested in this round.

## Passive-notification feasibility follow-up (2026-10-11)

USB additionally exposes vendor FF00/usage1 input4 and input8 collections. With the official keyboard driver exited and tray stopped,90-second zero-request listening captured four `03 08 00 01` input4 reports during user-confirmed Fn operations. This demonstrates notification arrival, not direct brightness/speed/battery fields or notification semantics. Input8 was silent in that observation; wireless was not tested.

Notification-triggered config queries parsed unchanged effect5/brightness4/speed0; actual physical lighting changes were not observed by the user. A delayed diagnostic incorrectly kept querying after its invalid baseline; it was stopped and later guarded against baseline/event failures and unbounded event queries. Input8 reports `06 0A 07 00 11 00 00 00` and repeated input4 reports during failed queries remain undecoded, and possible query/notification feedback is unproven. No event reader was installed.

Afterward the tray displayed battery only. An isolated one-shot read validated the config header/trailer but found zeros at56/57 and almost all non-lighting prefix bytes; current strict ConfigPage validation therefore rejected lighting/sleep. No invalid page was written or validation relaxed. USB replug alone did not restore controls. With tray stopped, user opened the official driver, confirmed recognition/settings, then exited it; original tray restarted and user confirmed brightness/speed/sleep all restored. The official driver's internal actions and exact recovery cause remain unknown. Evidence: verification/keyboard-passive/REPORT.md and captures. Preserve existing3-second reader until notification semantics and reliable current values are independently established.

## Verified USB lighting hint integration (latest 2026-10-11)

The preceding passive-feasibility section describes the historical failed experiment, not the current implementation. Controlled user-observed USB brightness/speed changes subsequently correlated identical input4 `03 08 00 01` reports with separately read full-page field changes. Query-only and ordinary-arrow controls produced no corresponding arrivals in that bounded comparison; exclusivity is not established. Identical packets for different values establish a hint, not numeric telemetry. A read approximately150ms after a hint still saw the old value; two one-shot reads delayed1second correctly saw75% brightness/speed, each followed by10seconds without further reports.

The existing Worker now reads only the exact USB FF00/usage1 input4 vendor collection, waits1second, coalesces bursts up to2seconds and limits hint queries to4/minute. Failed/unchanged hinted reads or report flood disable this path until connection context changes. Three-second lighting/60-second power polling and backoff remain. Windows overlapped reads are cancelled and drained on close; no ordinary typing input, extra production thread or persistence. Source: src/keyboard_notifications.h and src/keyboard_manager.cpp; actual-Worker and Win32-substitute regressions are in final-tests.txt. User confirmed real USB percentage popups on installed2215611F... EXE.

Wireless45-second pure observation completed with real user-observed shortcut changes but zero reports: a bounded negative result only. Input8, battery/effect/sleep report fields remain undecoded. Historical invalid configuration layout remains unexplained and strict validation is retained. Latest authorized keyboard USB/radio lighting and sleep roundtrips each passed full128-byte independent equality; original6minutes restored, then user manually selected5minutes. Actual elapsed idle/wake and wireless driver coexistence were not validated. Latest evidence and limits: verification/optimization-next/REPORT.md.

## 2026-10-12 bounded unsolicited observation
Exact vendor-only zero-request observation45s per mode with tray/official driver out: USB4 input4 packets03080001 plus1 input8 packet060A070000000000; radio10 identical20-byte packets130A010004070000000000000000000000000029. Radio checksum valid and unsolicited arrival now positively observed. Existing WirelessPower rejects byte7=0; do not interpret its zeros as0% battery. Numeric battery/effect/sleep and precise trigger semantics remain unknown, no new production parser integrated. Preserve existing hint queries/polling/backoff. Evidence: verification/startup-sync-and-active/resume-20261012/REPORT.md. Latest user workflow is listen-only without lighting-effect confirmation questions.

## 2026-10-12 radio07 notification runtime integration
Exactradio07 now correlated withstatechange: baselinebrightness/speed4/4 versus1sdelayed knownquery3/3, power100/effect8/sleep300unchanged. Secondsegmentended1167whenuser switchedUSB; overallexit6, validpartialkept. Runtime strict07hint now triggersboundedfullstate refresh; values stillquery-derived, notnumericpacketdecode. Pause/cancel/drain sharedradioinput underioMutex beforequeries/settings, preserveevent and rejectoldgeneration/mode.05power subtype enforced. Mockcore/input/9radioscenarios/nativeUI passed; E05B450A installed. User explicitlydeferred productionworkerUSB/radio liveacceptance. Directbattery/effect/sleep activevalues remainunknown. Latestreport verification/startup-sync-and-active/state-packets-20261012/REPORT.md.

## 2026-10-13 verified-event update
User chose to update using existing evidence, with no further firmware search. The zero-query connection captures `13 0A 01 00 04 02 00 ... 24` and `13 0A 01 00 04 02 01 ... 25` now also schedule the existing settled full-state query. Accept exactly20 bytes, validated checksum/header, subtype02, state0/1 and zero remaining bytes; reject unknown values or other payloads. The event does not provide battery/light/sleep numbers or restore cached values. Query results remain authoritative; failed identity cannot retain connected numeric state. Existing1s settling/2s burst cap/4 reads per minute, generation/mode guard, query-reader serialization and polling/backoff remain intact. Same/unproductive events can disable hint refresh until reconnect, so polling still provides recovery. Core/notification/13 worker radio scenarios passed with fake HID only; new connection-triggered production behavior has not been hardware-accepted. See verification/startup-sync-and-active/notify-update-20261013/REPORT.md for candidate/install status.

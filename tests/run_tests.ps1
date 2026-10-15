param([switch]$Ui,[switch]$Smoke,[switch]$Render,[switch]$KeyboardUi,[switch]$SelectedUi)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
if (!(Test-Path 'bin')) { New-Item 'bin' -ItemType Directory | Out-Null }
& g++ -std=c++17 -O2 -Wall -Wextra tests/core_tests.cpp src/rapoo_protocol.cpp -o bin/core_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& g++ -std=c++17 -O2 -Wall -Wextra tests/autostart_tests.cpp -o bin/autostart_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\bin\autostart_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& g++ -std=c++17 -O2 -Wall -Wextra tests/serial_tasks_tests.cpp -o bin/serial_tasks_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\bin\serial_tasks_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\bin\core_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& g++ -std=c++17 -O2 tests/hid_feature_tests.cpp -lsetupapi -lhid -o bin/hid_feature_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\bin\hid_feature_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& g++ -std=c++17 -O2 tests/diagnostics_tests.cpp -o bin/diagnostics_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\bin\diagnostics_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& g++ -std=c++17 -O2 tests/device_notifications_tests.cpp -lhid -o bin/device_notifications_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\bin\device_notifications_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& g++ -std=c++17 -O2 -Wall -Wextra tests/keyboard_notifications_tests.cpp -o bin/keyboard_notifications_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\bin\keyboard_notifications_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& g++ -std=c++17 -O2 tests/keyboard_manager_tests.cpp src/hid_transport.cpp -lsetupapi -lhid -o bin/keyboard_manager_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\bin\keyboard_manager_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& g++ -std=c++17 -DUNICODE -D_UNICODE -O2 tests/regression_tests.cpp src/rapoo_protocol.cpp src/hid_transport.cpp src/alert_settings.cpp -lsetupapi -lhid -luser32 -lgdi32 -lshell32 -ladvapi32 -ldwmapi -o bin/regression_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\bin\regression_tests.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($SelectedUi) {
    & g++ -std=c++17 -DUNICODE -D_UNICODE -O2 tests/ui_refresh_tests.cpp src/rapoo_protocol.cpp src/device_manager.cpp src/keyboard_manager.cpp src/hid_transport.cpp src/autostart.cpp src/tray_menu.cpp src/alert_window.cpp src/alert_settings.cpp -lsetupapi -lhid -luser32 -lgdi32 -lshell32 -ladvapi32 -luxtheme -ldwmapi -o bin/ui_refresh_tests.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & .\bin\ui_refresh_tests.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & g++ -std=c++17 -DUNICODE -D_UNICODE -O2 tests/selected_ui_tests.cpp src/rapoo_protocol.cpp src/device_manager.cpp src/keyboard_manager.cpp src/hid_transport.cpp src/autostart.cpp -lsetupapi -lhid -luser32 -lgdi32 -lshell32 -ladvapi32 -luxtheme -ldwmapi -o bin/selected_ui_tests.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & .\bin\selected_ui_tests.exe --settings-only
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if ($Ui -or $Render) {
    New-Item 'verification/render' -ItemType Directory -Force | Out-Null
    & windres --codepage 65001 -I res -i res/app.rc -o bin/test_ui.o
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $taskUiSources = Get-ChildItem src/*.cpp | Where-Object { $_.Name -ne 'main.cpp' -and $_.Name -ne 'tray_menu.cpp' } | ForEach-Object FullName
    & g++ -std=c++17 -DUNICODE -D_UNICODE -O2 tests/ui_tests.cpp $taskUiSources bin/test_ui.o -lsetupapi -lhid -luser32 -lgdi32 -lshell32 -ladvapi32 -ldwmapi -luxtheme -o bin/ui_tests.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if($Render){ & .\bin\ui_tests.exe --render-only } else { & .\bin\ui_tests.exe }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if ($KeyboardUi) {
    New-Item 'verification/keyboard-ui' -ItemType Directory -Force | Out-Null
    & windres --codepage 65001 -I res -i res/app.rc -o bin/test_keyboard_ui.o
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $taskKeyboardUiSources = Get-ChildItem src/*.cpp | Where-Object { $_.Name -notin @('main.cpp','tray_menu.cpp','osd_window.cpp') } | ForEach-Object FullName
    & g++ -std=c++17 -DUNICODE -D_UNICODE -O2 -static tests/keyboard_ui_tests.cpp $taskKeyboardUiSources bin/test_keyboard_ui.o -lsetupapi -lhid -luser32 -lgdi32 -lshell32 -ladvapi32 -ldwmapi -luxtheme -o bin/keyboard_ui_tests.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & .\bin\keyboard_ui_tests.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Get-Content verification/keyboard-ui/event-results.txt
}
if ($Smoke) {
    New-Item 'verification/render' -ItemType Directory -Force | Out-Null
    & g++ -std=c++17 -O2 tests/app_smoke.cpp -luser32 -lgdi32 -ladvapi32 -lshell32 -o bin/app_smoke.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & .\bin\app_smoke.exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
exit 0

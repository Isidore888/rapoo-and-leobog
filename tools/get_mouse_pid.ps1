[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Write-Host 'VT3S MAX 大师版 V2 接口检查（只读，不修改设备或剪贴板）' -ForegroundColor Cyan
try {
    $devices = Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.InstanceId -match 'VID_24AE&PID_[0-9A-Fa-f]{4}' }
} catch {
    Write-Host '无法查询设备，请检查 Windows PnP 服务。' -ForegroundColor Red
    exit 1
}
$pids = @($devices | ForEach-Object {
    if ($_.InstanceId -match 'VID_24AE&PID_([0-9A-Fa-f]{4})') { $Matches[1].ToUpperInvariant() }
} | Sort-Object -Unique)
if (!$pids.Count) {
    Write-Host '未发现雷柏接口。请检查 USB 数据线或 2.4G 接收器。' -ForegroundColor Yellow
    return
}
foreach ($code in $pids) {
    switch ($code) {
        '466E' { Write-Host '24AE:466E · VT3S MAX 大师版 V2 · USB 有线接口' -ForegroundColor Green }
        '146E' { Write-Host '24AE:146E · VT3S MAX 大师版 V2 · 2.4G 接收器接口' -ForegroundColor Green }
        default { Write-Host "24AE:$code · 不在本项目支持范围内，无法确认型号或连接模式。" -ForegroundColor Yellow }
    }
}
Write-Host '检测到接口不代表已获得有效电量；程序还需成功读取状态报文。'

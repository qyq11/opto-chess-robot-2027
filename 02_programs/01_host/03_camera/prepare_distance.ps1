param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-zA-Z0-9_.:%-]+$')]
    [string]$BoardHost,
    [switch]$Install
)

# 默认只上传、原生编译和运行虚拟串口检查；加-Install才替换工作台。
# 所有检查均不打开真实电机串口，不发送真实运动或查询命令。
$ErrorActionPreference = 'Stop'
$projectDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$knownHosts = Join-Path $env:USERPROFILE '.ssh\known_hosts_rk3576_board_b'
if (-not (Test-Path -LiteralPath $knownHosts -PathType Leaf)) { throw '缺少板B专用SSH指纹文件。' }
$sshOptions = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
    '-o', 'HostKeyAlias=rk3576-board-b', '-o', "UserKnownHostsFile=$knownHosts", '-o', 'ConnectTimeout=8')
$runLabel = (Get-Date -Format 'yyyyMMdd_HHmmss') + '_' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$localDirectory = Join-Path $projectDirectory "temp\distance-motion-$runLabel"
$remoteDirectory = "/root/opto-chess-robot-2027/temp/distance-motion-$runLabel"
New-Item -ItemType Directory -Path $localDirectory | Out-Null
$identityGuard = @'
set -eu
serial=$(tr -d '\000\r\n' </proc/device-tree/serial-number)
printf 'BOARD_SERIAL=%s\n' "$serial"
test "$serial" = 8314b1e8ffd0321f
'@

function Invoke-Board([string]$Body, [string]$LogName) {
    $command = ($identityGuard + "`n" + $Body).Replace("`r", '')
    $command | & ssh @sshOptions "root@$BoardHost" 'tr -d ''\015'' | sh -s' 2>&1 | Tee-Object -FilePath (Join-Path $localDirectory $LogName)
    if ($LASTEXITCODE -ne 0) { throw "板B操作失败，停止后续步骤。日志：$localDirectory\$LogName" }
}

Invoke-Board ('mkdir -p ' + $remoteDirectory + '/03_camera') 'identity.log'
$bundle = Join-Path $localDirectory 'source.tar.gz'
& tar -czf $bundle -C $PSScriptRoot .
if ($LASTEXITCODE -ne 0) { throw '源码打包失败。' }
$scpHost = if ($BoardHost.Contains(':')) { "[$BoardHost]" } else { $BoardHost }
& scp @sshOptions $bundle "root@${scpHost}:$remoteDirectory/source.tar.gz"
if ($LASTEXITCODE -ne 0) { throw '源码上传失败。' }
$bundleHash = (Get-FileHash -LiteralPath $bundle -Algorithm SHA256).Hash.ToLowerInvariant()
$build = @'
work=__WORK__
test "$(sha256sum "$work/source.tar.gz" | cut -d ' ' -f 1)" = __HASH__
tar -xzf "$work/source.tar.gz" -C "$work/03_camera"
cd "$work/03_camera"
gcc --version | head -n 1
make CC=gcc BUILD_DIR=../build all motor-check distance-check
printf '\nVIRTUAL UART CHECKS ONLY; NO PHYSICAL MOTOR COMMANDS\n'
../build/motor-control-check
../build/motor-distance-check
sha256sum ../build/opto-camera motor_control.c motor_control.h Makefile diagnostics/motor_control_check.c diagnostics/motor_distance_check.c web/index.html web/motion.js web/motion.css > "$work/new-hashes.txt"
touch "$work/virtual-checks-passed"
cat "$work/new-hashes.txt"
'@
Invoke-Board ($build.Replace('__WORK__', $remoteDirectory).Replace('__HASH__', $bundleHash)) 'build-check.log'
if (-not $Install) {
    Write-Output "源码已上传，RK原生编译和虚拟串口检查通过；工作台尚未替换。日志：$localDirectory"
    return
}

# 替换前先核空闲，备份会覆盖的程序、网页和已有源码；不改网络、系统或相机参数。
$prepare = @'
work=__WORK__
test -f "$work/virtual-checks-passed"
curl -fsS http://127.0.0.1:8080/api/motion > "$work/before-motion.json"
grep -q '"moving":false' "$work/before-motion.json"
grep -q '"querying":false' "$work/before-motion.json"
test ! -e "$work/before.tar.gz"
cd /
set -- opt/opto-camera/bin/opto-camera opt/opto-camera/web/index.html opt/opto-camera/web/motion.js opt/opto-camera/web/motion.css
for file in motor_control.c motor_control.h Makefile diagnostics/motor_control_check.c diagnostics/motor_distance_check.c; do
    if [ -f "opt/opto-camera/src/$file" ]; then set -- "$@" "opt/opto-camera/src/$file"; fi
done
tar -czf "$work/before.tar.gz" "$@"
sha256sum "$work/before.tar.gz" /opt/opto-camera/bin/opto-camera /var/lib/opto-camera/camera.ini > "$work/before-hashes.txt"
cat "$work/before-hashes.txt"
'@
Invoke-Board ($prepare.Replace('__WORK__', $remoteDirectory)) 'backup.log'

$backupDirectory = Join-Path $projectDirectory '07_backups\04_网页运动'
New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null
$lastNumber = 0
Get-ChildItem -LiteralPath $backupDirectory -File | ForEach-Object {
    if ($_.Name -match '^(\d+)_') { $lastNumber = [Math]::Max($lastNumber, [int]$Matches[1]) }
}
$backupName = '{0:D2}_定距离接入前_{1}.tar.gz' -f ($lastNumber + 1), $runLabel
$metadataName = '{0:D2}_定距离回退_{1}.json' -f ($lastNumber + 2), $runLabel
$backupPath = Join-Path $backupDirectory $backupName
& scp @sshOptions "root@${scpHost}:$remoteDirectory/before.tar.gz" $backupPath
if ($LASTEXITCODE -ne 0) { throw '回退备份未成功下载，不替换工作台。' }
$backupHash = (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash.ToLowerInvariant()
if (-not (Select-String -LiteralPath (Join-Path $localDirectory 'backup.log') -SimpleMatch $backupHash -Quiet)) {
    throw '回退文件指纹不一致，不替换工作台。'
}
$metadata = [ordered]@{
    date = (Get-Date).ToString('o'); board = 'board_b'; serial = '8314b1e8ffd0321f'
    archive = $backupName; sha256 = $backupHash; remote_source = "$remoteDirectory/before.tar.gz"
    scope = '仅覆盖工作台二进制、网页和本次改动的已有源码；不含系统、网络、相机参数或驱动保存参数'
    restore = "systemctl stop opto-camera.service; tar -xzf '$remoteDirectory/before.tar.gz' -C /; systemctl start opto-camera.service"
}
$metadataPath = Join-Path $backupDirectory $metadataName
[IO.File]::WriteAllText($metadataPath, ($metadata | ConvertTo-Json -Depth 4) + "`n", [Text.UTF8Encoding]::new($false))

$installCommand = @'
work=__WORK__
curl -fsS http://127.0.0.1:8080/api/motion > "$work/install-motion.json"
grep -q '"moving":false' "$work/install-motion.json"
grep -q '"querying":false' "$work/install-motion.json"
cd "$work/03_camera"
sha256sum -c "$work/new-hashes.txt"
config_hash=$(sha256sum /var/lib/opto-camera/camera.ini | cut -d ' ' -f 1)
grep -q "$config_hash  /var/lib/opto-camera/camera.ini" "$work/before-hashes.txt"
rollback() {
    systemctl stop opto-camera.service || true
    tar -xzf "$work/before.tar.gz" -C /
    systemctl start opto-camera.service || true
}
trap rollback EXIT
systemctl stop opto-camera.service
install -m 755 "$work/build/opto-camera" /opt/opto-camera/bin/opto-camera
install -d /opt/opto-camera/src/diagnostics
install -m 644 motor_control.c motor_control.h Makefile /opt/opto-camera/src/
install -m 644 diagnostics/motor_control_check.c diagnostics/motor_distance_check.c /opt/opto-camera/src/diagnostics/
install -m 644 web/index.html web/motion.js web/motion.css /opt/opto-camera/web/
systemctl start opto-camera.service
ready=0
for i in $(seq 1 30); do
    if curl -fsS http://127.0.0.1:8080/api/motion > "$work/after-motion.json"; then ready=1; break; fi
    sleep 1
done
test "$ready" = 1
grep -q '"moving":false' "$work/after-motion.json"
grep -q '"distance_mm"' "$work/after-motion.json"
ready=0
for i in $(seq 1 15); do
    if curl -fsS http://127.0.0.1:8080/api/status > "$work/after-camera.json" && grep -q '"fresh":true' "$work/after-camera.json"; then ready=1; break; fi
    sleep 1
done
test "$ready" = 1
curl -fsS http://127.0.0.1:8080/ > "$work/after-page.html"
grep -q 'motion-distance-number' "$work/after-page.html"
test "$(sha256sum /var/lib/opto-camera/camera.ini | cut -d ' ' -f 1)" = "$config_hash"
systemctl show opto-camera.service -p MainPID -p NRestarts -p ActiveState
cat "$work/after-motion.json"
cat "$work/after-camera.json"
trap - EXIT
'@
Invoke-Board ($installCommand.Replace('__WORK__', $remoteDirectory)) 'install.log'
Write-Output "工作台已替换，尚未发送真实运动；请现场刷新页面后手动启动。回退：$backupPath；日志：$localDirectory"

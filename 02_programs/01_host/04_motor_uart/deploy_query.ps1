param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-zA-Z0-9_.:%-]+$')]
    [string]$BoardHost
)

$ErrorActionPreference = 'Stop'
$knownHosts = Join-Path $env:USERPROFILE '.ssh/known_hosts_rk3576_board_b'
if (-not (Test-Path -LiteralPath $knownHosts -PathType Leaf)) {
    throw "缺少板B专用指纹文件：$knownHosts"
}
$sshOptions = @(
    '-o', 'BatchMode=yes',
    '-o', 'StrictHostKeyChecking=yes',
    '-o', 'HostKeyAlias=rk3576-board-b',
    '-o', "UserKnownHostsFile=$knownHosts",
    '-o', 'ConnectTimeout=8'
)
$identityGuard = @'
set -eu
serial=$(tr -d '\000\r\n' </proc/device-tree/serial-number)
printf 'BOARD_SERIAL=%s\n' "$serial"
[ "$serial" = 8314b1e8ffd0321f ] || exit 42
'@
$identityGuard = $identityGuard.Replace("`r", '') + "`n"
$runLabel = (Get-Date -Format 'yyyyMMdd_HHmmss') + '_' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$remoteDirectory = "/root/opto-chess-robot-2027/temp/motor-uart-query-$runLabel"
# IPv6本地地址可附带网口编号（例如%7）；SCP要求IPv6地址放在方括号内。
$scpBoardHost = if ($BoardHost.Contains(':')) { "[$BoardHost]" } else { $BoardHost }

function Invoke-BoardCommand {
    param([string]$RemoteCommand)
    & ssh @sshOptions "root@$BoardHost" ($identityGuard + $RemoteCommand.Replace("`r", ''))
    if ($LASTEXITCODE -ne 0) {
        throw "板B命令失败，SSH退出码=$LASTEXITCODE；停止后续操作。"
    }
}

$prepareCommand = @'
date -Is
cat /proc/sys/kernel/random/boot_id
readlink -f /sys/class/tty/ttyS4/device/of_node
for pid in $(pgrep -x SN450_CPU3; pgrep -x sn100_shell; pgrep -x rkaiq_3A_server); do
  ps -p "$pid" -o pid,lstart,stat,comm
done
systemctl show opto-camera.service -p MainPID -p ActiveState -p SubState
'@
Invoke-BoardCommand ($prepareCommand + "`n" + 'mkdir -p ' + $remoteDirectory)
& scp @sshOptions (Join-Path $PSScriptRoot 'motor_query.c') (Join-Path $PSScriptRoot 'Makefile') "root@${scpBoardHost}:$remoteDirectory/"
if ($LASTEXITCODE -ne 0) {
    throw "上传失败，SCP退出码=$LASTEXITCODE；未继续编译。"
}
$buildCommand = @'
cd '__WORK__'
sha256sum motor_query.c Makefile
gcc --version | head -n 1
make CC=gcc BUILD_DIR=build
readelf -h build/motor-query
build/motor-query --help
status=0
build/motor-query --port /dev/ttyS4 || status=$?
[ "$status" -eq 2 ] || exit 44
printf 'MISSING_ARGUMENTS_EXIT=%s; NO_UART_OPEN_REQUESTED\n' "$status"
systemctl show opto-camera.service -p MainPID -p ActiveState -p SubState
printf 'QUERY_BINARY=__WORK__/build/motor-query\n'
'@
Invoke-BoardCommand ($buildCommand.Replace('__WORK__', $remoteDirectory))
Write-Output '源码已上传，板端编译及帮助/缺参检查已完成；尚未打开UART或发送查询。'
Write-Output "后续核实接线后，使用：$remoteDirectory/build/motor-query"

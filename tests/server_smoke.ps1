param(
    [string]$Server = "$PSScriptRoot/../server/build/lan-audio-server.exe",
    [int]$Port = 15010,
    [int]$DiscoveryPort = 15011
)
$ErrorActionPreference = 'Stop'
$serverPath = (Resolve-Path -LiteralPath $Server).Path
$outputDir = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$process = Start-Process -FilePath $serverPath -ArgumentList "$Port", "$DiscoveryPort" -WindowStyle Hidden -PassThru -RedirectStandardOutput "$outputDir/server-smoke.out.log" -RedirectStandardError "$outputDir/server-smoke.err.log"
$client = [System.Net.Sockets.UdpClient]::new()
$probe = [System.Net.Sockets.UdpClient]::new()
function Read-NetworkInteger([byte[]]$Bytes, [int]$Offset, [int]$Length) {
    [uint64]$value = 0
    for ($i = 0; $i -lt $Length; $i++) { $value = $value * 256 + $Bytes[$Offset + $i] }
    return $value
}
try {
    $probe.Connect('127.0.0.1', $DiscoveryPort)
    $probe.Client.ReceiveTimeout = 200
    $probe.Client.ReceiveBufferSize = 256 * 1024
    $endpoint = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
    $query = [byte[]](76, 65, 68, 81, 1, 18, 52, 86, 120)
    function Assert-DiscoveryReply([byte[]]$response) {
        if ($response.Length -lt 15 -or $response.Length -gt 78 -or
            (Read-NetworkInteger $response 0 4) -ne 1279345746 -or $response[4] -ne 1 -or
            (Read-NetworkInteger $response 5 4) -ne 305419896 -or $response[9] -ne 2 -or
            (Read-NetworkInteger $response 10 2) -ne $Port -or
            (Read-NetworkInteger $response 12 2) -ne 96 -or $response.Length -ne 15 + $response[14]) {
            throw 'Malformed discovery response'
        }
    }
    function Discover-Server {
        [void]$probe.Send($query, $query.Length)
        Assert-DiscoveryReply ($probe.Receive([ref]$endpoint))
    }
    $discovered = $false
    for ($attempt = 0; $attempt -lt 15; $attempt++) {
        try { Discover-Server; $discovered = $true; break }
        catch [System.Net.Sockets.SocketException] { Start-Sleep -Milliseconds 100 }
    }
    if (!$discovered) { throw 'Discovery failed' }
    try { [void]$probe.Receive([ref]$endpoint); throw 'Discovery unexpectedly started audio' }
    catch [System.Net.Sockets.SocketException] {
        if ($_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::TimedOut) { throw }
    }
    $invalidQuery = [byte[]](76, 65, 68, 81, 2, 18, 52, 86, 120)
    [void]$probe.Send($invalidQuery, $invalidQuery.Length)
    try { [void]$probe.Receive([ref]$endpoint); throw 'Unsupported discovery version was accepted' }
    catch [System.Net.Sockets.SocketException] {
        if ($_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::TimedOut) { throw }
    }
    $client.Connect('127.0.0.1', $Port)
    $client.Client.ReceiveTimeout = 200
    $client.Client.ReceiveBufferSize = 256 * 1024
    $endpoint = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
    $hello = [byte[]](76, 65, 72, 73, 2)
    $acked = $false
    for ($attempt = 0; $attempt -lt 15; $attempt++) {
        if ($process.HasExited) { throw 'Server exited before handshake' }
        [void]$client.Send($hello, $hello.Length)
        try {
            $reply = $client.Receive([ref]$endpoint)
            if ($reply.Length -eq 5 -and [System.BitConverter]::ToString($reply) -eq '4C-41-4F-4B-02') { $acked = $true; break }
        } catch [System.Net.Sockets.SocketException] {
            if ($_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::TimedOut -and $_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::ConnectionReset) { throw }
        }
        Start-Sleep -Milliseconds 100
    }
    if (!$acked) { throw 'Protocol v2 handshake failed' }
    # Keepalive must also be acknowledged after capture has started.
    [void]$client.Send($hello, $hello.Length)
    $acked = $false
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        $reply = $client.Receive([ref]$endpoint)
        if ($reply.Length -eq 5 -and [System.BitConverter]::ToString($reply) -eq '4C-41-4F-4B-02') { $acked = $true; break }
        if ($reply.Length -ne 808 -or $reply[4] -ne 2 -or $reply[5] -ne 2) { throw 'Malformed audio datagram' }
    }
    if (!$acked) { throw 'Keepalive acknowledgement failed' }
    # Discovery while streaming must leave the existing audio destination intact.
    Discover-Server
    # Empty and oversized datagrams must not block the following valid burst.
    $empty = [byte[]]::new(0)
    $oversized = [byte[]]::new(128)
    [void]$probe.Send($empty, 0)
    [void]$probe.Send($oversized, $oversized.Length)
    [void]$client.Send($empty, 0)
    [void]$client.Send($oversized, $oversized.Length)
    $burstWatch = [System.Diagnostics.Stopwatch]::StartNew()
    for ($i = 0; $i -lt 32; $i++) {
        [void]$probe.Send($query, $query.Length)
        [void]$client.Send($hello, $hello.Length)
    }
    for ($i = 0; $i -lt 32; $i++) { Assert-DiscoveryReply ($probe.Receive([ref]$endpoint)) }
    $ackCount = 0
    while ($ackCount -lt 32 -and $burstWatch.ElapsedMilliseconds -lt 2000) {
        $reply = $client.Receive([ref]$endpoint)
        if ($reply.Length -eq 5 -and [System.BitConverter]::ToString($reply) -eq '4C-41-4F-4B-02') { $ackCount++ }
        elseif ($reply.Length -ne 808 -or $reply[4] -ne 2 -or $reply[5] -ne 2) { throw 'Malformed burst audio datagram' }
    }
    if ($ackCount -ne 32) { throw 'Control burst lost acknowledgements' }
    Write-Output "32 discovery and 32 keepalive burst replies received in $($burstWatch.ElapsedMilliseconds) ms"
    $audioPackets = 0
    [uint64]$previousSession = 0
    [uint64]$previousTimestamp = 0
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt 1000) {
        $reply = $client.Receive([ref]$endpoint)
        if ($reply.Length -eq 5) { continue }
        if ($reply.Length -ne 808 -or $reply[4] -ne 2 -or $reply[5] -ne 2) { throw 'Malformed paced audio datagram' }
        $rate = Read-NetworkInteger $reply 8 4
        $sequence = Read-NetworkInteger $reply 16 4
        $session = Read-NetworkInteger $reply 20 8
        $timestamp = Read-NetworkInteger $reply 28 8
        if ($timestamp % 96 -ne 0 -or ([uint64]($timestamp / 96) % 4294967296) -ne $sequence) { throw 'Invalid packet timeline' }
        if ($previousSession -eq $session -and $timestamp -le $previousTimestamp) { throw 'Audio timeline did not advance' }
        $previousSession = $session; $previousTimestamp = $timestamp
        $audioPackets++
    }
    if ($audioPackets -lt ($rate / 96 / 2) -or $audioPackets -gt ($rate / 96 * 2)) { throw "Unexpected audio cadence: $audioPackets packets at $rate Hz" }
    Write-Output "Discovery without audio takeover, protocol-v2 handshake, keepalive and paced audio passed: $audioPackets packets at $rate Hz"
} finally {
    $probe.Dispose()
    $client.Dispose()
    # Only stop the helper process created by this test, never an existing server.
    if (!$process.HasExited) { Stop-Process -Id $process.Id -Force }
    $process.Dispose()
}

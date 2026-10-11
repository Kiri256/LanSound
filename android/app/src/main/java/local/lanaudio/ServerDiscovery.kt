package local.lanaudio

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.SecureRandom
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.concurrent.thread

data class DiscoveredServer(val name: String, val host: String, val port: Int, val protocol: Int, val frames: Int, val networkPriority: Int = 2) {
    val compatible: Boolean get() = protocol == 2 && frames == BuildConfig.PACKET_FRAMES
    val label: String get() = "$name  $host:$port" + if (compatible) "" else " (不兼容)"
}

class ServerDiscovery(
    private val knownHost: String,
    private val onFound: (DiscoveredServer) -> Unit,
    private val onFinished: (String?) -> Unit,
) : AutoCloseable {
    @Volatile private var closed = false
    private val sockets = CopyOnWriteArrayList<DatagramSocket>()

    private data class Probe(val socket: DatagramSocket, val targets: Set<InetAddress>, val priority: Int)

    private fun openProbe(local: InetAddress, targets: Set<InetAddress>, priority: Int): Probe? {
        val udp = try { DatagramSocket(0, local) } catch (_: java.io.IOException) { return null }
        sockets.add(udp)
        try {
            if (closed) { udp.close(); return null }
            udp.broadcast = true
            udp.soTimeout = 5
            return Probe(udp, targets, priority)
        } catch (_: java.io.IOException) {
            udp.close()
            return null
        }
    }

    private fun probes(): List<Probe> {
        val knownAddress = knownHost.split('.').takeIf { parts ->
            parts.size == 4 && parts.all { it.toIntOrNull()?.let { value -> value in 0..255 } == true }
        }?.let { InetAddress.getByName(knownHost) }
        val result = mutableListOf<Probe>()
        val networks = runCatching { NetworkInterface.getNetworkInterfaces()?.toList().orEmpty() }.getOrDefault(emptyList())
        val locals = mutableSetOf<InetAddress>()
        for (network in networks) {
            runCatching {
                if (network.isUp && !network.isLoopback) {
                    for (address in network.interfaceAddresses) {
                        val local = address.address
                        if (local !is Inet4Address || !locals.add(local)) continue
                        val prefix = address.networkPrefixLength.toInt()
                        if (prefix !in 1..30) continue
                        val value = ByteBuffer.wrap(local.address).int
                        val mask = -1 shl (32 - prefix)
                        val bytes = ByteBuffer.allocate(4).putInt(value or mask.inv()).array()
                        val destinations = linkedSetOf(InetAddress.getByAddress(bytes))
                        if (knownAddress != null && (ByteBuffer.wrap(knownAddress.address).int and mask) == (value and mask)) {
                            destinations.add(knownAddress)
                        }
                        val name = network.displayName.lowercase()
                        val priority = when {
                            "rndis" in name || "usb" in name || "tether" in name -> 0
                            "ethernet" in name -> 1
                            else -> 2
                        }
                        openProbe(local, destinations, priority)?.let(result::add)
                    }
                }
            }
        }
        return result
    }

    fun start() {
        thread(name = "LanAudio-discovery") {
            try {
                val probes = probes()
                if (closed) return@thread
                check(probes.isNotEmpty()) { "未找到可用 IPv4 网卡，请检查 USB 网络共享" }
                val nonce = SecureRandom().nextInt()
                val query = ByteBuffer.allocate(9).order(ByteOrder.BIG_ENDIAN)
                    .putInt(0x4c414451).put(1.toByte()).putInt(nonce).array()
                val buffer = ByteArray(79)
                val deadline = System.nanoTime() + 3_000_000_000L
                var nextQuery = 0L
                while (!closed && System.nanoTime() < deadline) {
                    if (System.nanoTime() >= nextQuery) {
                        probes.forEach { probe ->
                            probe.targets.forEach { target ->
                                try { probe.socket.send(DatagramPacket(query, query.size, target, 30011)) }
                                catch (_: java.io.IOException) { /* Other interfaces may still reach a server. */ }
                            }
                        }
                        nextQuery = System.nanoTime() + 700_000_000L
                    }
                    for (probe in probes) {
                        if (closed || System.nanoTime() >= deadline) break
                        val packet = DatagramPacket(buffer, buffer.size)
                        try { probe.socket.receive(packet) }
                        catch (_: SocketTimeoutException) { continue }
                        catch (_: java.io.IOException) { continue }
                        decode(packet, nonce, probe.priority)?.let(onFound)
                    }
                    Thread.sleep(10)
                }
                if (!closed) onFinished(null)
            } catch (error: Exception) {
                if (!closed) onFinished(error.message ?: "扫描失败")
            } finally { sockets.forEach { it.close() }; sockets.clear() }
        }
    }

    override fun close() { closed = true; sockets.forEach { it.close() } }

    private fun decode(packet: DatagramPacket, nonce: Int, networkPriority: Int): DiscoveredServer? {
        if (packet.address !is Inet4Address || packet.length < 15) return null
        val reply = ByteBuffer.wrap(packet.data, packet.offset, packet.length).order(ByteOrder.BIG_ENDIAN)
        if (reply.int != 0x4c414452 || reply.get().toInt() != 1 || reply.int != nonce) return null
        val protocol = reply.get().toInt() and 255
        val port = reply.short.toInt() and 65535
        val frames = reply.short.toInt() and 65535
        val nameLength = reply.get().toInt() and 255
        if (port == 0 || nameLength > 63 || reply.remaining() != nameLength) return null
        val bytes = ByteArray(nameLength); reply.get(bytes)
        val host = packet.address.hostAddress ?: return null
        val name = bytes.toString(Charsets.UTF_8).filter { !it.isISOControl() }.ifBlank { "Windows" }
        return DiscoveredServer(name, host, port, protocol, frames, networkPriority)
    }
}

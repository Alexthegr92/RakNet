# RakNet RDMA Support

This implementation adds RDMA (Remote Direct Memory Access) support to RakNet using libfabric, providing ultra-low latency networking for datacenter and high-performance computing applications.

## Overview

RDMA enables direct memory access between computers without involving the operating system, CPU, or cache, resulting in:
- **Sub-microsecond latency** (vs milliseconds for TCP/UDP)
- **High throughput** (100+ Gbps)
- **Zero-copy data transfer**
- **Minimal CPU overhead**

## Architecture

The implementation follows RakNet's existing transport patterns:

```
RDMAInterface.h/cpp       - Low-level RDMA transport (like TCPInterface)
PacketizedRDMA.h/cpp      - Packet framing wrapper (like PacketizedTCP)
PluginInterface2          - Integration with RakNet plugin system
```

### Key Components

1. **RDMAInterface**: Core RDMA transport layer
   - libfabric integration
   - Connection management
   - Memory region registration
   - Send/Receive operations

2. **PacketizedRDMA**: Message framing
   - Adds 4-byte length headers
   - Handles message boundaries
   - Compatible with RakNet's packet system

3. **RDMABufferPool**: Pre-registered memory management
   - 1024 buffers of 64KB each
   - Registered with RDMA hardware for zero-copy
   - Thread-safe allocation/deallocation

## Connection Modes

- **RDMA_MODE_RC** (Reliable Connected): Like TCP, ordered and reliable
- **RDMA_MODE_UD** (Unreliable Datagram): Like UDP, unordered
- **RDMA_MODE_RD** (Reliable Datagram): Datagram with reliability

## Prerequisites

### Software
```bash
# Ubuntu/Debian
sudo apt-get install libfabric-dev

# RHEL/CentOS
sudo yum install libfabric-devel

# Build from source
git clone https://github.com/ofiwg/libfabric
cd libfabric
./autogen.sh
./configure --prefix=/usr
make
sudo make install
```

### Hardware (Optional)
- InfiniBand HCA (e.g., Mellanox ConnectX)
- RoCE-capable Ethernet NIC
- Intel Omni-Path adapter

**Note**: Software emulation ("sockets" provider) works without RDMA hardware for testing.

## Building

1. Enable RDMA support in `Source/NativeFeatureIncludes.h`:
```cpp
#define _RAKNET_SUPPORT_RDMAInterface 1
#define _RAKNET_SUPPORT_PacketizedRDMA 1
```

2. Add source files to your build:
   - `Source/RDMAInterface.cpp`
   - `Source/PacketizedRDMA.cpp`

3. Link with libfabric:
```bash
g++ -o myapp myapp.cpp -lRakNet -lfabric
```

## Usage Example

### Server
```cpp
#include "PacketizedRDMA.h"

PacketizedRDMA* rdma = PacketizedRDMA::GetInstance();

// Start RDMA server
rdma->Start(
    60000,                    // Port
    10,                       // Max connections
    10,                       // Max total connections
    0,                        // Thread priority
    RDMA_MODE_RC,             // Reliable Connected mode
    "verbs",                  // Provider (use "sockets" for testing)
    nullptr                   // Bind address
);

// Receive messages
while (true) {
    Packet* packet = rdma->Receive();
    if (packet) {
        // Process packet
        rdma->DeallocatePacket(packet);
    }
}
```

### Client
```cpp
PacketizedRDMA* rdma = PacketizedRDMA::GetInstance();
rdma->Start(0, 1, 1, 0, RDMA_MODE_RC, "verbs", nullptr);

// Connect to server
SystemAddress server = rdma->Connect("192.168.1.100", 60000, true);

// Send message
const char* msg = "Hello RDMA!";
rdma->Send(msg, strlen(msg) + 1, server, false);
```

## Testing

Software emulation (no RDMA hardware needed):

```bash
# Terminal 1 - Start server
cd Samples/RDMAEchoServer
./rdma_echo_server

# Terminal 2 - Run client
./rdma_echo_client localhost
```

## Performance Considerations

### Best Practices
1. **Pre-register memory**: Use the buffer pool for frequently sent data
2. **Batch operations**: Coalesce small messages when possible
3. **Tune buffer sizes**: Match to your message patterns
4. **Use appropriate mode**: RC for reliability, UD for lowest latency

### Expected Performance (with RDMA hardware)
- **Latency**: 1-5 microseconds
- **Throughput**: 25-100 Gbps
- **CPU usage**: < 10% per core

### Software Emulation Performance
- **Latency**: ~50-100 microseconds
- **Throughput**: Limited by TCP/IP stack
- **Purpose**: Development and testing only

## Use Cases

### Ideal For
✅ Datacenter server-to-server communication  
✅ Distributed physics simulation (PhysX clustering)  
✅ High-frequency trading systems  
✅ Distributed databases  
✅ Clustered game servers  
✅ Machine learning training (distributed)

### Not Suitable For
❌ Client-server gaming (clients don't have RDMA)  
❌ Internet communication (NAT traversal impossible)  
❌ Mobile/embedded devices  
❌ Small message workloads without hardware

## Integration with PhysX

Perfect for distributed physics simulation in your `distributed-physics-visualiser` project:

```cpp
// Synchronize PhysX state between nodes with sub-millisecond latency
void SyncPhysicsState(PacketizedRDMA* rdma, SystemAddress peer) {
    // Serialize PhysX state
    RakNet::BitStream bs;
    SerializePhysicsActors(bs);
    
    // Send with RDMA (ultra-low latency)
    rdma->Send((const char*)bs.GetData(), bs.GetNumberOfBytesUsed(), peer, false);
}
```

## Troubleshooting

### "fi_getinfo failed"
- Check libfabric installation: `fi_info`
- Try software provider: `"sockets"` instead of `"verbs"`
- Verify RDMA hardware: `ibstat` (InfiniBand) or `show_gids` (RoCE)

### "Failed to register buffer"
- Check locked memory limits: `ulimit -l unlimited`
- Verify sufficient memory available
- Try smaller buffer pool size

### Connection failures
- Ensure firewall allows RDMA traffic
- Check provider compatibility between client/server
- Verify network reachability with `ping`

## Limitations

1. **Current Implementation**
   - Thread management is stubbed (needs worker threads)
   - Active connection establishment incomplete
   - No RDMA Write/Read operations (only Send/Recv)
   - Limited error handling

2. **Production Readiness**
   - This is a **prototype/proof-of-concept**
   - Complete thread implementation needed
   - Add comprehensive error handling
   - Implement connection retry logic
   - Add performance instrumentation

3. **Platform Support**
   - Linux: Full support
   - Windows: Requires NetworkDirect
   - macOS: No native RDMA support

## Future Enhancements

- [ ] Complete thread implementation (UpdateRDMAInterfaceLoop)
- [ ] RDMA Write/Read operations for true zero-copy
- [ ] Dynamic buffer pool sizing
- [ ] Connection migration/failover
- [ ] Performance profiling hooks
- [ ] Windows NetworkDirect support
- [ ] Integration with ReplicaManager3
- [ ] Hybrid UDP/RDMA fallback

## References

- [libfabric Documentation](https://ofiwg.github.io/libfabric/)
- [RDMA Programming Guide](https://www.rdmamojo.com/)
- [RakNet Documentation](http://www.jenkinssoftware.com/)
- [PhysX SDK](https://github.com/NVIDIAGameWorks/PhysX)

## License

Same as RakNet (BSD License). See LICENSE file in repository root.

## Author

Created as an enhancement to RakNet for distributed physics simulation applications.

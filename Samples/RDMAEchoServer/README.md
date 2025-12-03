# RDMA Echo Server/Client Example

Simple echo server and client demonstrating RakNet's RDMA support via libfabric.

## Prerequisites

- libfabric installed (libfabric_new.dll must be in PATH or same directory as executable)
- For real RDMA: RDMA-capable hardware (InfiniBand, RoCE, or Omni-Path)
- For testing: sockets provider (software emulation, limited functionality)

## Building

```powershell
cd C:\repos\distributed-physics-visualiser\PhysX-3.4\externals\raknet
msbuild "Samples\RDMAEchoServer\RDMAEchoSample.sln" /p:Configuration=Release
```

Executables will be in `Samples\RDMAEchoServer\Release\`

## Usage

### Server

```powershell
.\rdma_echo_server.exe <client_ip:port> [client_ip:port ...]
```

**Example:**
```powershell
.\rdma_echo_server.exe 192.168.1.100:60001 192.168.1.101:60001
```

The server listens on port 60000 and needs to know client addresses in advance (required for RDM mode with sockets provider).

### Client

```powershell
.\rdma_echo_client.exe <server_ip> [client_port]
```

**Example:**
```powershell
.\rdma_echo_client.exe 192.168.1.50 60001
```

- `server_ip`: IP address of the server (default: 127.0.0.1)
- `client_port`: Port for client to bind to (default: 0 = any port). If specified, server should be told this address.

## Environment Variables

### For Sockets Provider (Testing)
```powershell
$env:FI_PROVIDER='sockets'
$env:FI_PROVIDER_EXCLUDE='efa'
```

### For Real RDMA Hardware
```powershell
$env:FI_PROVIDER='verbs'   # For InfiniBand/RoCE
# OR
$env:FI_PROVIDER='psm2'    # For Omni-Path
```

## Complete Example (Sockets Provider)

Terminal 1 (Server):
```powershell
cd Samples\RDMAEchoServer\Release
$env:FI_PROVIDER='sockets'
$env:FI_PROVIDER_EXCLUDE='efa'
.\rdma_echo_server.exe 127.0.0.1:60001
```

Terminal 2 (Client):
```powershell
cd Samples\RDMAEchoServer\Release
$env:FI_PROVIDER='sockets'
$env:FI_PROVIDER_EXCLUDE='efa'
.\rdma_echo_client.exe 127.0.0.1 60001
```

## Known Limitations

### Sockets Provider
- **Limited RDM support**: The sockets provider's FI_EP_RDM implementation has known issues
- **Address resolution**: Both endpoints must know each other's addresses in advance
- **Send failures**: fi_send may fail with ENOENT despite correct setup
- **Purpose**: Software emulation for development only, not for production use

### Production Use
For actual low-latency RDMA networking, use real RDMA hardware with:
- **verbs provider**: InfiniBand or RoCE (RDMA over Converged Ethernet)
- **psm2 provider**: Intel Omni-Path Architecture
- **efa provider**: AWS Elastic Fabric Adapter (cloud RDMA)

## How It Works

1. Server starts and listens on port 60000
2. Server pre-inserts client addresses into its address vector (for RDM mode)
3. Client starts and binds to specified port (or any available port)
4. Client connects to server and inserts server address into its address vector
5. Both endpoints can now exchange messages using connectionless RDM semantics
6. Server echoes back any received messages

## Architecture

- **FI_EP_RDM**: Reliable Datagram Message endpoint type (connectionless with reliability)
- **Address Vector**: FI_AV_TABLE mode for simple indexed addressing
- **Shared Endpoint**: Single endpoint per interface, all clients share it
- **Worker Thread**: Background thread processes completion queue events
- **Buffer Pool**: Pre-registered memory regions for zero-copy transfers (with real RDMA)

## Troubleshooting

**"Failed to start RDMA server"**
- Ensure libfabric_new.dll is accessible
- Check FI_PROVIDER environment variable
- Verify no other process is using port 60000

**"fi_send failed: No such file or directory"**
- With sockets provider, this is expected due to implementation limitations
- With real RDMA hardware, check that both endpoints have inserted addresses
- Verify network connectivity and RDMA fabric configuration

**"Connection timeout"**
- Server may not have client's address pre-inserted
- Check that client port matches what server expects
- Verify network connectivity

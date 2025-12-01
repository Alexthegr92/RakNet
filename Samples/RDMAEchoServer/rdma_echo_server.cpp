/*
 * RDMA Echo Server Example
 * Demonstrates basic usage of RakNet's RDMA interface
 * 
 * This example creates a server that listens for RDMA connections
 * and echoes back any received messages.
 * 
 * Prerequisites:
 * - libfabric installed (sudo apt-get install libfabric-dev)
 * - RDMA-capable hardware OR use software emulation with "sockets" provider
 * Compile with: g++ -o rdma_echo_server rdma_echo_server.cpp -I../../Source -L../../Lib -lRakNetLibStatic -lfabric -lpthread
 */

// Enable RDMA support (MUST be defined before any RakNet includes)
#define _RAKNET_SUPPORT_RDMAInterface 1
#define _RAKNET_SUPPORT_PacketizedRDMA 1

#include <stdio.h>
#include <string.h>
#include "RakPeerInterface.h"
#include "MessageIdentifiers.h"
#include "BitStream.h"
#include "RakNetTypes.h"
#include "GetTime.h"
#include "RakSleep.h"

#if _RAKNET_SUPPORT_PacketizedRDMA==1
#include "PacketizedRDMA.h"
#endif

using namespace RakNet;

#define MAX_CLIENTS 10
#define SERVER_PORT 60000

int main(void)
{
#if _RAKNET_SUPPORT_PacketizedRDMA==1
    printf("RakNet RDMA Echo Server Example\n");
    printf("================================\n\n");

    // Create RDMA interface
    PacketizedRDMA *rdmaInterface = PacketizedRDMA::GetInstance();
    
    if (!rdmaInterface)
    {
        printf("Failed to create RDMA interface\n");
        return 1;
    }

    // Start the RDMA server
    // Using "sockets" provider for software emulation (works without RDMA hardware)
    // For real RDMA, use "verbs" (InfiniBand/RoCE) or "psm2" (Omni-Path)
    printf("Starting RDMA server on port %d...\n", SERVER_PORT);
    printf("Provider: sockets (software emulation)\n");
    printf("Connection mode: RC (Reliable Connected)\n\n");
    
    bool startResult = rdmaInterface->Start(
        SERVER_PORT,                  // Port
        MAX_CLIENTS,                  // Max incoming connections
        MAX_CLIENTS,                  // Max connections
        0,                            // Thread priority
        RDMA_MODE_RC,                 // Connection mode (Reliable Connected)
        "sockets",                    // Provider name (software emulation)
        nullptr                       // Bind address (any)
    );

    if (!startResult)
    {
        printf("Failed to start RDMA server!\n");
        printf("Make sure libfabric is installed: sudo apt-get install libfabric-dev\n");
        PacketizedRDMA::DestroyInstance(rdmaInterface);
        return 1;
    }

    printf("RDMA server started successfully!\n");
    printf("Waiting for connections...\n");
    printf("Press Ctrl+C to exit\n\n");

    // Main server loop
    Packet *packet;
    unsigned char packetIdentifier;
    
    while (true)
    {
        // Check for new connections
        SystemAddress newConnection = rdmaInterface->HasNewIncomingConnection();
        if (newConnection != UNASSIGNED_SYSTEM_ADDRESS)
        {
            printf("[+] New RDMA connection from %s\n", newConnection.ToString());
        }

        // Check for lost connections
        SystemAddress lostConnection = rdmaInterface->HasLostConnection();
        if (lostConnection != UNASSIGNED_SYSTEM_ADDRESS)
        {
            printf("[-] Lost RDMA connection from %s\n", lostConnection.ToString());
        }

        // Process incoming packets
        for (packet = rdmaInterface->Receive(); packet; 
             rdmaInterface->DeallocatePacket(packet), packet = rdmaInterface->Receive())
        {
            packetIdentifier = packet->data[0];

            switch (packetIdentifier)
            {
                case ID_NEW_INCOMING_CONNECTION:
                    printf("[CONNECT] New incoming connection from %s\n", packet->systemAddress.ToString());
                    break;

                case ID_DISCONNECTION_NOTIFICATION:
                    printf("[DISCONNECT] %s disconnected\n", packet->systemAddress.ToString());
                    break;

                case ID_CONNECTION_LOST:
                    printf("[LOST] Connection lost from %s\n", packet->systemAddress.ToString());
                    break;

                default:
                    // Echo the message back
                    printf("[ECHO] Received %d bytes from %s, echoing back...\n", 
                           packet->length, packet->systemAddress.ToString());
                    
                    rdmaInterface->Send((const char*)packet->data, packet->length, 
                                       packet->systemAddress, false);
                    break;
            }
        }

        // Sleep to avoid busy-waiting (RDMA is event-driven in production)
        RakSleep(30);
    }

    // Cleanup
    rdmaInterface->Stop();
    PacketizedRDMA::DestroyInstance(rdmaInterface);

    return 0;

#else
    printf("RDMA support not compiled in!\n");
    printf("Set _RAKNET_SUPPORT_RDMAInterface=1 in NativeFeatureIncludes.h\n");
    return 1;
#endif
}

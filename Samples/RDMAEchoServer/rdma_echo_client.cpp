/*
 * RDMA Echo Client Example
 * Demonstrates basic usage of RakNet's RDMA interface from client side
 * 
 * This example creates a client that connects to the RDMA echo server
 * and sends messages, then receives the echoed responses.
 * 
 * Prerequisites:
 * - libfabric installed
 * - RDMA echo server running
 * Compile with: g++ -o rdma_echo_client rdma_echo_client.cpp -I../../Source -L../../Lib -lRakNetLibStatic -lfabric -lpthread
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

#define SERVER_PORT 60000

int main(int argc, char *argv[])
{
#if _RAKNET_SUPPORT_PacketizedRDMA==1
    const char *serverAddress = "127.0.0.1";
    
    if (argc > 1)
        serverAddress = argv[1];

    printf("RakNet RDMA Echo Client Example\n");
    printf("================================\n\n");

    // Create RDMA interface
    PacketizedRDMA *rdmaInterface = PacketizedRDMA::GetInstance();
    
    if (!rdmaInterface)
    {
        printf("Failed to create RDMA interface\n");
        return 1;
    }

    // Start the RDMA interface (required before connecting)
    printf("Initializing RDMA client...\n");
    bool startResult = rdmaInterface->Start(
        0,                            // Port (0 = any port for client)
        1,                            // Max incoming (not used for client)
        1,                            // Max connections
        0,                            // Thread priority
        RDMA_MODE_RC,                 // Connection mode
        "sockets",                    // Provider name
        nullptr                       // Bind address
    );

    if (!startResult)
    {
        printf("Failed to initialize RDMA client!\n");
        PacketizedRDMA::DestroyInstance(rdmaInterface);
        return 1;
    }

    printf("Connecting to %s:%d...\n", serverAddress, SERVER_PORT);
    
    SystemAddress serverAddr = rdmaInterface->Connect(serverAddress, SERVER_PORT, true);
    
    if (serverAddr == UNASSIGNED_SYSTEM_ADDRESS)
    {
        printf("Failed to connect to server!\n");
        rdmaInterface->Stop();
        PacketizedRDMA::DestroyInstance(rdmaInterface);
        return 1;
    }

    // Wait for connection to complete
    printf("Waiting for connection to complete...\n");
    bool connected = false;
    TimeMS startTime = GetTimeMS();
    
    while (GetTimeMS() - startTime < 5000) // 5 second timeout
    {
        SystemAddress completedConn = rdmaInterface->HasCompletedConnectionAttempt();
        if (completedConn != UNASSIGNED_SYSTEM_ADDRESS)
        {
            printf("Successfully connected to %s!\n\n", completedConn.ToString());
            connected = true;
            break;
        }
        
        SystemAddress failedConn = rdmaInterface->HasFailedConnectionAttempt();
        if (failedConn != UNASSIGNED_SYSTEM_ADDRESS)
        {
            printf("Connection attempt failed!\n");
            break;
        }
        
        RakSleep(100);
    }

    if (!connected)
    {
        printf("Connection timeout or failed!\n");
        rdmaInterface->Stop();
        PacketizedRDMA::DestroyInstance(rdmaInterface);
        return 1;
    }

    // Send test messages
    printf("Sending test messages...\n\n");
    
    const char* testMessages[] = {
        "Hello RDMA!",
        "Low latency networking",
        "Zero-copy data transfer",
        "Distributed physics simulation",
        "RakNet + libfabric"
    };
    
    int numMessages = sizeof(testMessages) / sizeof(testMessages[0]);
    
    for (int i = 0; i < numMessages; i++)
    {
        printf("[SEND %d/%d] %s\n", i+1, numMessages, testMessages[i]);
        rdmaInterface->Send(testMessages[i], (unsigned int)strlen(testMessages[i]) + 1, 
                           serverAddr, false);
        RakSleep(500); // Small delay between sends
    }

    // Receive echoed responses
    printf("\nWaiting for echo responses...\n");
    int receivedCount = 0;
    startTime = GetTimeMS();
    
    while (receivedCount < numMessages && (GetTimeMS() - startTime < 10000))
    {
        Packet *packet = rdmaInterface->Receive();
        
        if (packet)
        {
            printf("[RECV %d/%d] Echo: %s\n", receivedCount+1, numMessages, packet->data);
            receivedCount++;
            rdmaInterface->DeallocatePacket(packet);
        }
        else
        {
            RakSleep(50);
        }
    }

    if (receivedCount == numMessages)
        printf("\nAll messages echoed successfully!\n");
    else
        printf("\nReceived %d/%d echo responses\n", receivedCount, numMessages);

    // Cleanup
    printf("\nDisconnecting...\n");
    rdmaInterface->CloseConnection(serverAddr);
    RakSleep(100);
    rdmaInterface->Stop();
    PacketizedRDMA::DestroyInstance(rdmaInterface);

    printf("Client finished.\n");
    return 0;

#else
    printf("RDMA support not compiled in!\n");
    return 1;
#endif
}

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
    int serverPort = SERVER_PORT;
    
    if (argc > 1)
        serverAddress = argv[1];
    if (argc > 2)
        serverPort = atoi(argv[2]);

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

    printf("Client interface started, now connecting...\n");
    fflush(stdout);

    printf("Connecting to %s:%d...\n", serverAddress, serverPort);
    fflush(stdout);
    
    SystemAddress serverAddr = rdmaInterface->Connect(serverAddress, serverPort, true);
    
    printf("Connect() returned, checking result...\n");
    fflush(stdout);
    
    if (serverAddr == UNASSIGNED_SYSTEM_ADDRESS)
    {
        printf("Failed to connect to server!\n");
        rdmaInterface->Stop();
        PacketizedRDMA::DestroyInstance(rdmaInterface);
        return 1;
    }

    // Wait for connection to complete
    printf("Waiting for connection to complete...\n");
    fflush(stdout);
    bool connected = false;
    TimeMS startTime = GetTimeMS();
    
    printf("Starting wait loop (timeout=5000ms)...\n");
    fflush(stdout);
    
    while (GetTimeMS() - startTime < 5000) // 5 second timeout
    {
        printf("Checking for completed connection...\n");
        fflush(stdout);
        SystemAddress completedConn = rdmaInterface->HasCompletedConnectionAttempt();
        printf("HasCompletedConnectionAttempt returned %s\n", completedConn.ToString());
        fflush(stdout);
        if (completedConn != UNASSIGNED_SYSTEM_ADDRESS)
        {
            printf("Connection detected! Setting connected=true\n");
            fflush(stdout);
            printf("Successfully connected to %s!\n\n", completedConn.ToString());
            fflush(stdout);
            connected = true;
            printf("Breaking from loop...\n");
            fflush(stdout);
            break;
        }
        
        printf("Checking for failed connection...\n");
        fflush(stdout);
        SystemAddress failedConn = rdmaInterface->HasFailedConnectionAttempt();
        printf("HasFailedConnectionAttempt returned %s\n", failedConn.ToString());
        fflush(stdout);
        if (failedConn != UNASSIGNED_SYSTEM_ADDRESS)
        {
            printf("Connection attempt failed!\n");
            break;
        }
        
        printf("Sleeping 100ms...\n");
        fflush(stdout);
        RakSleep(100);
    }
    
    printf("Exited wait loop, connected=%d\n", connected);
    fflush(stdout);

    printf("About to check connected flag...\n");
    fflush(stdout);
    
    if (!connected)
    {
        printf("Connection timeout or failed!\n");
        rdmaInterface->Stop();
        PacketizedRDMA::DestroyInstance(rdmaInterface);
        return 1;
    }

    printf("Connected flag is true, preparing to send...\n");
    fflush(stdout);
    
    // Send test messages
    printf("Sending test messages...\n\n");
    fflush(stdout);
    
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
        fflush(stdout);
        
        rdmaInterface->Send(testMessages[i], (unsigned int)strlen(testMessages[i]) + 1, 
                           serverAddr, false);
        RakSleep(500); // Small delay between sends
    }

    // Receive echoed responses
    printf("\nWaiting for echo responses...\n");
    fflush(stdout);
    int receivedCount = 0;
    startTime = GetTimeMS();
    
    while (receivedCount < numMessages && (GetTimeMS() - startTime < 10000))
    {
        Packet *packet = rdmaInterface->Receive();
        
        if (packet)
        {
            printf("[RECV %d/%d] Echo: %s\n", receivedCount+1, numMessages, packet->data);
            fflush(stdout);
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
    
    fflush(stdout);

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

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_RDMAInterface==1

#include "RDMAInterface.h"
#include "RakSleep.h"
#include "RakAssert.h"
#include "StringCompressor.h"
#include "StringTable.h"
#include "Itoa.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <WinSock2.h>
#include <ws2tcpip.h>
#include "WSAStartupSingleton.h"
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

using namespace RakNet;

// Forward declarations for thread functions
RAK_THREAD_DECLARATION(UpdateRDMAInterfaceLoop);

STATIC_FACTORY_DEFINITIONS(RDMAInterface, RDMAInterface);

// Buffer Pool Implementation
RDMABufferPool::RDMABuffer* RDMABufferPool::AllocateBuffer()
{
	poolMutex.Lock();
	for (unsigned int i = 0; i < POOL_SIZE; i++)
	{
		if (!buffers[i].inUse)
		{
			buffers[i].inUse = true;
			poolMutex.Unlock();
			return &buffers[i];
		}
	}
	poolMutex.Unlock();
	return nullptr; // Pool exhausted
}

void RDMABufferPool::FreeBuffer(RDMABuffer* buffer)
{
	if (buffer)
	{
		poolMutex.Lock();
		buffer->inUse = false;
		poolMutex.Unlock();
	}
}

void RDMABufferPool::RegisterBuffers(struct fid_domain* domain)
{
	int successCount = 0;
	for (unsigned int i = 0; i < POOL_SIZE; i++)
	{
		buffers[i].data = (char*)rakMalloc_Ex(BUFFER_SIZE, _FILE_AND_LINE_);
		
		// Register memory region with RDMA
		int ret = fi_mr_reg(domain, buffers[i].data, BUFFER_SIZE,
		                    FI_SEND | FI_RECV, 0, 0, 0, &buffers[i].mr, nullptr);
		
		if (ret != 0)
		{
			// Sockets provider often can't register buffers - this is OK for RDM mode
			// Silently continue - we only print summary
			buffers[i].mr = nullptr;
		}
		else
		{
			successCount++;
		}
	}
	printf("RDMA: Buffer registration: %d/%u succeeded\n", successCount, POOL_SIZE);
}

void RDMABufferPool::DeregisterBuffers()
{
	for (unsigned int i = 0; i < POOL_SIZE; i++)
	{
		if (buffers[i].mr)
		{
			fi_close(&buffers[i].mr->fid);
			buffers[i].mr = nullptr;
		}
		if (buffers[i].data)
		{
			rakFree_Ex(buffers[i].data, _FILE_AND_LINE_);
			buffers[i].data = nullptr;
		}
	}
}

// RDMAInterface Implementation
RDMAInterface::RDMAInterface()
{
	hints = nullptr;
	fabric_info = nullptr;
	fabric = nullptr;
	domain = nullptr;
	listener = nullptr;
	event_queue = nullptr;
	completion_queue = nullptr;
	address_vector = nullptr;
	main_endpoint = nullptr;
	remoteClients = nullptr;
	remoteClientsLength = 0;
	control_sockfd = -1;
	local_ep_namelen = 0;
	memset(local_ep_name, 0, sizeof(local_ep_name));
	// isStarted and threadRunning are LocklessUint32_t, initialized to 0 by default constructor
	threadPriority = 0;
	connectionMode = RDMA_MODE_RC;
	maxConnections = 0;
	
#ifdef _WIN32
	WSAStartupSingleton::AddRef();
#endif
}

RDMAInterface::~RDMAInterface()
{
	Stop();
	
#ifdef _WIN32
	WSAStartupSingleton::Deref();
#endif
}

bool RDMAInterface::InitializeFabric(const char* providerName)
{
	printf("RDMA: Initializing fabric with provider '%s'...\n", providerName ? providerName : "default");
	
	// Allocate hints structure
	hints = fi_allocinfo();
	if (!hints)
	{
		printf("RDMA: fi_allocinfo failed\n");
		return false;
	}

	// Configure hints based on connection mode
	// Use FI_EP_RDM (Reliable Datagram) - standard connectionless mode with AV
	// This is what EFA_Test.cpp uses and works with sockets provider
	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG;
	hints->mode = 0;
	hints->domain_attr->mr_mode = 0;
	hints->addr_format = FI_SOCKADDR_IN;

	if (providerName)
	{
		// Don't use strdup - just point to the string, libfabric will copy if needed
		hints->fabric_attr->prov_name = (char*)providerName;
	}

	// Get fabric info
	printf("RDMA: Calling fi_getinfo...\n");
	int ret = fi_getinfo(FI_VERSION(1, 9), nullptr, nullptr, 0, hints, &fabric_info);
	if (ret != 0)
	{
		printf("RDMA: fi_getinfo failed: %s\n", fi_strerror(-ret));
		return false;
	}
	printf("RDMA: fi_getinfo succeeded\n");

	// Open fabric
	printf("RDMA: Opening fabric...\n");
	ret = fi_fabric(fabric_info->fabric_attr, &fabric, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_fabric failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Open domain
	printf("RDMA: Opening domain...\n");
	ret = fi_domain(fabric, fabric_info, &domain, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_domain failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Create event queue
	printf("RDMA: Creating event queue...\n");
	struct fi_eq_attr eq_attr = {0};
	eq_attr.size = 1024;
	eq_attr.wait_obj = FI_WAIT_UNSPEC;
	ret = fi_eq_open(fabric, &eq_attr, &event_queue, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_eq_open failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Create completion queue
	printf("RDMA: Creating completion queue...\n");
	struct fi_cq_attr cq_attr = {0};
	cq_attr.size = 4096;
	cq_attr.format = FI_CQ_FORMAT_MSG;
	ret = fi_cq_open(domain, &cq_attr, &completion_queue, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_cq_open failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Create address vector for MSG mode (like EFA_Test.cpp)
	printf("RDMA: Creating address vector...\n");
	struct fi_av_attr av_attr = {0};
	av_attr.type = FI_AV_MAP;  // Use map type for address translation
	av_attr.count = 1024;       // Max addresses
	ret = fi_av_open(domain, &av_attr, &address_vector, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_av_open failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Register buffer pool
	printf("RDMA: Registering buffer pool...\n");
	bufferPool.RegisterBuffers(domain);
	printf("RDMA: Fabric initialization complete\n");

	return true;
}

void RDMAInterface::CleanupFabric()
{
	bufferPool.DeregisterBuffers();

	if (main_endpoint)
	{
		fi_close(&main_endpoint->fid);
		main_endpoint = nullptr;
	}

	if (address_vector)
	{
		fi_close(&address_vector->fid);
		address_vector = nullptr;
	}

	if (completion_queue)
	{
		fi_close(&completion_queue->fid);
		completion_queue = nullptr;
	}

	if (event_queue)
	{
		fi_close(&event_queue->fid);
		event_queue = nullptr;
	}

	if (listener)
	{
		fi_close(&listener->fid);
		listener = nullptr;
	}

	if (domain)
	{
		fi_close(&domain->fid);
		domain = nullptr;
	}

	if (fabric)
	{
		fi_close(&fabric->fid);
		fabric = nullptr;
	}

	if (fabric_info)
	{
		fi_freeinfo(fabric_info);
		fabric_info = nullptr;
	}

	if (hints)
	{
		fi_freeinfo(hints);
		hints = nullptr;
	}
}

bool RDMAInterface::CreateListenEndpoint(unsigned short port, const char* hostAddress)
{
	// Create passive endpoint for listening
	int ret = fi_passive_ep(fabric, fabric_info, &listener, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_passive_ep failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Bind event queue to listener
	ret = fi_pep_bind(listener, &event_queue->fid, 0);
	if (ret != 0)
	{
		printf("RDMA: fi_pep_bind failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Setup address
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	
	if (hostAddress && strlen(hostAddress) > 0)
	{
		inet_pton(AF_INET, hostAddress, &addr.sin_addr);
	}
	else
	{
		addr.sin_addr.s_addr = INADDR_ANY;
	}

	// Bind event queue to passive endpoint
	ret = fi_pep_bind(listener, &event_queue->fid, 0);
	if (ret != 0)
	{
		printf("RDMA: Failed to bind event queue: %s\n", fi_strerror(-ret));
		return false;
	}

	// Start listening
	ret = fi_listen(listener);
	if (ret != 0)
	{
		printf("RDMA: fi_listen failed: %s\n", fi_strerror(-ret));
		return false;
	}

	return true;
}

// Control socket helpers for address exchange (matching EFA_Test pattern)
int RDMAInterface::ControlSocketServerInit(unsigned short port)
{
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
	{
#ifdef _WIN32
		printf("RDMA: Control socket creation failed: error %d\n", WSAGetLastError());
#else
		printf("RDMA: Control socket creation failed\n");
#endif
		return -1;
	}
	printf("RDMA: Created control socket %d\n", sockfd);

	// Allow reuse
	int opt = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
#ifdef _WIN32
		printf("RDMA: Control socket bind failed: error %d\n", WSAGetLastError());
		closesocket(sockfd);
#else
		printf("RDMA: Control socket bind failed\n");
		close(sockfd);
#endif
		return -1;
	}
	printf("RDMA: Control socket bound to port %u\n", port);

	if (listen(sockfd, 5) < 0)
	{
#ifdef _WIN32
		printf("RDMA: Control socket listen failed: error %d\n", WSAGetLastError());
		closesocket(sockfd);
#else
		printf("RDMA: Control socket listen failed\n");
		close(sockfd);
#endif
		return -1;
	}

	// Set socket to non-blocking mode
#ifdef _WIN32
	u_long mode = 1;
	if (ioctlsocket(sockfd, FIONBIO, &mode) != 0)
	{
		printf("RDMA: ioctlsocket failed: error %d\n", WSAGetLastError());
		closesocket(sockfd);
		return -1;
	}
#else
	int flags = fcntl(sockfd, F_GETFL, 0);
	if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0)
	{
		printf("RDMA: fcntl failed to set non-blocking\n");
		close(sockfd);
		return -1;
	}
#endif

	printf("RDMA: Control socket listening on port %u\n", port);
	return sockfd;
}

int RDMAInterface::ControlSocketClientConnect(const char* host, unsigned short port)
{
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
	{
#ifdef _WIN32
		printf("RDMA: Control socket creation failed: error %d\n", WSAGetLastError());
#else
		printf("RDMA: Control socket creation failed\n");
#endif
		return -1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, host, &addr.sin_addr);

	if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
#ifdef _WIN32
		printf("RDMA: Control socket connect failed: error %d\n", WSAGetLastError());
		closesocket(sockfd);
#else
		printf("RDMA: Control socket connect failed\n");
		close(sockfd);
#endif
		return -1;
	}

	printf("RDMA: Control socket connected to %s:%u\n", host, port);
	return sockfd;
}

fi_addr_t RDMAInterface::ControlSocketExchangeAddresses(int sockfd, bool isServer)
{
	printf("RDMA: ControlSocketExchangeAddresses START (isServer=%d, sockfd=%d)\n", isServer, sockfd);
	fflush(stdout);
	
	// Exchange endpoint addresses via control socket (like EFA_Test pp_ctrl_txrx)
	if (isServer)
	{
		printf("RDMA: Server path - sending local address (%zu bytes)\n", local_ep_namelen);
		fflush(stdout);
		
		// Server: send local address first, then receive client address
		int sent = send(sockfd, local_ep_name, local_ep_namelen, 0);
		if (sent != (int)local_ep_namelen)
		{
			printf("RDMA: Failed to send local address (sent=%d, WSAGetLastError=%d)\n", sent, WSAGetLastError());
			fflush(stdout);
			return FI_ADDR_UNSPEC;
		}
		
		printf("RDMA: Sent local address, now receiving client address\n");
		fflush(stdout);
		
		// Wait for data to be available (non-blocking socket)
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET((SOCKET)sockfd, &readfds);
		struct timeval timeout;
		timeout.tv_sec = 5;
		timeout.tv_usec = 0;
		
		int select_result = select(sockfd + 1, &readfds, nullptr, nullptr, &timeout);
		if (select_result <= 0)
		{
			printf("RDMA: select() failed or timed out waiting for client address (result=%d, WSAGetLastError=%d)\n", 
			       select_result, WSAGetLastError());
			fflush(stdout);
			return FI_ADDR_UNSPEC;
		}
		
		// Receive client address
		char remote_ep_name[64];
		int received = recv(sockfd, remote_ep_name, sizeof(remote_ep_name), 0);
		if (received <= 0)
		{
			printf("RDMA: Failed to receive client address (received=%d, WSAGetLastError=%d)\n", received, WSAGetLastError());
			fflush(stdout);
			return FI_ADDR_UNSPEC;
		}
		
		printf("RDMA: Received client address (%d bytes), inserting into AV\n", received);
		fflush(stdout);
		
		// Insert client address into AV
		fi_addr_t client_fi_addr;
		int ret = fi_av_insert(address_vector, remote_ep_name, 1, &client_fi_addr, 0, nullptr);
		if (ret != 1)
		{
			printf("RDMA: Failed to insert client address: %s\n", fi_strerror(-ret));
			fflush(stdout);
			return FI_ADDR_UNSPEC;
		}
		printf("RDMA: Client connected, fi_addr=%lu\n", (unsigned long)client_fi_addr);
		fflush(stdout);
		return client_fi_addr;
	}
	else
	{
		// Client: receive server address first
		// Wait for data to be available (non-blocking socket)
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET((SOCKET)sockfd, &readfds);
		struct timeval timeout;
		timeout.tv_sec = 5;
		timeout.tv_usec = 0;
		
		int select_result = select(sockfd + 1, &readfds, nullptr, nullptr, &timeout);
		if (select_result <= 0)
		{
			printf("RDMA: Client select() failed or timed out waiting for server address (result=%d, WSAGetLastError=%d)\n", 
			       select_result, WSAGetLastError());
			fflush(stdout);
			return FI_ADDR_UNSPEC;
		}
		
		char remote_ep_name[64];
		int received = recv(sockfd, remote_ep_name, sizeof(remote_ep_name), 0);
		if (received <= 0)
		{
			printf("RDMA: Failed to receive server address (WSAGetLastError=%d)\n", WSAGetLastError());
			return FI_ADDR_UNSPEC;
		}
		
		// Insert server address into AV
		fi_addr_t server_fi_addr;
		int ret = fi_av_insert(address_vector, remote_ep_name, 1, &server_fi_addr, 0, nullptr);
		if (ret != 1)
		{
			printf("RDMA: Failed to insert server address: %s\n", fi_strerror(-ret));
			return FI_ADDR_UNSPEC;
		}
		printf("RDMA: Connected to server, fi_addr=%lu\n", (unsigned long)server_fi_addr);
		
		// Now send our address back
		int sent = send(sockfd, local_ep_name, local_ep_namelen, 0);
		if (sent != (int)local_ep_namelen)
		{
			printf("RDMA: Failed to send local address (WSAGetLastError=%d)\n", WSAGetLastError());
			return FI_ADDR_UNSPEC;
		}
		return server_fi_addr;
	}
	return FI_ADDR_UNSPEC;
}

bool RDMAInterface::Start(unsigned short port, 
                          unsigned short maxIncomingConnections, 
                          unsigned short maxConn,
                          int _threadPriority,
                          RDMAConnectionMode connMode,
                          const char* providerName,
                          const char* bindAddress)
{
	if (isStarted.GetValue() != 0)
		return false;

	connectionMode = connMode;
	threadPriority = _threadPriority;
	maxConnections = (maxConn == 0) ? maxIncomingConnections : maxConn;

	if (!InitializeFabric(providerName))
	{
		CleanupFabric();
		return false;
	}

	// Create main endpoint (both client and server use AV-based addressing like EFA_Test.cpp)
	printf("RDMA: Creating main endpoint with AV...\n");
	int ret = fi_endpoint(domain, fabric_info, &main_endpoint, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_endpoint failed: %s\n", fi_strerror(-ret));
		CleanupFabric();
		return false;
	}

	// Bind address vector to endpoint
	ret = fi_ep_bind(main_endpoint, &address_vector->fid, 0);
	if (ret != 0)
	{
		printf("RDMA: fi_ep_bind AV failed: %s\n", fi_strerror(-ret));
		CleanupFabric();
		return false;
	}

	// Bind completion queue for transmit and receive
	ret = fi_ep_bind(main_endpoint, &completion_queue->fid, FI_TRANSMIT | FI_RECV);
	if (ret != 0)
	{
		printf("RDMA: fi_ep_bind CQ failed: %s\n", fi_strerror(-ret));
		CleanupFabric();
		return false;
	}

	// Enable endpoint (AFTER all bindings)
	ret = fi_enable(main_endpoint);
	if (ret != 0)
	{
		printf("RDMA: fi_enable failed: %s\n", fi_strerror(-ret));
		CleanupFabric();
		return false;
	}
	
	// Get the actual endpoint address (needed for address exchange)
	local_ep_namelen = sizeof(local_ep_name);
	ret = fi_getname(&main_endpoint->fid, local_ep_name, &local_ep_namelen);
	if (ret == 0)
	{
		struct sockaddr_in* addr = (struct sockaddr_in*)local_ep_name;
		printf("RDMA: Endpoint address: %s:%u (namelen=%zu)\n", 
		       inet_ntoa(addr->sin_addr), ntohs(addr->sin_port), local_ep_namelen);
	}
	else
	{
		printf("RDMA: fi_getname failed: %s\n", fi_strerror(-ret));
	}
	
	// For server mode, create control socket listener
	if (port > 0)
	{
		control_sockfd = ControlSocketServerInit(port);
		if (control_sockfd < 0)
		{
			printf("RDMA: Failed to create control socket\n");
			CleanupFabric();
			return false;
		}
	}

	// Allocate remote clients array
	remoteClientsLength = maxConnections;
	remoteClients = RakNet::OP_NEW_ARRAY<RDMARemoteClient>(remoteClientsLength, _FILE_AND_LINE_);

	isStarted.Increment();
	threadRunning.Increment();

	// Start worker thread for handling RDMA events
	printf("RDMA: Starting worker thread...\n");
	fflush(stdout);
	int errorCode = RakNet::RakThread::Create(UpdateRDMAInterfaceLoop, this, threadPriority);
	printf("RDMA: RakThread::Create returned %d\n", errorCode);
	fflush(stdout);
	if (errorCode != 0)
	{
		printf("RDMA: Worker thread creation failed with error %d\n", errorCode);
		Stop();
		return false;
	}

	printf("RDMA: Interface started successfully!\n");
	fflush(stdout);
	return true;
}

void RDMAInterface::Stop(void)
{
	if (isStarted.GetValue() == 0)
		return;

	isStarted.Decrement();
	
	// Wait for thread to finish
	while (threadRunning.GetValue() > 0)
		RakSleep(10);

	// Wait for threads to complete
	RakSleep(100);

	// Close all connections
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive)
		{
			CloseConnection(remoteClients[i].systemAddress);
		}
	}

	// Cleanup
	if (remoteClients)
	{
		RakNet::OP_DELETE_ARRAY(remoteClients, _FILE_AND_LINE_);
		remoteClients = nullptr;
	}

	CleanupFabric();

	// Clear queues
	Packet* packet;
	while ((packet = headPush.Pop()))
		DeallocatePacket(packet);
	while ((packet = tailPush.Pop()))
		DeallocatePacket(packet);
}

SystemAddress RDMAInterface::Connect(const char* host, unsigned short remotePort, bool block)
{
	printf("RDMA: Connect() called for %s:%u\n", host, remotePort);
	
	if (isStarted.GetValue() == 0)
	{
		printf("RDMA: Connect failed - interface not started\n");
		return UNASSIGNED_SYSTEM_ADDRESS;
	}

	// Find available client slot
	int clientIndex = -1;
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (!remoteClients[i].isActive)
		{
			clientIndex = i;
			break;
		}
	}

	if (clientIndex == -1)
	{
		printf("RDMA: Connect failed - no slots available\n");
		return UNASSIGNED_SYSTEM_ADDRESS;
	}

	// Connect control socket for address exchange (EFA_Test pattern)
	printf("RDMA: Connecting control socket to %s:%u...\n", host, remotePort);
	int ctrl_sock = ControlSocketClientConnect(host, remotePort);
	if (ctrl_sock < 0)
	{
		printf("RDMA: Control socket connection failed\n");
		return UNASSIGNED_SYSTEM_ADDRESS;
	}

	// Exchange addresses via control socket
	fi_addr_t server_fi_addr = ControlSocketExchangeAddresses(ctrl_sock, false);  // false = client mode
	
	if (server_fi_addr == FI_ADDR_UNSPEC)
	{
		printf("RDMA: Address exchange failed\n");
#ifdef _WIN32
		closesocket(ctrl_sock);
#else
		close(ctrl_sock);
#endif
		return UNASSIGNED_SYSTEM_ADDRESS;
	}
	
	// Close control socket after exchange
#ifdef _WIN32
	closesocket(ctrl_sock);
#else
	close(ctrl_sock);
#endif

	// Setup client structure
	RDMARemoteClient& client = remoteClients[clientIndex];
	client.endpoint = main_endpoint;  // Use shared endpoint
	client.fi_addr = server_fi_addr;
	
	// Set system address
	struct sockaddr_in dest_addr;
	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(remotePort);
	inet_pton(AF_INET, host, &dest_addr.sin_addr);
	
	client.systemAddress.address.addr4.sin_addr.s_addr = dest_addr.sin_addr.s_addr;
	client.systemAddress.address.addr4.sin_family = AF_INET;
	client.systemAddress.SetPortHostOrder(remotePort);
	client.sendBuffer = bufferPool.AllocateBuffer();
	client.recvBuffer = bufferPool.AllocateBuffer();
	client.SetActive(true);

	// Post initial receive
	if (client.recvBuffer)
	{
		void* desc = client.recvBuffer->mr ? fi_mr_desc(client.recvBuffer->mr) : nullptr;
		fi_recv(main_endpoint, client.recvBuffer->data, RDMABufferPool::BUFFER_SIZE, desc, FI_ADDR_UNSPEC, client.recvBuffer);
	}

	completedConnectionAttemptMutex.Lock();
	completedConnectionAttempts.Push(client.systemAddress, _FILE_AND_LINE_);
	completedConnectionAttemptMutex.Unlock();

	printf("RDMA: Connection setup complete\n");
	return client.systemAddress;
}

void RDMAInterface::Send(const char* data, unsigned int length, const SystemAddress& systemAddress, bool broadcast)
{
	printf("RDMA: RDMAInterface::Send (len=%u, broadcast=%d)\n", length, broadcast);
	fflush(stdout);
	
	if (broadcast)
	{
		for (int i = 0; i < remoteClientsLength; i++)
		{
			if (remoteClients[i].isActive)
			{
				printf("RDMA: Broadcasting to client %d\n", i);
				remoteClients[i].Send(data, length);
			}
		}
	}
	else
	{
		// Find the specific client
		bool found = false;
		for (int i = 0; i < remoteClientsLength; i++)
		{
			if (remoteClients[i].isActive && remoteClients[i].systemAddress == systemAddress)
			{
				printf("RDMA: Found client %d for address %s\n", i, systemAddress.ToString());
				remoteClients[i].Send(data, length);
				found = true;
				break;
			}
		}
		if (!found)
		{
			printf("RDMA: No active client found for address %s\n", systemAddress.ToString());
		}
	}
}

bool RDMAInterface::SendList(const char** data, const unsigned int* lengths, const int numParameters,
                             const SystemAddress& systemAddress, bool broadcast)
{
	if (numParameters == 0)
		return false;

	// Calculate total length
	unsigned int totalLength = 0;
	for (int i = 0; i < numParameters; i++)
		totalLength += lengths[i];

	// Allocate concatenated buffer
	char* buffer = (char*)rakMalloc_Ex(totalLength, _FILE_AND_LINE_);
	unsigned int offset = 0;
	
	for (int i = 0; i < numParameters; i++)
	{
		memcpy(buffer + offset, data[i], lengths[i]);
		offset += lengths[i];
	}

	Send(buffer, totalLength, systemAddress, broadcast);
	
	rakFree_Ex(buffer, _FILE_AND_LINE_);
	return true;
}

unsigned int RDMAInterface::GetOutgoingDataBufferSize(SystemAddress systemAddress) const
{
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive && remoteClients[i].systemAddress == systemAddress)
		{
			return remoteClients[i].outgoingData.GetBytesWritten();
		}
	}
	return 0;
}

bool RDMAInterface::ReceiveHasPackets(void)
{
	return headPush.Size() > 0 || tailPush.Size() > 0 || incomingMessages.Size() > 0;
}

Packet* RDMAInterface::Receive(void)
{
	Packet* packet = nullptr;
	
	if (!headPush.IsEmpty())
	{
		packet = headPush.Pop();
		if (packet)
			return packet;
	}

	if (!incomingMessages.IsEmpty())
	{
		packet = incomingMessages.Pop();
		if (packet)
			return packet;
	}

	if (!tailPush.IsEmpty())
	{
		return tailPush.Pop();
	}
	
	return nullptr;
}

Packet* RDMAInterface::ReceiveInt(void)
{
	return Receive();
}

void RDMAInterface::CloseConnection(SystemAddress systemAddress)
{
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive && remoteClients[i].systemAddress == systemAddress)
		{
			// In AV-based mode, we don't close the endpoint (it's shared)
			// Just mark inactive and free buffers
			remoteClients[i].endpoint = nullptr;
			
			if (remoteClients[i].sendBuffer)
			{
				bufferPool.FreeBuffer(remoteClients[i].sendBuffer);
				remoteClients[i].sendBuffer = nullptr;
			}
			
			if (remoteClients[i].recvBuffer)
			{
				bufferPool.FreeBuffer(remoteClients[i].recvBuffer);
				remoteClients[i].recvBuffer = nullptr;
			}
			
			remoteClients[i].SetActive(false);
			SystemAddress* sa = lostConnections.Allocate(_FILE_AND_LINE_);
			*sa = systemAddress;
			lostConnections.Push(sa);
			break;
		}
	}
}

void RDMAInterface::DeallocatePacket(Packet* packet)
{
	if (packet)
	{
		rakFree_Ex(packet->data, _FILE_AND_LINE_);
		RakNet::OP_DELETE(packet, _FILE_AND_LINE_);
	}
}

void RDMAInterface::GetConnectionList(SystemAddress* remoteSystems, unsigned short* numberOfSystems) const
{
	unsigned short count = 0;
	
	for (int i = 0; i < remoteClientsLength && count < *numberOfSystems; i++)
	{
		if (remoteClients[i].isActive && remoteSystems)
		{
			remoteSystems[count] = remoteClients[i].systemAddress;
			count++;
		}
	}
	
	*numberOfSystems = count;
}

unsigned short RDMAInterface::GetConnectionCount(void) const
{
	unsigned short count = 0;
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive)
			count++;
	}
	return count;
}

SystemAddress RDMAInterface::HasCompletedConnectionAttempt(void)
{
	completedConnectionAttemptMutex.Lock();
	SystemAddress sa = completedConnectionAttempts.IsEmpty() ? UNASSIGNED_SYSTEM_ADDRESS : completedConnectionAttempts.Pop();
	completedConnectionAttemptMutex.Unlock();
	return sa;
}

SystemAddress RDMAInterface::HasFailedConnectionAttempt(void)
{
	failedConnectionAttemptMutex.Lock();
	SystemAddress sa = failedConnectionAttempts.IsEmpty() ? UNASSIGNED_SYSTEM_ADDRESS : failedConnectionAttempts.Pop();
	failedConnectionAttemptMutex.Unlock();
	return sa;
}

SystemAddress RDMAInterface::HasNewIncomingConnection(void)
{
	if (newIncomingConnections.IsEmpty())
		return UNASSIGNED_SYSTEM_ADDRESS;
	SystemAddress* sa = newIncomingConnections.Pop();
	if (!sa)
		return UNASSIGNED_SYSTEM_ADDRESS;
	SystemAddress ret = *sa;
	newIncomingConnections.Deallocate(sa, _FILE_AND_LINE_);
	return ret;
}

SystemAddress RDMAInterface::HasLostConnection(void)
{
	if (lostConnections.IsEmpty())
		return UNASSIGNED_SYSTEM_ADDRESS;
	SystemAddress* sa = lostConnections.Pop();
	if (!sa)
		return UNASSIGNED_SYSTEM_ADDRESS;
	SystemAddress ret = *sa;
	lostConnections.Deallocate(sa, _FILE_AND_LINE_);
	return ret;
}

Packet* RDMAInterface::AllocatePacket(unsigned dataSize)
{
	Packet* packet = RakNet::OP_NEW<Packet>(_FILE_AND_LINE_);
	packet->data = (unsigned char*)rakMalloc_Ex(dataSize, _FILE_AND_LINE_);
	packet->length = dataSize;
	packet->bitSize = BYTES_TO_BITS(dataSize);
	packet->deleteData = false;
	return packet;
}

void RDMAInterface::PushBackPacket(Packet* packet, bool pushAtHead)
{
	if (pushAtHead)
		headPush.Push(packet, _FILE_AND_LINE_);
	else
		tailPush.Push(packet, _FILE_AND_LINE_);
}

bool RDMAInterface::WasStarted(void) const
{
	return isStarted.GetValue() != 0;
}

void RDMAInterface::AttachPlugin(PluginInterface2* plugin)
{
	if (messageHandlerList.GetIndexOf(plugin) == (unsigned)-1)
	{
		messageHandlerList.Insert(plugin, _FILE_AND_LINE_);
		plugin->SetRDMAInterface(this);
		plugin->OnAttach();
	}
}

void RDMAInterface::DetachPlugin(PluginInterface2* plugin)
{
	unsigned int index = messageHandlerList.GetIndexOf(plugin);
	if (index != (unsigned)-1)
	{
		messageHandlerList.RemoveAtIndex(index);
		plugin->OnDetach();
	}
}

// RDMARemoteClient Implementation
int RDMARemoteClient::Send(const char* data, unsigned int length)
{
	if (!isActive)
	{
		printf("RDMA: Send failed - client not active\n");
		return -1;
	}
	if (!endpoint)
	{
		printf("RDMA: Send failed - no endpoint\n");
		return -1;
	}
	if (!sendBuffer)
	{
		printf("RDMA: Send failed - no send buffer\n");
		return -1;
	}

	// Check if data fits in buffer
	if (length > RDMABufferPool::BUFFER_SIZE)
	{
		printf("RDMA: Send failed - data too large (%u > %u)\n", length, RDMABufferPool::BUFFER_SIZE);
		return -1;
	}

	// Copy data to pre-registered buffer
	memcpy(sendBuffer->data, data, length);

	// Post RDMA send to specific destination address
	// Handle NULL mr for sockets provider
	void* desc = sendBuffer->mr ? fi_mr_desc(sendBuffer->mr) : nullptr;
	printf("RDMA: Calling fi_send(ep=%p, buf=%p, len=%u, desc=%p, fi_addr=%lu, ctx=%p)\n",
	       endpoint, sendBuffer->data, length, desc, (unsigned long)fi_addr, this);
	fflush(stdout);
	int ret = fi_send(endpoint, sendBuffer->data, length, 
	                  desc, fi_addr, this);
	printf("RDMA: fi_send returned %d\n", ret);
	fflush(stdout);
	
	if (ret != 0)
	{
		printf("RDMA: fi_send failed: %s (ret=%d, fi_addr=%lu)\n", fi_strerror(-ret), ret, (unsigned long)fi_addr);
		return -1;
	}

	printf("RDMA: fi_send succeeded for %u bytes\n", length);
	return (int)length;
}

int RDMARemoteClient::Recv(char* data, const int dataSize)
{
	if (!isActive || !endpoint || !recvBuffer)
		return -1;

	// Post receive
	int ret = fi_recv(endpoint, recvBuffer->data, RDMABufferPool::BUFFER_SIZE,
	                  fi_mr_desc(recvBuffer->mr), 0, this);
	
	if (ret != 0)
	{
		printf("RDMA: fi_recv failed: %s\n", fi_strerror(-ret));
		return -1;
	}

	return 0;
}

void RDMARemoteClient::SetActive(bool a)
{
	isActiveMutex.Lock();
	isActive = a;
	isActiveMutex.Unlock();
}

void RDMARemoteClient::SendOrBuffer(const char** data, const unsigned int* lengths, const int numParameters)
{
	for (int i = 0; i < numParameters; i++)
	{
		Send(data[i], lengths[i]);
	}
}

// Thread functions would go here (UpdateRDMAInterfaceLoop, RDMAConnectionAttemptLoop)
// Similar to TCPInterface's implementation

RAK_THREAD_DECLARATION(RakNet::UpdateRDMAInterfaceLoop)
{
	RDMAInterface* rdmaInterface = (RDMAInterface*)arguments;
	
	printf("RDMA: Worker thread started\n");
	fflush(stdout);
	
	// Validate critical pointers
	if (!rdmaInterface)
	{
		printf("RDMA: ERROR - rdmaInterface is NULL\n");
		return 0;
	}
	
	if (!rdmaInterface->main_endpoint)
	{
		printf("RDMA: ERROR - main_endpoint is NULL\n");
		return 0;
	}
	
	if (!rdmaInterface->completion_queue)
	{
		printf("RDMA: ERROR - completion_queue is NULL\n");
		return 0;
	}
	
	struct fi_cq_msg_entry cq_entry[32];
	
	// Post initial receives using shared endpoint (AV-based mode like EFA_Test.cpp)
	printf("RDMA: Posting initial receives...\n");
	for (int i = 0; i < 16; i++)
	{
		RDMABufferPool::RDMABuffer* buf = rdmaInterface->bufferPool.AllocateBuffer();
		if (buf)
		{
			void* desc = buf->mr ? fi_mr_desc(buf->mr) : nullptr;
			int ret = fi_recv(rdmaInterface->main_endpoint, buf->data, RDMABufferPool::BUFFER_SIZE, 
			        desc, FI_ADDR_UNSPEC, buf);
			if (ret != 0)
			{
				printf("RDMA: Worker thread: fi_recv %d failed: %s\n", i, fi_strerror(-ret));
			}
		}
		else
		{
			printf("RDMA: Failed to allocate buffer %d\n", i);
		}
	}
	printf("RDMA: Initial receives posted, entering main loop\n");
	fflush(stdout);
	
	while (rdmaInterface->isStarted.GetValue() > 0)
	{
		// Check for incoming connections on control socket (non-blocking accept)
		if (rdmaInterface->control_sockfd != -1)
		{
			struct sockaddr_in client_addr;
			int addr_len = sizeof(client_addr);
			SOCKET client_sock = accept((SOCKET)rdmaInterface->control_sockfd, 
			                            (struct sockaddr*)&client_addr, &addr_len);
			
			if (client_sock != INVALID_SOCKET)
			{
				printf("RDMA: Accepted control connection from %s:%d\n",
				       inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
				fflush(stdout);
				
				// Do the address exchange (server side)
				fi_addr_t client_fi_addr = rdmaInterface->ControlSocketExchangeAddresses((int)client_sock, true);
				
				// Create RemoteClient if address exchange succeeded
				if (client_fi_addr != FI_ADDR_UNSPEC)
				{
					// Create SystemAddress from control socket address
					SystemAddress sysAddr;
					sysAddr.address.addr4.sin_addr.s_addr = client_addr.sin_addr.S_un.S_addr;
					sysAddr.address.addr4.sin_family = AF_INET;
					sysAddr.SetPortNetworkOrder(client_addr.sin_port);
					
					printf("RDMA: Creating RemoteClient for %s with fi_addr=%lu\n", 
					       sysAddr.ToString(), (unsigned long)client_fi_addr);
					fflush(stdout);
					
					// Find a free slot or reuse existing
					bool found = false;
					for (int i = 0; i < rdmaInterface->remoteClientsLength; i++)
					{
						if (rdmaInterface->remoteClients[i].systemAddress == sysAddr)
						{
							found = true;
							rdmaInterface->remoteClients[i].fi_addr = client_fi_addr;
							rdmaInterface->remoteClients[i].SetActive(true);
							printf("RDMA: Updated existing RemoteClient %d\n", i);
							fflush(stdout);
							break;
						}
					}
					
					// Add new client if not found
					if (!found)
					{
						// Find free slot
						for (int i = 0; i < rdmaInterface->remoteClientsLength; i++)
						{
							if (!rdmaInterface->remoteClients[i].isActive)
							{
								rdmaInterface->remoteClients[i].systemAddress = sysAddr;
								rdmaInterface->remoteClients[i].fi_addr = client_fi_addr;
								rdmaInterface->remoteClients[i].endpoint = rdmaInterface->main_endpoint;
								
								// Allocate send and receive buffers
								rdmaInterface->remoteClients[i].sendBuffer = rdmaInterface->bufferPool.AllocateBuffer();
								rdmaInterface->remoteClients[i].recvBuffer = rdmaInterface->bufferPool.AllocateBuffer();
								
								rdmaInterface->remoteClients[i].SetActive(true);
								printf("RDMA: Added new RemoteClient at slot %d with buffers\n", i);
								fflush(stdout);
								break;
							}
						}
					}
				}
				
				// Close control socket after exchange
				closesocket(client_sock);
			}
		}
		
		// Process completions from completion queue (AV-based mode)
		int ret = fi_cq_read(rdmaInterface->completion_queue, cq_entry, 32);			if (ret > 0)
		{
			for (int i = 0; i < ret; i++)
			{
				// Context is the buffer pointer
				RDMABufferPool::RDMABuffer* buf = (RDMABufferPool::RDMABuffer*)cq_entry[i].op_context;
				
				if (!buf)
					continue;
				
				// Check if this is a receive completion
				if (cq_entry[i].flags & FI_RECV)
				{
					printf("RDMA: Received %zu bytes\n", cq_entry[i].len);
					
					// Create packet from received data
					Packet* packet = rdmaInterface->AllocatePacket((unsigned)cq_entry[i].len);
					
					if (packet)
					{
						memcpy(packet->data, buf->data, cq_entry[i].len);
						
						// Find the SystemAddress by looking at active clients
						// In AV mode with MSG CQ, we should check cq_entry[i].addr for source fi_addr
						SystemAddress sourceAddr = UNASSIGNED_SYSTEM_ADDRESS;
						
						// Try to find client by checking active RemoteClients
						for (int j = 0; j < rdmaInterface->remoteClientsLength; j++)
						{
							if (rdmaInterface->remoteClients[j].isActive)
							{
								sourceAddr = rdmaInterface->remoteClients[j].systemAddress;
								printf("RDMA: Packet from client %d (%s)\n", j, sourceAddr.ToString());
								break;
							}
						}
						
						packet->systemAddress = sourceAddr;
						packet->guid = UNASSIGNED_RAKNET_GUID;
						packet->length = (unsigned)cq_entry[i].len;
						packet->bitSize = BYTES_TO_BITS(cq_entry[i].len);
												// Push packet to incoming queue
						printf("RDMA: Pushing packet to receive queue\n");
						rdmaInterface->headPush.Push(packet, _FILE_AND_LINE_);
					}
					
					// Re-post receive with the same buffer
					void* desc = buf->mr ? fi_mr_desc(buf->mr) : nullptr;
					fi_recv(rdmaInterface->main_endpoint, buf->data, RDMABufferPool::BUFFER_SIZE, 
					        desc, FI_ADDR_UNSPEC, buf);
				}
				else if (cq_entry[i].flags & FI_SEND)
				{
					// Send completed - buffer can be reused
					// Note: We're not tracking send buffers yet, just log completion
				}
			}
		}
		else if (ret < 0 && ret != -FI_EAGAIN)
		{
			// Error reading completion queue
			struct fi_cq_err_entry err_entry;
			fi_cq_readerr(rdmaInterface->completion_queue, &err_entry, 0);
			printf("RDMA: CQ error: %s\n", fi_strerror(err_entry.err));
		}
		
		// Small sleep to avoid busy-waiting
		RakSleep(1);
	}
	
	printf("RDMA: Worker thread exiting\n");
	rdmaInterface->threadRunning.Decrement();
	return 0;
}

#endif // _RAKNET_SUPPORT_RDMAInterface==1


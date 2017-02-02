/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

 // Demonstrates ReplicaManager 3: A system to automatically create, destroy, and serialize objects

#include "StringTable.h"
#include "RakPeerInterface.h"

#include <stdio.h>
#include "Kbhit.h"
#include <string.h>
#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "ReplicaManager3.h"
#include "NetworkIDManager.h"
#include "RakSleep.h"
#include "FormatString.h"
#include "RakString.h"
#include "GetTime.h"
#include "SocketLayer.h"
#include "Getche.h"
#include "Rand.h"
#include "VariableDeltaSerializer.h"
#include "Gets.h"

// ReplicaManager3 is in the namespace RakNet
using namespace RakNet;

struct SampleReplica : public Replica3
{
	SampleReplica() { posX = 0; posY = 0; posZ = 0; rotX = 0; rotY = 0; rotZ = 0; rotW = 0; }
	~SampleReplica() {}
	virtual RakNet::RakString GetName(void) const = 0;
	virtual void WriteAllocationID(RakNet::Connection_RM3 *destinationConnection, RakNet::BitStream *allocationIdBitstream) const {
		allocationIdBitstream->Write(GetName());
	}
	void PrintStringInBitstream(RakNet::BitStream *bs)
	{
		if (bs->GetNumberOfBitsUsed() == 0)
			return;
		RakNet::RakString rakString;
		bs->Read(rakString);
		printf("Receive: %s\n", rakString.C_String());
	}

	virtual void SerializeConstruction(RakNet::BitStream *constructionBitstream, RakNet::Connection_RM3 *destinationConnection) {

		// variableDeltaSerializer is a helper class that tracks what variables were sent to what remote system
		// This call adds another remote system to track
		variableDeltaSerializer.AddRemoteSystemVariableHistory(destinationConnection->GetRakNetGUID());

		constructionBitstream->Write(GetName() + RakNet::RakString(" SerializeConstruction"));
	}
	virtual bool DeserializeConstruction(RakNet::BitStream *constructionBitstream, RakNet::Connection_RM3 *sourceConnection) {
		PrintStringInBitstream(constructionBitstream);
		return true;
	}
	virtual void SerializeDestruction(RakNet::BitStream *destructionBitstream, RakNet::Connection_RM3 *destinationConnection) {

		// variableDeltaSerializer is a helper class that tracks what variables were sent to what remote system
		// This call removes a remote system
		variableDeltaSerializer.RemoveRemoteSystemVariableHistory(destinationConnection->GetRakNetGUID());

		destructionBitstream->Write(GetName() + RakNet::RakString(" SerializeDestruction"));

	}
	virtual bool DeserializeDestruction(RakNet::BitStream *destructionBitstream, RakNet::Connection_RM3 *sourceConnection) {
		PrintStringInBitstream(destructionBitstream);
		return true;
	}
	virtual void DeallocReplica(RakNet::Connection_RM3 *sourceConnection) {
		delete this;
	}

	/// Overloaded Replica3 function
	virtual void OnUserReplicaPreSerializeTick(void)
	{
		/// Required by VariableDeltaSerializer::BeginIdenticalSerialize()
		variableDeltaSerializer.OnPreSerializeTick();
	}

	virtual RM3SerializationResult Serialize(SerializeParameters *serializeParameters) {

		VariableDeltaSerializer::SerializationContext serializationContext;

		// Put all variables to be sent unreliably on the same channel, then specify the send type for that channel
		serializeParameters->pro[0].reliability = UNRELIABLE_WITH_ACK_RECEIPT;
		// Sending unreliably with an ack receipt requires the receipt number, and that you inform the system of ID_SND_RECEIPT_ACKED and ID_SND_RECEIPT_LOSS
		serializeParameters->pro[0].sendReceipt = replicaManager->GetRakPeerInterface()->IncrementNextSendReceipt();
		serializeParameters->messageTimestamp = RakNet::GetTime();

		// Begin writing all variables to be sent UNRELIABLE_WITH_ACK_RECEIPT 
		variableDeltaSerializer.BeginUnreliableAckedSerialize(
			&serializationContext,
			serializeParameters->destinationConnection->GetRakNetGUID(),
			&serializeParameters->outputBitstream[0],
			serializeParameters->pro[0].sendReceipt
		);

		variableDeltaSerializer.SerializeVariable(&serializationContext, posX);
		variableDeltaSerializer.SerializeVariable(&serializationContext, posY);
		variableDeltaSerializer.SerializeVariable(&serializationContext, posZ);
		variableDeltaSerializer.SerializeVariable(&serializationContext, rotX);
		variableDeltaSerializer.SerializeVariable(&serializationContext, rotY);
		variableDeltaSerializer.SerializeVariable(&serializationContext, rotZ);
		variableDeltaSerializer.SerializeVariable(&serializationContext, rotW);
		// Tell the system this is the last variable to be written
		variableDeltaSerializer.EndSerialize(&serializationContext);

		// This return type makes is to ReplicaManager3 itself does not do a memory compare. we entirely control serialization ourselves here.
		// Use RM3SR_SERIALIZED_ALWAYS instead of RM3SR_SERIALIZED_ALWAYS_IDENTICALLY to support sending different data to different system, which is needed when using unreliable and dirty variable resends
		return RM3SR_SERIALIZED_ALWAYS;
	}
	virtual void Deserialize(RakNet::DeserializeParameters *deserializeParameters) {}

	virtual void SerializeConstructionRequestAccepted(RakNet::BitStream *serializationBitstream, RakNet::Connection_RM3 *requestingConnection) {
		serializationBitstream->Write(GetName() + RakNet::RakString(" SerializeConstructionRequestAccepted"));
	}
	virtual void DeserializeConstructionRequestAccepted(RakNet::BitStream *serializationBitstream, RakNet::Connection_RM3 *acceptingConnection) {
		PrintStringInBitstream(serializationBitstream);
	}
	virtual void SerializeConstructionRequestRejected(RakNet::BitStream *serializationBitstream, RakNet::Connection_RM3 *requestingConnection) {
		serializationBitstream->Write(GetName() + RakNet::RakString(" SerializeConstructionRequestRejected"));
	}
	virtual void DeserializeConstructionRequestRejected(RakNet::BitStream *serializationBitstream, RakNet::Connection_RM3 *rejectingConnection) {
		PrintStringInBitstream(serializationBitstream);
	}

	virtual void OnPoppedConnection(RakNet::Connection_RM3 *droppedConnection)
	{
		// Same as in SerializeDestruction(), no longer track this system
		variableDeltaSerializer.RemoveRemoteSystemVariableHistory(droppedConnection->GetRakNetGUID());
	}
	void NotifyReplicaOfMessageDeliveryStatus(RakNetGUID guid, uint32_t receiptId, bool messageArrived)
	{
		// When using UNRELIABLE_WITH_ACK_RECEIPT, the system tracks which variables were updated with which sends
		// So it is then necessary to inform the system of messages arriving or lost
		// Lost messages will flag each variable sent in that update as dirty, meaning the next Serialize() call will resend them with the current values
		variableDeltaSerializer.OnMessageReceipt(guid, receiptId, messageArrived);
	}
	void RandomizeVariables(void)
	{
		if (randomMT() % 2) { posX = RandomFloat(-100.0f, 100.0f); }
		if (randomMT() % 2) { posY = RandomFloat(-100.0f, 100.0f); }
		if (randomMT() % 2) { posZ = RandomFloat(-100.0f, 100.0f); }
		if (randomMT() % 2) { rotX = RandomFloat(-10.0f, 10.0f); }
		if (randomMT() % 2) { rotY = RandomFloat(-10.0f, 10.0f); }
		if (randomMT() % 2) { rotZ = RandomFloat(-10.0f, 10.0f); }
		if (randomMT() % 2) { rotW = RandomFloat(-10.0f, 10.0f); }

		normaliseRotation();
	}

	float RandomFloat(float min, float max)
	{
		return min + static_cast <float> (rand()) / (static_cast <float> (RAND_MAX / (max - min)));
	}

	void normaliseRotation() {
		double n = sqrt(rotX*rotX + rotY*rotY + rotZ*rotZ + rotW*rotW);
		rotX /= n;
		rotY /= n;
		rotZ /= n;
		rotW /= n;
	}

	// Demonstrate per-variable synchronization
	// We manually test each variable to the last synchronized value and only send those values that change
	float posX, posY, posZ;
	float rotX, rotY, rotZ, rotW;

	// Class to save and compare the states of variables this Serialize() to the last Serialize()
	// If the value is different, true is written to the bitStream, followed by the value. Otherwise false is written.
	// It also tracks which variables changed which Serialize() call, so if an unreliable message was lost (ID_SND_RECEIPT_LOSS) those variables are flagged 'dirty' and resent
	VariableDeltaSerializer variableDeltaSerializer;
};

struct ServerCreated_ServerSerialized : public SampleReplica {
	virtual RakNet::RakString GetName(void) const { return RakNet::RakString("ServerCreated_ServerSerialized"); }
	virtual RM3SerializationResult Serialize(SerializeParameters *serializeParameters)
	{
		return SampleReplica::Serialize(serializeParameters);
	}
	virtual RM3ConstructionState QueryConstruction(RakNet::Connection_RM3 *destinationConnection, ReplicaManager3 *replicaManager3) {
		return QueryConstruction_ServerConstruction(destinationConnection, true);
	}
	virtual bool QueryRemoteConstruction(RakNet::Connection_RM3 *sourceConnection) {
		return QueryRemoteConstruction_ServerConstruction(sourceConnection, true);
	}
	virtual RM3QuerySerializationResult QuerySerialization(RakNet::Connection_RM3 *destinationConnection) {
		return QuerySerialization_ServerSerializable(destinationConnection, true);
	}
	virtual RM3ActionOnPopConnection QueryActionOnPopConnection(RakNet::Connection_RM3 *droppedConnection) const {
		return QueryActionOnPopConnection_Server(droppedConnection);
	}
};

class SampleConnection : public Connection_RM3 {
public:
	SampleConnection(const SystemAddress &_systemAddress, RakNetGUID _guid) : Connection_RM3(_systemAddress, _guid) {}
	virtual ~SampleConnection() {}

	// See documentation - Makes all messages between ID_REPLICA_MANAGER_DOWNLOAD_STARTED and ID_REPLICA_MANAGER_DOWNLOAD_COMPLETE arrive in one tick
	bool QueryGroupDownloadMessages(void) const { return true; }

	virtual Replica3 *AllocReplica(RakNet::BitStream *allocationId, ReplicaManager3 *replicaManager3)
	{
		RakNet::RakString typeName;
		allocationId->Read(typeName);
		if (typeName == "ServerCreated_ServerSerialized") return new ServerCreated_ServerSerialized;
		return 0;
	}
protected:
};

class ReplicaManager3Sample : public ReplicaManager3
{
	virtual Connection_RM3* AllocConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID) const {
		return new SampleConnection(systemAddress, rakNetGUID);
	}
	virtual void DeallocConnection(Connection_RM3 *connection) const {
		delete connection;
	}
};

int main(void)
{
	char ch;
	RakNet::SocketDescriptor sd;
	sd.socketFamily = AF_INET; // Only IPV4 supports broadcast on 255.255.255.255
	char ip[128];
	static const int SERVER_PORT = 12345;

	// ReplicaManager3 requires NetworkIDManager to lookup pointers from numbers.
	NetworkIDManager networkIdManager;
	// Each application has one instance of RakPeerInterface
	RakNet::RakPeerInterface *rakPeer;
	// The system that performs most of our functionality for this demo
	ReplicaManager3Sample replicaManager;

	printf("Demonstration of ReplicaManager3.\n");
	printf("1. Demonstrates creating objects created by the server and client.\n");
	printf("2. Demonstrates automatic serialization data members\n");
	printf("Difficulty: Intermediate\n\n");

	rakPeer = RakNet::RakPeerInterface::GetInstance();
	sd.port = SERVER_PORT;

	// Start RakNet, up to 32 connections if the server
	rakPeer->Startup(32, &sd, 1);
	rakPeer->AttachPlugin(&replicaManager);
	replicaManager.SetNetworkIDManager(&networkIdManager);
	rakPeer->SetMaximumIncomingConnections(32);

	printf("\nMy GUID is %s\n", rakPeer->GetMyGUID().ToString());
	printf("Commands:\n(Q)uit\n'C'reate objects\n'R'andomly change variables in my objects\n'D'estroy my objects\n");

	// Enter infinite loop to run the system
	RakNet::Packet *packet;
	bool quit = false;
	while (!quit)
	{
		for (packet = rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), packet = rakPeer->Receive())
		{
			switch (packet->data[0])
			{
			case ID_CONNECTION_ATTEMPT_FAILED:
				printf("ID_CONNECTION_ATTEMPT_FAILED\n");
				quit = true;
				break;
			case ID_NO_FREE_INCOMING_CONNECTIONS:
				printf("ID_NO_FREE_INCOMING_CONNECTIONS\n");
				quit = true;
				break;
			case ID_CONNECTION_REQUEST_ACCEPTED:
				printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
				break;
			case ID_NEW_INCOMING_CONNECTION:
				printf("ID_NEW_INCOMING_CONNECTION from %s\n", packet->systemAddress.ToString());
				break;
			case ID_DISCONNECTION_NOTIFICATION:
				printf("ID_DISCONNECTION_NOTIFICATION\n");
				break;
			case ID_CONNECTION_LOST:
				printf("ID_CONNECTION_LOST\n");
				break;
			case ID_ADVERTISE_SYSTEM:
				// The first conditional is needed because ID_ADVERTISE_SYSTEM may be from a system we are connected to, but replying on a different address.
				// The second conditional is because AdvertiseSystem also sends to the loopback
				if (rakPeer->GetSystemAddressFromGuid(packet->guid) == RakNet::UNASSIGNED_SYSTEM_ADDRESS &&
					rakPeer->GetMyGUID() != packet->guid)
				{
					printf("Connecting to %s\n", packet->systemAddress.ToString(true));
					rakPeer->Connect(packet->systemAddress.ToString(false), packet->systemAddress.GetPort(), 0, 0);
				}
				break;
			case ID_SND_RECEIPT_LOSS:
			case ID_SND_RECEIPT_ACKED:
			{
				uint32_t msgNumber;
				memcpy(&msgNumber, packet->data + 1, 4);

				DataStructures::List<Replica3*> replicaListOut;
				replicaManager.GetReplicasCreatedByMe(replicaListOut);
				unsigned int idx;
				for (idx = 0; idx < replicaListOut.Size(); idx++)
				{
					((SampleReplica*)replicaListOut[idx])->NotifyReplicaOfMessageDeliveryStatus(packet->guid, msgNumber, packet->data[0] == ID_SND_RECEIPT_ACKED);
				}
			}
			break;
			}
		}

		if (rakPeer->NumberOfConnections() > 0)
		{
			DataStructures::List<Replica3*> replicaListOut;
			replicaManager.GetReplicasCreatedByMe(replicaListOut);
			unsigned int idx;
			for (idx = 0; idx < replicaListOut.Size(); idx++)
			{
				((SampleReplica*)replicaListOut[idx])->RandomizeVariables();
			}
		}

		if (kbhit())
		{
			ch = getch();
			if (ch == 'q' || ch == 'Q')
			{
				printf("Quitting.\n");
				quit = true;
			}
			if (ch == 'c' || ch == 'C')
			{
				printf("Objects created.\n");
				replicaManager.Reference(new ServerCreated_ServerSerialized);
			}
			if (ch == 'r' || ch == 'R')
			{
				DataStructures::List<Replica3*> replicaListOut;
				replicaManager.GetReplicasCreatedByMe(replicaListOut);
				unsigned int idx;
				for (idx = 0; idx < replicaListOut.Size(); idx++)
				{
					((SampleReplica*)replicaListOut[idx])->RandomizeVariables();
				}
			}
			if (ch == 'd' || ch == 'D')
			{
				printf("My objects destroyed.\n");
				DataStructures::List<Replica3*> replicaListOut;
				// The reason for ClearPointers is that in the sample, I don't track which objects have and have not been allocated at the application level. So ClearPointers will call delete on every object in the returned list, which is every object that the application has created. Another way to put it is
				// 	A. Send a packet to tell other systems to delete these objects
				// 	B. Delete these objects on my own system
				replicaManager.GetReplicasCreatedByMe(replicaListOut);
				replicaManager.BroadcastDestructionList(replicaListOut, RakNet::UNASSIGNED_SYSTEM_ADDRESS);
				for (unsigned int i = 0; i < replicaListOut.Size(); i++)
					RakNet::OP_DELETE(replicaListOut[i], _FILE_AND_LINE_);
			}

		}

		RakSleep(30);
		for (int i = 0; i < 4; i++)
		{
			if (rakPeer->GetInternalID(RakNet::UNASSIGNED_SYSTEM_ADDRESS, 0).GetPort() != SERVER_PORT + i)
				rakPeer->AdvertiseSystem("255.255.255.255", SERVER_PORT + i, 0, 0, 0);
		}
	}

	rakPeer->Shutdown(100, 0);
	RakNet::RakPeerInterface::DestroyInstance(rakPeer);
}

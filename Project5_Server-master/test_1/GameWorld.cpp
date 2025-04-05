#include "GameWorld.h"
#include <iostream>
#include <print>
#include <thread>

GameWorld::GameWorld() : listenSock(INVALID_SOCKET), iocp(NULL), running(false)
{
	InitializeCriticalSection(&playersCriticalSection);
}

GameWorld::~GameWorld() { stop(); }

void GameWorld::start()
{
	WSAData wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData)) {
		std::cerr << "WSAStartup failed!\n";
		exit(-1);
	}

	listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET) {
		std::cerr << "Server socket creation failed!\n";
		exit(-1);
	}

	sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	serverAddr.sin_port = htons(PORT);

	if (bind(listenSock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
		std::cerr << "Bind failed!\n";
		exit(-1);
	}

	if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
		std::cerr << "Listen failed!\n";
		exit(-1);
	}

	iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (iocp == NULL) {
		std::cerr << "CreateIoCompletionPort failed!\n";
		exit(-1);
	}

	running = true;
	std::vector<std::thread> workers;

	for (int i = 0; i < 4; ++i) // �񵿱� ����
		workers.emplace_back(&GameWorld::workerThread, this);
	for (auto& worker : workers) worker.detach();
	                            // ���� �۽� (�ֱ���)
	std::thread sendThread(&GameWorld::sendWorldData, this);
	sendThread.detach();

	acceptConnections();
}

void GameWorld::acceptConnections()
{
	sockaddr_in caddr;
	SOCKET csock;
	int addrlen = sizeof(caddr);

	while (running)
	{
		csock = accept(listenSock, (SOCKADDR*)&caddr, &addrlen);
		if (csock == INVALID_SOCKET)
		{
			std::cerr << "accept failed!\n";
			continue;
		}

		// Ŭ���̾�Ʈ ���� ����
		PlayerData* newPlayer = new PlayerData("UninitPlayer", csock);

		// IOCP�� Ŭ���̾�Ʈ ���� �߰�
		if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(csock), iocp,
			reinterpret_cast<ULONG_PTR>(newPlayer), 0) == NULL)
		{
			std::cerr << "CreateIoCompletionPort failed!\n";
			closesocket(csock);
			delete newPlayer;
			continue;
		}

		// Ŭ���̾�Ʈ ������ GameWorld�� �߰�
		addPlayer(csock, newPlayer);

		// ������ ������ ����
		newPlayer->session.PostRecv();
	}
}

void GameWorld::workerThread()
{
	DWORD bytesTransferred;
	ULONG_PTR completionKey;
	LPOVERLAPPED lpOverlapped;

	while (running)
	{
		BOOL result = GetQueuedCompletionStatus
		(iocp, &bytesTransferred, &completionKey, &lpOverlapped, INFINITE);

		PlayerData* player = reinterpret_cast<PlayerData*>(completionKey);

		if (!result)              // ��������
		{
			std::cerr << "GetQueuedCompletionStatus failed! Error: " << GetLastError() << '\n';
			std::print("player {} is disconnected!\n", player->name);
			closesocket(player->session.getClientSocket());
			removePlayer(player->session.getClientSocket());
			continue;
		}
		if (bytesTransferred == 0) // ��������
		{
			std::print("player {} is disconnected!\n", player->name);
			closesocket(player->session.getClientSocket());
			removePlayer(player->session.getClientSocket());
			continue;
		}
		player->PlayerCommitWrite(bytesTransferred);

		// Ŭ���̾�Ʈ���� �����͸� ���� �� ó��
		player->PlayerPostRecv();

		// ��Ŷ�� �����Ͽ� ó��
		Packet packet;
	
		while (player->PlayerExtractPacket(packet))
		{
			// ��Ŷ Ÿ���� �а� ó��
			PacketType dataType = packet.header.type;

			if     (dataType == PacketType::PlayerInit)   processPlayerInit(player, packet);
			else if(dataType == PacketType::PlayerUpdate) processPlayerUpdate(player, packet);
			else if(dataType == PacketType::MonsterUpdate)processMonsterUpdate(packet);
			else    std::cerr << "Invalid packet type\n";
		}
	}
}


void GameWorld::processPlayerInit(PlayerData* player, Packet& packet)
{
	player->processInit(packet);
}

void GameWorld::processPlayerUpdate(PlayerData* player, Packet& packet)
{
	player->processUpdate(packet);
}

void GameWorld::processMonsterUpdate(Packet& packet)
{
	float monsterX = packet.read<float>();
	float monsterY = packet.read<float>();

	std::print("Monster updated position to ({}, {})\n", monsterX, monsterY);
}


void GameWorld::sendWorldData() // ���� ������
{
	while (running)
	{
		if (players.empty()) continue;

		// ���� ������ ����ȭ
		Packet worldPacket;
		worldPacket.header.type = PacketType::WorldUpdate;
		worldPacket.header.playerCount = players.size();

		lockPlayers();
		for (auto& pair : players)
		{
			PlayerData* player = pair.second;
			worldPacket.writeString(player->getName());   // �÷��̾� �̸�
			worldPacket.write<float>(player->getPosX());  // X ��ǥ
			worldPacket.write<float>(player->getPosY());  // Y ��ǥ
			worldPacket.write<uint8_t>(player->getAnimTypeAsByte());
		}
		unlockPlayers();
		std::vector<uint8_t> serializedPacket = worldPacket.Serialize();  // ��Ŷ ����ȭ
		const char* sendBuffer = reinterpret_cast<const char*>(serializedPacket.data());
		// ����ȭ�� ���� �����͸� ��� �÷��̾�� ��ε�ĳ��Ʈ
		for (auto& pair : players)
		{
			PlayerData* player = pair.second;
			int bytesSent = send(player->getClientSession().getClientSocket(), sendBuffer
				, static_cast<int>(serializedPacket.size()), 0);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));  // ������ �ӵ� ����(�������� Ŭ���̾�Ʈ���)
	}
}


void GameWorld::addPlayer(SOCKET socket, PlayerData* player)
{
	lockPlayers();
	players[socket] = player;
	unlockPlayers();
}

void GameWorld::removePlayer(SOCKET socket)
{
	lockPlayers();
	auto it = players.find(socket);
	if (it != players.end())
	{
		delete it->second;
		players.erase(it);
	}
	unlockPlayers();
}

PlayerData* GameWorld::getPlayer(SOCKET socket)
{
	lockPlayers();
	auto it = players.find(socket);
	PlayerData* player = nullptr;
	if (it != players.end())
	{
		player = it->second;
	}
	unlockPlayers();
	return player;
}

void GameWorld::lockPlayers()
{
	EnterCriticalSection(&playersCriticalSection);
}

void GameWorld::unlockPlayers()
{
	LeaveCriticalSection(&playersCriticalSection);
}

void GameWorld::stop()
{
	running = false;
	closesocket(listenSock);
	for (auto& pair : players)
	{
		closesocket(pair.first);
		delete pair.second;
	}
	players.clear();
	WSACleanup();
}
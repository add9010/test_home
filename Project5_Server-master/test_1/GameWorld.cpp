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

	for (int i = 0; i < 4; ++i) // 비동기 수신
		workers.emplace_back(&GameWorld::workerThread, this);
	for (auto& worker : workers) worker.detach();
	                            // 동기 송신 (주기적)
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

		// 클라이언트 세션 생성
		PlayerData* newPlayer = new PlayerData("UninitPlayer", csock);

		// IOCP에 클라이언트 소켓 추가
		if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(csock), iocp,
			reinterpret_cast<ULONG_PTR>(newPlayer), 0) == NULL)
		{
			std::cerr << "CreateIoCompletionPort failed!\n";
			closesocket(csock);
			delete newPlayer;
			continue;
		}

		// 클라이언트 세션을 GameWorld에 추가
		addPlayer(csock, newPlayer);

		// 데이터 수신을 시작
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

		if (!result)              // 강제종료
		{
			std::cerr << "GetQueuedCompletionStatus failed! Error: " << GetLastError() << '\n';
			std::print("player {} is disconnected!\n", player->name);
			closesocket(player->session.getClientSocket());
			removePlayer(player->session.getClientSocket());
			continue;
		}
		if (bytesTransferred == 0) // 정상종료
		{
			std::print("player {} is disconnected!\n", player->name);
			closesocket(player->session.getClientSocket());
			removePlayer(player->session.getClientSocket());
			continue;
		}
		player->PlayerCommitWrite(bytesTransferred);

		// 클라이언트에서 데이터를 받은 후 처리
		player->PlayerPostRecv();

		// 패킷을 추출하여 처리
		Packet packet;
	
		while (player->PlayerExtractPacket(packet))
		{
			// 패킷 타입을 읽고 처리
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


void GameWorld::sendWorldData() // 보낼 데이터
{
	while (running)
	{
		if (players.empty()) continue;

		// 월드 데이터 직렬화
		Packet worldPacket;
		worldPacket.header.type = PacketType::WorldUpdate;
		worldPacket.header.playerCount = players.size();

		lockPlayers();
		for (auto& pair : players)
		{
			PlayerData* player = pair.second;
			worldPacket.writeString(player->getName());   // 플레이어 이름
			worldPacket.write<float>(player->getPosX());  // X 좌표
			worldPacket.write<float>(player->getPosY());  // Y 좌표
			worldPacket.write<uint8_t>(player->getAnimTypeAsByte());
		}
		unlockPlayers();
		std::vector<uint8_t> serializedPacket = worldPacket.Serialize();  // 패킷 직렬화
		const char* sendBuffer = reinterpret_cast<const char*>(serializedPacket.data());
		// 직렬화된 월드 데이터를 모든 플레이어에게 브로드캐스트
		for (auto& pair : players)
		{
			PlayerData* player = pair.second;
			int bytesSent = send(player->getClientSession().getClientSocket(), sendBuffer
				, static_cast<int>(serializedPacket.size()), 0);
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 보내는 속도 조절(서버에서 클라이언트들로)
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
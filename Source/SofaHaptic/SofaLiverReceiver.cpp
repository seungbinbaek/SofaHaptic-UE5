// SofaLiverReceiver.cpp

#include "SofaLiverReceiver.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/PlatformProcess.h"

// ─────────────────────────────────────────────────────────────
// FSofaReceiverRunnable
// ─────────────────────────────────────────────────────────────

FSofaReceiverRunnable::FSofaReceiverRunnable(ASofaLiverReceiver* InOwner)
	: Owner(InOwner)
{
}

bool FSofaReceiverRunnable::RecvAll(FSocket* Sock, uint8* Buf, int32 Len)
{
	int32 Received = 0;
	while (Received < Len && !bStop)
	{
		int32 n = 0;
		if (!Sock->Recv(Buf + Received, Len - Received, n))
			return false;
		if (n == 0)
			return false;
		Received += n;
	}
	return (Received == Len);
}

uint32 FSofaReceiverRunnable::Run()
{
	ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

	// ── SOFA 연결 대기 (Accept) ───────────────────────────
	FSocket* Listen = Owner->ListenSocket;
	while (!bStop && Listen)
	{
		bool bPending = false;
		if (Listen->HasPendingConnection(bPending) && bPending)
		{
			Owner->ClientSocket = Listen->Accept(TEXT("SOFA_Client"));
			if (Owner->ClientSocket)
			{
				UE_LOG(LogTemp, Log, TEXT("[SofaReceiver] SOFA 연결됨"));
				break;
			}
		}
		FPlatformProcess::Sleep(0.1f);
	}

	FSocket* Client = Owner->ClientSocket;
	if (!Client) return 0;

	// ── 패킷 수신 루프 ────────────────────────────────────
	while (!bStop)
	{
		// 헤더 읽기: [type:1][frameId:4][numVerts:4]
		uint8  PktType  = 0;
		uint32 FrameId  = 0;
		uint32 NumVerts = 0;

		if (!RecvAll(Client, &PktType,          1)) break;
		if (!RecvAll(Client, (uint8*)&FrameId,  4)) break;
		if (!RecvAll(Client, (uint8*)&NumVerts, 4)) break;

		// ── INIT 패킷: 위상 데이터 포함 ──────────────────
		if (PktType == PKT_INIT)
		{
			uint32 NumTris = 0;
			if (!RecvAll(Client, (uint8*)&NumTris, 4)) break;

			TArray<float>  RawVerts; RawVerts.SetNumUninitialized(NumVerts * 3);
			TArray<uint32> RawTris;  RawTris.SetNumUninitialized(NumTris * 3);

			if (!RecvAll(Client, (uint8*)RawVerts.GetData(), NumVerts * 12)) break;
			if (!RecvAll(Client, (uint8*)RawTris.GetData(),  NumTris  * 12)) break;

			// float → FVector 변환 (scale 적용)
			TArray<FVector> Verts;
			Verts.SetNumUninitialized(NumVerts);
			float Scale = Owner->PositionScale;
			for (uint32 i = 0; i < NumVerts; ++i)
			{
				Verts[i] = FVector(
					RawVerts[i * 3 + 0] * Scale,
					RawVerts[i * 3 + 1] * Scale,
					RawVerts[i * 3 + 2] * Scale);
			}

			TArray<int32> Tris;
			Tris.SetNumUninitialized(NumTris * 3);
			for (uint32 i = 0; i < NumTris * 3; ++i)
				Tris[i] = (int32)RawTris[i];

			Owner->PushTopology(MoveTemp(Verts), MoveTemp(Tris));
		}
		// ── UPDATE 패킷: vertex만 ─────────────────────────
		else if (PktType == PKT_UPDATE)
		{
			TArray<float> RawVerts; RawVerts.SetNumUninitialized(NumVerts * 3);
			if (!RecvAll(Client, (uint8*)RawVerts.GetData(), NumVerts * 12)) break;

			TArray<FVector> Verts;
			Verts.SetNumUninitialized(NumVerts);
			float Scale = Owner->PositionScale;
			for (uint32 i = 0; i < NumVerts; ++i)
			{
				Verts[i] = FVector(
					RawVerts[i * 3 + 0] * Scale,
					RawVerts[i * 3 + 1] * Scale,
					RawVerts[i * 3 + 2] * Scale);
			}

			Owner->PushVertices(MoveTemp(Verts));
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[SofaReceiver] 수신 스레드 종료"));
	return 0;
}

// ─────────────────────────────────────────────────────────────
// ASofaLiverReceiver
// ─────────────────────────────────────────────────────────────

ASofaLiverReceiver::ASofaLiverReceiver()
{
	PrimaryActorTick.bCanEverTick = true;

	LiverMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("LiverMesh"));
	RootComponent = LiverMesh;
}

void ASofaLiverReceiver::BeginPlay()
{
	Super::BeginPlay();

	ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

	// TCP Listen 소켓 생성
	ListenSocket = SocketSub->CreateSocket(NAME_Stream, TEXT("SOFA_Listen"), false);

	TSharedRef<FInternetAddr> Addr = SocketSub->CreateInternetAddr();
	Addr->SetAnyAddress();
	Addr->SetPort(ListenPort);

	if (!ListenSocket->Bind(*Addr))
	{
		UE_LOG(LogTemp, Error, TEXT("[SofaReceiver] 포트 %d 바인딩 실패"), ListenPort);
		return;
	}
	ListenSocket->Listen(1);
	UE_LOG(LogTemp, Log, TEXT("[SofaReceiver] 포트 %d 에서 SOFA 연결 대기 중..."), ListenPort);

	// 수신 스레드 시작
	RecvRunnable = new FSofaReceiverRunnable(this);
	RecvThread   = FRunnableThread::Create(RecvRunnable, TEXT("SofaReceiverThread"));
}

void ASofaLiverReceiver::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);

	if (RecvRunnable) RecvRunnable->Stop();

	// 소켓 닫기 → 블로킹 Accept/Recv 해제
	ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (ClientSocket) { ClientSocket->Close(); SocketSub->DestroySocket(ClientSocket); ClientSocket = nullptr; }
	if (ListenSocket) { ListenSocket->Close(); SocketSub->DestroySocket(ListenSocket); ListenSocket = nullptr; }

	if (RecvThread)   { RecvThread->WaitForCompletion(); delete RecvThread;   RecvThread   = nullptr; }
	if (RecvRunnable) {                                  delete RecvRunnable; RecvRunnable = nullptr; }
}

void ASofaLiverReceiver::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	FScopeLock Lock(&DataLock);

	if (bNewTopology)
	{
		bNewTopology = false;
		bHasTopo     = true;

		TArray<FVector> Normals;
		ComputeNormals(PendingVerts, CachedTris, Normals);

		TArray<FVector2D>      UVs;
		TArray<FColor>         Colors;
		TArray<FProcMeshTangent> Tangents;

		LiverMesh->CreateMeshSection(0, PendingVerts, CachedTris,
		                             Normals, UVs, Colors, Tangents, false);
		UE_LOG(LogTemp, Log, TEXT("[SofaReceiver] 메쉬 생성: %d verts, %d tris"),
		       PendingVerts.Num(), CachedTris.Num() / 3);
	}
	else if (bHasTopo && bNewVerts)
	{
		bNewVerts = false;

		TArray<FVector> Normals;
		ComputeNormals(PendingVerts, CachedTris, Normals);

		TArray<FVector2D>      UVs;
		TArray<FColor>         Colors;
		TArray<FProcMeshTangent> Tangents;

		LiverMesh->UpdateMeshSection(0, PendingVerts, Normals, UVs, Colors, Tangents);
	}
}

void ASofaLiverReceiver::PushTopology(TArray<FVector>&& Verts, TArray<int32>&& Tris)
{
	FScopeLock Lock(&DataLock);
	PendingVerts = MoveTemp(Verts);
	CachedTris   = MoveTemp(Tris);
	bNewTopology = true;
	bNewVerts    = false;
}

void ASofaLiverReceiver::PushVertices(TArray<FVector>&& Verts)
{
	FScopeLock Lock(&DataLock);
	PendingVerts = MoveTemp(Verts);
	bNewVerts    = true;
}

void ASofaLiverReceiver::ComputeNormals(const TArray<FVector>& Verts,
                                         const TArray<int32>&   Tris,
                                         TArray<FVector>&        OutNormals)
{
	OutNormals.SetNumZeroed(Verts.Num());

	for (int32 i = 0; i + 2 < Tris.Num(); i += 3)
	{
		int32 i0 = Tris[i], i1 = Tris[i + 1], i2 = Tris[i + 2];
		if (i0 >= Verts.Num() || i1 >= Verts.Num() || i2 >= Verts.Num()) continue;

		FVector FaceN = FVector::CrossProduct(
			Verts[i1] - Verts[i0],
			Verts[i2] - Verts[i0]);

		OutNormals[i0] += FaceN;
		OutNormals[i1] += FaceN;
		OutNormals[i2] += FaceN;
	}

	for (FVector& N : OutNormals) N = N.GetSafeNormal();
}

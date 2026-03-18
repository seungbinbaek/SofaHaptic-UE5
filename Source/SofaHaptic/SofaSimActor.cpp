// SofaSimActor.cpp

#include "SofaSimActor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

THIRD_PARTY_INCLUDES_START
#include <string>
#include <vector>
#include "SofaPhysicsAPI.h"
THIRD_PARTY_INCLUDES_END

ASofaSimActor::ASofaSimActor()
{
	PrimaryActorTick.bCanEverTick = true;

	LiverMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("LiverMesh"));
	RootComponent = LiverMesh;
}

void ASofaSimActor::BeginPlay()
{
	Super::BeginPlay();
	InitSOFA();
}

void ASofaSimActor::InitSOFA()
{
	if (SceneFilePath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("SofaSimActor: SceneFilePath is empty"));
		return;
	}

	// ── sofa::simulation::graph::init() 명시 호출 ──────────────────────────
	{
		void* GraphDll = FPlatformProcess::GetDllHandle(TEXT("Sofa.Simulation.Graph.dll"));
		if (GraphDll)
		{
			typedef void(*FGraphInit)();
			FGraphInit GraphInit = (FGraphInit)FPlatformProcess::GetDllExport(
				GraphDll, TEXT("?init@graph@simulation@sofa@@YAXXZ"));
			if (GraphInit)
			{
				GraphInit();
				UE_LOG(LogTemp, Log, TEXT("SofaSimActor: sofa::simulation::graph::init() called"));
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("SofaSimActor: graph::init symbol not found — may crash"));
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("SofaSimActor: Sofa.Simulation.Graph.dll not loaded"));
		}
	}

	SofaAPI = new SofaPhysicsAPI(/*useGUI=*/false, 0);
	if (!SofaAPI)
	{
		UE_LOG(LogTemp, Error, TEXT("SofaSimActor: Failed to allocate SofaPhysicsAPI"));
		return;
	}

	SofaAPI->setTimeStep(SimDT);

	// SOFA DataRepository에 share 경로 등록
	const char* sharePath = SofaAPI->loadSofaIni("D:/sofa/build/etc/sofa.ini");
	UE_LOG(LogTemp, Log, TEXT("SofaSimActor: SOFA share path = %hs"), sharePath ? sharePath : "(null)");

	// ── Binaries/Win64 플러그인 선로드 ──────────────────────────────────────
	{
		FString BinDir = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries/Win64")));
		FPlatformProcess::AddDllDirectory(*BinDir);
		UE_LOG(LogTemp, Log, TEXT("SofaSimActor: Added DLL dir: %s"), *BinDir);

		TArray<FString> FoundFiles;
		IFileManager::Get().FindFiles(FoundFiles, *(BinDir / TEXT("Sofa.*.dll")), true, false);
		int loadedCount = 0;
		for (const FString& FileName : FoundFiles)
		{
			std::string dllPath = TCHAR_TO_UTF8(*(BinDir / FileName));
			int r = SofaAPI->loadPlugin(dllPath.c_str());
			if (r == 0) loadedCount++;
		}
		UE_LOG(LogTemp, Log, TEXT("SofaSimActor: Preloaded %d / %d Sofa plugins"), loadedCount, FoundFiles.Num());

		std::string fdPath = TCHAR_TO_UTF8(*(BinDir / TEXT("ForceDimensions.dll")));
		int pluginResult = SofaAPI->loadPlugin(fdPath.c_str());
		UE_LOG(LogTemp, Log, TEXT("SofaSimActor: ForceDimensions loadPlugin = %d"), pluginResult);
	}

	SofaAPI->activateMessageHandler(true);
	SofaAPI->clearMessages();

	std::string path = TCHAR_TO_UTF8(*SceneFilePath);
	UE_LOG(LogTemp, Log, TEXT("SofaSimActor: Loading scene: %s"), *SceneFilePath);
	int result = SofaAPI->load(path.c_str());
	UE_LOG(LogTemp, Log, TEXT("SofaSimActor: load() returned %d"), result);

	int nbMsg = SofaAPI->getNbMessages();
	for (int i = 0; i < nbMsg; i++)
	{
		int msgType = 0;
		const char* msg = SofaAPI->getMessage(i, msgType);
		UE_LOG(LogTemp, Log, TEXT("SOFA [type=%d]: %hs"), msgType, msg ? msg : "(null)");
	}
	SofaAPI->clearMessages();

	if (result < 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SofaSimActor: Failed to load scene (err=%d): %s"), result, *SceneFilePath);
		delete SofaAPI;
		SofaAPI = nullptr;
		return;
	}
	if (result > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SofaSimActor: Scene loaded with %d warning(s)"), result);
	}

	int nbMeshes = (int)SofaAPI->getNbOutputMeshes();
	UE_LOG(LogTemp, Log, TEXT("SofaSimActor: Scene loaded — %d output mesh(es)"), nbMeshes);

	// ── step() 전 초기 rest position에서 토폴로지+메쉬 생성 ─────────────────
	// SofaUE5-Renderer 방식: 시뮬레이션 시작 전에 메쉬를 만들면 NaN 없음
	BuildInitialMesh();

	bInitialized = true;
	StartSimulation();
}

void ASofaSimActor::BuildInitialMesh()
{
	SofaPhysicsOutputMesh* mesh = SofaAPI->getOutputMeshPtr((unsigned int)MeshIndex);
	if (!mesh)
	{
		UE_LOG(LogTemp, Error, TEXT("SofaSimActor: BuildInitialMesh — getOutputMeshPtr(%d) null"), MeshIndex);
		return;
	}

	unsigned int nbVerts = mesh->getNbVertices();
	if (nbVerts == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SofaSimActor: BuildInitialMesh — no vertices"));
		return;
	}

	// SOFA는 double 정밀도로 빌드됨 → getVPositions() 포인터 직접 사용 시 double→float 오독 발생
	// getVPositions(buffer) 오버로드를 사용해야 올바른 float 변환이 이루어짐
	std::vector<float> posBuffer(nbVerts * 3);
	if (mesh->getVPositions(posBuffer.data()) < 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SofaSimActor: BuildInitialMesh — getVPositions failed"));
		return;
	}
	const float* rawPos = posBuffer.data();

	UE_LOG(LogTemp, Log, TEXT("SofaSimActor: BuildInitialMesh — nbVerts=%u, v[0]=(%.4f,%.4f,%.4f)"),
		nbVerts, rawPos[0], rawPos[1], rawPos[2]);

	unsigned int nbTris  = mesh->getNbTriangles();
	const unsigned int* tris = mesh->getTriangles();
	if (!tris || nbTris == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SofaSimActor: BuildInitialMesh — no triangles (nbTris=%u)"), nbTris);
		return;
	}

	// 버텍스 변환 (SOFA Y-up → UE Z-up, cm 단위 변환)
	TArray<FVector> Verts;
	Verts.Reserve(nbVerts);
	for (unsigned int i = 0; i < nbVerts; i++)
	{
		float sx = rawPos[i * 3 + 0];
		float sy = rawPos[i * 3 + 1];
		float sz = rawPos[i * 3 + 2];
		Verts.Add(FVector(sx * PositionScale, -sz * PositionScale, sy * PositionScale));
	}

	// 삼각형 인덱스 캐시
	CachedTriangles.Reset(nbTris * 3);
	for (unsigned int i = 0; i < nbTris * 3; i++)
		CachedTriangles.Add((int32)tris[i]);

	// 노멀 (버퍼 버전 사용)
	std::vector<float> normBuffer(nbVerts * 3);
	TArray<FVector> Normals;
	if (mesh->getVNormals(normBuffer.data()) >= 0)
	{
		Normals.Reserve(nbVerts);
		for (unsigned int i = 0; i < nbVerts; i++)
			Normals.Add(FVector(normBuffer[i*3+0], -normBuffer[i*3+2], normBuffer[i*3+1]));
	}

	// Inf/NaN 버텍스 감지 → early return (ZeroVector 클램프 시 거대 삼각형 유발)
	for (unsigned int i = 0; i < nbVerts; i++)
	{
		if (!FMath::IsFinite(Verts[i].X) || !FMath::IsFinite(Verts[i].Y) || !FMath::IsFinite(Verts[i].Z))
		{
			UE_LOG(LogTemp, Warning, TEXT("SofaSimActor: BuildInitialMesh — invalid vertex[%u]=(%f,%f,%f), will retry"),
				i, Verts[i].X, Verts[i].Y, Verts[i].Z);
			return; // bTopoReady=false 유지 → Tick에서 재시도
		}
	}

	TArray<FVector2D> UV0;
	TArray<FLinearColor> VC;
	TArray<FProcMeshTangent> Tangents;
	// bCreateCollision=false: Inf 버텍스로 Chaos AABBTree ensure 오류 방지
	LiverMesh->CreateMeshSection_LinearColor(0, Verts, CachedTriangles, Normals, UV0, VC, Tangents, false);
	bTopoReady = true;

	UE_LOG(LogTemp, Log, TEXT("SofaSimActor: BuildInitialMesh OK — %u verts, %u tris"), nbVerts, nbTris);
}

void ASofaSimActor::StartSimulation()
{
	if (!bInitialized) return;
	SofaAPI->start();
	bRunning = true;
}

void ASofaSimActor::StopSimulation()
{
	if (!bInitialized) return;
	SofaAPI->stop();
	bRunning = false;
}

void ASofaSimActor::ResetSimulation()
{
	if (!bInitialized) return;
	SofaAPI->reset();
	bTopoReady = false;
}

void ASofaSimActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bInitialized || !bRunning) return;

	SofaAPI->step();

	// SOFA 메시지 캡처 (처음 30 스텝만 로그)
	{
		static int stepCount = 0;
		if (++stepCount <= 30)
		{
			int nbMsg = SofaAPI->getNbMessages();
			for (int i = 0; i < nbMsg; i++)
			{
				int msgType = 0;
				const char* msg = SofaAPI->getMessage(i, msgType);
				if (msg)
					UE_LOG(LogTemp, Warning, TEXT("SOFA step%d [type=%d]: %hs"), stepCount, msgType, msg);
			}
			SofaAPI->clearMessages();
		}
	}

	// 메쉬가 아직 없으면 이번 step() 이후 다시 시도 (초기 Inf 버텍스 안정화 대기)
	if (!bTopoReady)
	{
		BuildInitialMesh();
		return;
	}

	UpdateMesh();
}

void ASofaSimActor::UpdateMesh()
{
	// 토폴로지가 아직 안 만들어졌으면 스킵 (BuildInitialMesh 실패 시)
	if (!bTopoReady) return;

	SofaPhysicsOutputMesh* mesh = SofaAPI->getOutputMeshPtr((unsigned int)MeshIndex);
	if (!mesh) return;

	unsigned int nbVerts = mesh->getNbVertices();
	if (nbVerts == 0) return;

	// 버퍼 버전으로 double→float 명시적 변환
	std::vector<float> posBuffer(nbVerts * 3);
	if (mesh->getVPositions(posBuffer.data()) < 0) return;
	const float* rawPos = posBuffer.data();

	// 버텍스 변환
	TArray<FVector> Verts;
	Verts.Reserve(nbVerts);
	for (unsigned int i = 0; i < nbVerts; i++)
	{
		float sx = rawPos[i * 3 + 0];
		float sy = rawPos[i * 3 + 1];
		float sz = rawPos[i * 3 + 2];
		Verts.Add(FVector(sx * PositionScale, -sz * PositionScale, sy * PositionScale));
	}

	// NaN/Inf 감지 → 해당 프레임 스킵
	for (const FVector& V : Verts)
	{
		if (!FMath::IsFinite(V.X) || !FMath::IsFinite(V.Y) || !FMath::IsFinite(V.Z))
		{
			static int nanCount = 0;
			if (++nanCount <= 5)
				UE_LOG(LogTemp, Warning, TEXT("SofaSimActor: NaN/Inf vertex detected — skipping frame %d"), nanCount);
			return;
		}
	}

	// 노멀 (버퍼 버전)
	std::vector<float> normBuffer(nbVerts * 3);
	TArray<FVector> Normals;
	if (mesh->getVNormals(normBuffer.data()) >= 0)
	{
		Normals.Reserve(nbVerts);
		for (unsigned int i = 0; i < nbVerts; i++)
			Normals.Add(FVector(normBuffer[i*3+0], -normBuffer[i*3+2], normBuffer[i*3+1]));
	}

	LiverMesh->UpdateMeshSection(0, Verts, Normals,
		TArray<FVector2D>(), TArray<FColor>(), TArray<FProcMeshTangent>());
}

void ASofaSimActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ShutdownSOFA();
	Super::EndPlay(EndPlayReason);
}

void ASofaSimActor::ShutdownSOFA()
{
	bInitialized = false;
	bRunning     = false;

	if (SofaAPI)
	{
		SofaAPI->stop();
		FPlatformProcess::Sleep(0.5f);
		delete SofaAPI;
		SofaAPI = nullptr;
	}
}

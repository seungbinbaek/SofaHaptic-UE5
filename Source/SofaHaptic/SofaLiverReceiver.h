// SofaLiverReceiver.h
// TCP로 SOFA에서 간 메쉬 vertex 데이터를 수신하여 ProceduralMesh로 렌더링

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "ProceduralMeshComponent.h"
#include "SofaLiverReceiver.generated.h"

// ─────────────────────────────────────────────
// 백그라운드 수신 스레드
// ─────────────────────────────────────────────
class FSofaReceiverRunnable : public FRunnable
{
public:
	explicit FSofaReceiverRunnable(class ASofaLiverReceiver* InOwner);

	virtual bool   Init() override { return true; }
	virtual uint32 Run()  override;
	virtual void   Stop() override { bStop = true; }

private:
	// 정확히 Len 바이트 수신 (partial recv 처리)
	bool RecvAll(class FSocket* Sock, uint8* Buf, int32 Len);

	ASofaLiverReceiver* Owner;
	FThreadSafeBool     bStop;
};

// ─────────────────────────────────────────────
// 패킷 타입 상수
// ─────────────────────────────────────────────
//  0x01 = INIT   : [type:1][frameId:4][numVerts:4][numTris:4] + [verts:12*N] + [tris:12*T]
//  0x02 = UPDATE : [type:1][frameId:4][numVerts:4]            + [verts:12*N]
static constexpr uint8 PKT_INIT   = 0x01;
static constexpr uint8 PKT_UPDATE = 0x02;

// ─────────────────────────────────────────────
// Actor
// ─────────────────────────────────────────────
UCLASS()
class SOFAHAPTIC_API ASofaLiverReceiver : public AActor
{
	GENERATED_BODY()

public:
	ASofaLiverReceiver();

	/** SOFA 소켓 포트 (SOFASocketSender.d_port 와 동일하게 설정) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOFA")
	int32 ListenPort = 7777;

	/** SOFA 단위 → UE 단위 변환 (SOFA mm 기준이면 0.1, m 기준이면 100) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOFA")
	float PositionScale = 100.f;

	UPROPERTY(VisibleAnywhere, Category = "SOFA")
	UProceduralMeshComponent* LiverMesh;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	virtual void Tick(float DeltaTime) override;

	// ── 수신 스레드에서 호출 (스레드 안전) ──────────
	void PushTopology(TArray<FVector>&& Verts, TArray<int32>&& Tris);
	void PushVertices(TArray<FVector>&& Verts);

private:
	// 소켓 정적 법선 계산
	static void ComputeNormals(const TArray<FVector>& Verts,
	                           const TArray<int32>&   Tris,
	                           TArray<FVector>&        OutNormals);

	FSocket* ListenSocket  = nullptr;
	FSocket* ClientSocket  = nullptr;

	FSofaReceiverRunnable* RecvRunnable = nullptr;
	FRunnableThread*       RecvThread   = nullptr;

	// ── 메쉬 더블버퍼 ──────────────────────────────
	FCriticalSection DataLock;
	TArray<FVector>  PendingVerts;
	TArray<int32>    CachedTris;          // 위상 고정 (INIT 이후 변경 없음)
	bool             bNewTopology = false;
	bool             bNewVerts    = false;
	bool             bHasTopo     = false;

	friend class FSofaReceiverRunnable;
};

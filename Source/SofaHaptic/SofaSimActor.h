// SofaSimActor.h
// SofaPhysicsAPI를 UE5에 내장 — TCP 없이 SOFA 시뮬레이션 직접 구동

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "SofaSimActor.generated.h"

// SofaPhysicsAPI forward declare (헤더는 cpp에서만 include)
class SofaPhysicsAPI;

UCLASS()
class SOFAHAPTIC_API ASofaSimActor : public AActor
{
	GENERATED_BODY()

public:
	ASofaSimActor();

	/** .scn 파일 절대 경로 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOFA")
	FString SceneFilePath;

	/** SOFA 단위(m) → UE 단위(cm) 변환 배율 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOFA")
	float PositionScale = 100.f;

	/** 시뮬레이션 dt (초) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOFA")
	float SimDT = 0.01f;

	/** 읽을 OutputMesh 인덱스 (보통 0 = 간 VisualModel) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SOFA")
	int32 MeshIndex = 0;

	UPROPERTY(VisibleAnywhere, Category = "SOFA")
	UProceduralMeshComponent* LiverMesh;

	UPROPERTY(VisibleAnywhere, Category = "SOFA")
	UProceduralMeshComponent* InstrumentMesh;

	// Blueprint 제어
	UFUNCTION(BlueprintCallable, Category = "SOFA")
	void StartSimulation();

	UFUNCTION(BlueprintCallable, Category = "SOFA")
	void StopSimulation();

	UFUNCTION(BlueprintCallable, Category = "SOFA")
	void ResetSimulation();

	UFUNCTION(BlueprintCallable, Category = "SOFA")
	bool IsInitialized() const { return bInitialized; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	virtual void Tick(float DeltaTime) override;

private:
	void InitSOFA();
	void ShutdownSOFA();
	void BuildInitialMesh();
	void UpdateMesh();
	void BuildInitialInstrumentMesh();
	void UpdateInstrumentMesh();

	SofaPhysicsAPI* SofaAPI = nullptr;
	bool bInitialized = false;
	bool bRunning     = false;

	TArray<int32>   CachedTriangles;
	bool            bTopoReady = false;

	TArray<int32>   CachedInstrumentTriangles;
	bool            bInstrumentTopoReady = false;
};

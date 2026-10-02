#include "BounceLaserObstacle.h"

#include "BounceBallPawn.h"
#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace BounceLaser
{
	constexpr float PostWidth = 12.f;
	/** How far the posts stick up above the beam (cm). */
	constexpr float PostOverhang = 25.f;
	constexpr float EmitterSize = 16.f;
	/** Brightness of the harmless warning flicker before a blinking laser turns on. */
	constexpr float WarningBrightness = 0.2f;
	constexpr float LightCandelas = 25.f;

	UStaticMeshComponent* MakeMesh(AActor* Owner, FName Name, USceneComponent* Parent, UStaticMesh* Mesh)
	{
		UStaticMeshComponent* Component = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Component->SetupAttachment(Parent);
		Component->SetStaticMesh(Mesh);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCastShadow(false);
		Component->SetCanEverAffectNavigation(false);
		return Component;
	}
}

ABounceLaserObstacle::ABounceLaserObstacle()
{
	PrimaryActorTick.bCanEverTick = true;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> LaserMaterial(TEXT("/Game/Bounce/Materials/M_Laser.M_Laser"));
	BeamMaterial = LaserMaterial.Object;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	// Posts are solid: the ball bounces off them like any wall.
	LeftPost = BounceLaser::MakeMesh(this, TEXT("LeftPost"), Root, CylinderMesh.Object);
	RightPost = BounceLaser::MakeMesh(this, TEXT("RightPost"), Root, CylinderMesh.Object);
	for (UStaticMeshComponent* Post : { LeftPost.Get(), RightPost.Get() })
	{
		Post->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Post->SetCastShadow(true);
	}

	BeamPivot = CreateDefaultSubobject<USceneComponent>(TEXT("BeamPivot"));
	BeamPivot->SetupAttachment(Root);

	// Engine cylinder: 100cm across, 100cm tall along Z. Roll it so its length runs along Y (post to post).
	Beam = BounceLaser::MakeMesh(this, TEXT("Beam"), BeamPivot, CylinderMesh.Object);
	Beam->SetRelativeRotation(FRotator(0.f, 0.f, 90.f));

	LeftEmitter = BounceLaser::MakeMesh(this, TEXT("LeftEmitter"), BeamPivot, SphereMesh.Object);
	RightEmitter = BounceLaser::MakeMesh(this, TEXT("RightEmitter"), BeamPivot, SphereMesh.Object);

	KillZone = CreateDefaultSubobject<UBoxComponent>(TEXT("KillZone"));
	KillZone->SetupAttachment(BeamPivot);
	KillZone->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	KillZone->SetCollisionObjectType(ECC_WorldDynamic);
	KillZone->SetCollisionResponseToAllChannels(ECR_Ignore);
	KillZone->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	KillZone->SetGenerateOverlapEvents(true);
	KillZone->SetCanEverAffectNavigation(false);

	// A tube light along the beam (point lights stretch along their X axis).
	Glow = CreateDefaultSubobject<UPointLightComponent>(TEXT("Glow"));
	Glow->SetupAttachment(BeamPivot);
	Glow->SetRelativeRotation(FRotator(0.f, 90.f, 0.f));
	Glow->SetIntensityUnits(ELightUnits::Candelas);
	Glow->SetCastShadows(false);
}

void ABounceLaserObstacle::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	SetupMaterial();
	Glow->SetLightColor(Color);
	Glow->SetVisibility(bGlowLight);
	LeftPost->SetVisibility(bShowPosts);
	RightPost->SetVisibility(bShowPosts);
	LeftPost->SetCollisionEnabled(bShowPosts ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	RightPost->SetCollisionEnabled(bShowPosts ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);

	BuildLayout();
	ApplyBeamState(true, 1.f);
}

void ABounceLaserObstacle::SetupMaterial()
{
	BeamMID = BeamMaterial ? UMaterialInstanceDynamic::Create(BeamMaterial, this) : nullptr;
	for (UStaticMeshComponent* Mesh : { Beam.Get(), LeftEmitter.Get(), RightEmitter.Get() })
	{
		Mesh->SetMaterial(0, BeamMID ? static_cast<UMaterialInterface*>(BeamMID) : BeamMaterial.Get());
	}
	if (BeamMID)
	{
		BeamMID->SetVectorParameterValue(TEXT("Color"), Color);
	}
}

void ABounceLaserObstacle::BuildLayout()
{
	const float HalfLength = Length * 0.5f;

	Beam->SetRelativeLocation(FVector(0.f, 0.f, BeamHeight));
	Beam->SetRelativeScale3D(FVector(BeamThickness / 100.f, BeamThickness / 100.f, Length / 100.f));

	LeftEmitter->SetRelativeLocation(FVector(0.f, -HalfLength, BeamHeight));
	RightEmitter->SetRelativeLocation(FVector(0.f, HalfLength, BeamHeight));
	LeftEmitter->SetRelativeScale3D(FVector(BounceLaser::EmitterSize / 100.f));
	RightEmitter->SetRelativeScale3D(FVector(BounceLaser::EmitterSize / 100.f));

	// Stop the deadly part just short of the posts so grazing a post isn't a death.
	KillZone->SetRelativeLocation(FVector(0.f, 0.f, BeamHeight));
	KillZone->SetBoxExtent(FVector(KillThickness * 0.5f, FMath::Max(HalfLength - BounceLaser::PostWidth, 1.f), KillThickness * 0.5f));

	Glow->SetRelativeLocation(FVector(0.f, 0.f, BeamHeight));
	Glow->SetSourceLength(Length);
	Glow->SetAttenuationRadius(HalfLength + 250.f);

	// Posts stand on the floor and reach just above the beam, even while it slides up and down.
	const float PivotZ = BeamPivot->GetRelativeLocation().Z;
	const float PostHeight = FMath::Max(BeamHeight + PivotZ + BounceLaser::PostOverhang, 10.f);
	const FVector PivotXY(BeamPivot->GetRelativeLocation().X, BeamPivot->GetRelativeLocation().Y, 0.f);
	const FVector PostScale(BounceLaser::PostWidth / 100.f, BounceLaser::PostWidth / 100.f, PostHeight / 100.f);
	LeftPost->SetRelativeLocationAndRotation(PivotXY + FVector(0.f, -HalfLength, PostHeight * 0.5f), FRotator::ZeroRotator);
	RightPost->SetRelativeLocationAndRotation(PivotXY + FVector(0.f, HalfLength, PostHeight * 0.5f), FRotator::ZeroRotator);
	LeftPost->SetRelativeScale3D(PostScale);
	RightPost->SetRelativeScale3D(PostScale);
}

void ABounceLaserObstacle::BeginPlay()
{
	Super::BeginPlay();
	KillZone->OnComponentBeginOverlap.AddUniqueDynamic(this, &ABounceLaserObstacle::HandleKillZoneOverlap);
	Age = 0.f;
	// The material instance is transient, so placed lasers need a fresh one at runtime.
	if (!BeamMID)
	{
		SetupMaterial();
	}
}

void ABounceLaserObstacle::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;

	if (!MoveOffset.IsNearlyZero())
	{
		// Smooth back-and-forth: 0 -> MoveOffset -> 0.
		const float Cycle = (Age / MovePeriod + MovePhase) * UE_TWO_PI;
		const float Alpha = 0.5f - 0.5f * FMath::Cos(Cycle);
		BeamPivot->SetRelativeLocation(MoveOffset * Alpha);
		BuildLayout();
	}

	bool bOn = bEnabled;
	float Brightness = bEnabled ? 1.f : 0.f;
	if (bEnabled && bBlink)
	{
		const float CycleLength = OnTime + OffTime;
		const float T = FMath::Fmod(Age + BlinkOffset, CycleLength);
		bOn = T < OnTime;
		if (bOn)
		{
			Brightness = 1.f;
		}
		else if (T >= CycleLength - WarningTime)
		{
			// Flicker as a warning; harmless.
			Brightness = FMath::Fmod(Age * 20.f, 1.f) < 0.5f ? BounceLaser::WarningBrightness : BounceLaser::WarningBrightness * 0.3f;
		}
		else
		{
			Brightness = 0.f;
		}
	}
	ApplyBeamState(bOn, Brightness);

	// Catches the ball sitting in the beam at the moment it switches on.
	if (bLaserOn)
	{
		TArray<AActor*> Overlaps;
		KillZone->GetOverlappingActors(Overlaps, ABounceBallPawn::StaticClass());
		for (AActor* Actor : Overlaps)
		{
			Zap(Actor);
		}
	}
}

void ABounceLaserObstacle::ApplyBeamState(bool bOn, float Brightness)
{
	bLaserOn = bOn;
	const bool bVisible = Brightness > 0.f;
	Beam->SetVisibility(bVisible);
	if (BeamMID)
	{
		BeamMID->SetScalarParameterValue(TEXT("Glow"), GlowStrength * FMath::Max(Brightness, 0.05f));
	}
	Glow->SetIntensity(bGlowLight ? BounceLaser::LightCandelas * Brightness : 0.f);
	KillZone->SetCollisionEnabled(bOn ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
}

void ABounceLaserObstacle::SetLaserEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
}

void ABounceLaserObstacle::HandleKillZoneOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (bLaserOn)
	{
		Zap(OtherActor);
	}
}

void ABounceLaserObstacle::Zap(AActor* OtherActor)
{
	ABounceBallPawn* Player = Cast<ABounceBallPawn>(OtherActor);
	if (!Player || Player->IsDead())
	{
		return;
	}

	// The point on the beam closest to the ball.
	const FVector BeamStart = Beam->GetComponentLocation() - Beam->GetUpVector() * Length * 0.5f;
	const FVector BeamEnd = Beam->GetComponentLocation() + Beam->GetUpVector() * Length * 0.5f;
	const FVector ZapLocation = FMath::ClosestPointOnSegment(Player->GetActorLocation(), BeamStart, BeamEnd);

	Player->Pop(ZapLocation);
	ReceiveZapped(Player, ZapLocation);
}

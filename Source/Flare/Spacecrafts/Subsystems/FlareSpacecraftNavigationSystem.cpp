
#include "../../Flare.h"

#include "FlareSpacecraftNavigationSystem.h"
#include "../FlareSpacecraft.h"

#define LOCTEXT_NAMESPACE "FlareSpacecraftNavigationSystem"

/*----------------------------------------------------
	Constructor
----------------------------------------------------*/

UFlareSpacecraftNavigationSystem::UFlareSpacecraftNavigationSystem(const class FObjectInitializer& PCIP)
	: Super(PCIP)
	, Spacecraft(NULL)
	, Status(EFlareShipStatus::SS_Manual)
	, AngularDeadAngle(0.5)
	, LinearDeadDistance(0.1)
	, LinearMaxDockingVelocity(10)
	, NegligibleSpeedRatio(0.0005)
{
	AnticollisionAngle = FMath::FRandRange(0, 360);
}


/*----------------------------------------------------
	Gameplay events
----------------------------------------------------*/

void UFlareSpacecraftNavigationSystem::TickSystem(float DeltaSeconds)
{
	UpdateCOM();

	// Manual pilot
	if (IsManualPilot() && Spacecraft->GetDamageSystem()->IsAlive())
	{
		LinearTargetVelocity = Spacecraft->GetStateManager()->GetLinearTargetVelocity();
		AngularTargetVelocity = Spacecraft->GetStateManager()->GetAngularTargetVelocity();
		UseOrbitalBoost = Spacecraft->GetStateManager()->IsUseOrbitalBoost();

		if (Spacecraft->GetStateManager()->IsWantFire())
		{
			Spacecraft->GetWeaponsSystem()->StartFire();
		}
		else
		{
			Spacecraft->GetWeaponsSystem()->StopFire();
		}
	}

	// Autopilot
	else if (IsAutoPilot())
	{
		FFlareShipCommandData CurrentCommand;
		if (CommandData.Peek(CurrentCommand))
		{
			if (CurrentCommand.Type == EFlareCommandDataType::CDT_Location)
			{
				if(UpdateLinearAttitudeAuto(DeltaSeconds, CurrentCommand.LocationTarget, FVector::ZeroVector, (CurrentCommand.PreciseApproach ? LinearMaxDockingVelocity : LinearMaxVelocity)))
				{
					ClearCurrentCommand();
				}
			}
			else if (CurrentCommand.Type == EFlareCommandDataType::CDT_BrakeLocation)
			{
				UpdateLinearBraking(DeltaSeconds);
			}
			else if (CurrentCommand.Type == EFlareCommandDataType::CDT_Rotation)
			{
				UpdateAngularAttitudeAuto(DeltaSeconds);
			}
			else if (CurrentCommand.Type == EFlareCommandDataType::CDT_BrakeRotation)
			{
				UpdateAngularBraking(DeltaSeconds);
			}
			else if (CurrentCommand.Type == EFlareCommandDataType::CDT_Dock)
			{
				DockingAutopilot(Cast<IFlareSpacecraftInterface>(CurrentCommand.ActionTarget), CurrentCommand.ActionTargetParam, DeltaSeconds);
			}
		}

		// TODO Autopilot anticollision system
	}

	// Physics
	if (!IsDocked())
	{
		// TODO enable physic when docked but attach the ship to the station

		PhysicSubTick(DeltaSeconds);
	}
}

void UFlareSpacecraftNavigationSystem::Initialize(AFlareSpacecraft* OwnerSpacecraft, FFlareSpacecraftSave* OwnerData)
{
	Spacecraft = OwnerSpacecraft;
	Components = Spacecraft->GetComponentsByClass(UFlareSpacecraftComponent::StaticClass());
	Description = Spacecraft->GetDescription();
	Data = OwnerData;

	// Load data from the ship info
	if (Description)
	{
		LinearMaxVelocity = Description->LinearMaxVelocity;
		AngularMaxVelocity = Description->AngularMaxVelocity;
	}
}

void UFlareSpacecraftNavigationSystem::Start()
{
	UpdateCOM();
}


bool UFlareSpacecraftNavigationSystem::IsManualPilot()
{
	return (Status == EFlareShipStatus::SS_Manual);
}

bool UFlareSpacecraftNavigationSystem::IsAutoPilot()
{
	return (Status == EFlareShipStatus::SS_AutoPilot);
}

bool UFlareSpacecraftNavigationSystem::IsDocked()
{
	return (Status == EFlareShipStatus::SS_Docked);
}

void UFlareSpacecraftNavigationSystem::SetStatus(EFlareShipStatus::Type NewStatus)
{
	FLOGV("AFlareSpacecraft::SetStatus %d", NewStatus - EFlareShipStatus::SS_Manual);
	Status = NewStatus;
}

void UFlareSpacecraftNavigationSystem::SetAngularAccelerationRate(float Acceleration)
{
	AngularAccelerationRate = Acceleration;
}

/*----------------------------------------------------
	Docking
----------------------------------------------------*/

bool UFlareSpacecraftNavigationSystem::DockAt(IFlareSpacecraftInterface* TargetStation)
{
	FLOG("AFlareSpacecraft::DockAt");
	FFlareDockingInfo DockingInfo = TargetStation->GetDockingSystem()->RequestDock(Spacecraft, Spacecraft->GetActorLocation());

	// Try to dock
	if (DockingInfo.Granted)
	{
		FLOG("AFlareSpacecraft::DockAt : access granted");
		PushCommandDock(DockingInfo);
		return true;
	}

	// Failed
	FLOG("AFlareSpacecraft::DockAt failed");
	return false;
}

bool UFlareSpacecraftNavigationSystem::Undock()
{
	FLOG("AFlareSpacecraft::Undock");
	FFlareShipCommandData Head;

	// Try undocking
	if (IsDocked())
	{
		// Detach from station
		Spacecraft->DetachRootComponentFromParent(true);

		// Evacuate
		GetDockStation()->GetDockingSystem()->ReleaseDock(Spacecraft, Data->DockedAt);
		PushCommandLocation(Spacecraft->GetRootComponent()->GetComponentTransform().TransformPositionNoScale(5000 * FVector(-1, 0, 0)));

		// Update data
		SetStatus(EFlareShipStatus::SS_AutoPilot);
		Data->DockedTo = NAME_None;
		Data->DockedAt = -1;

		// Update Angular acceleration rate : when it's docked the mass is the ship mass + the station mass
		Spacecraft->SetRCSDescription(Spacecraft->GetRCSDescription());

		FLOG("AFlareSpacecraft::Undock successful");
		return true;
	}

	// Failed
	FLOG("AFlareSpacecraft::Undock failed");
	return false;
}

IFlareSpacecraftInterface* UFlareSpacecraftNavigationSystem::GetDockStation()
{
	if (IsDocked())
	{
		for (TActorIterator<AActor> ActorItr(Spacecraft->GetWorld()); ActorItr; ++ActorItr)
		{
			AFlareSpacecraft* Station = Cast<AFlareSpacecraft>(*ActorItr);
			if (Station && *Station->GetName() == Data->DockedTo)
			{
				return Station;
			}
		}
	}
	return NULL;
}

static float GetApproachDockToDockLateralDistanceLimit(float Distance)
{
	// Approch cone :
	//  At 1 m -> 1 m
	//  At 100 m -> 25 m
	return Distance / 4 + 75;
}

static float GetApproachVelocityLimit(float Distance)
{
	// Approch cone :
	//  At 1 m -> 5 m/s
	//  At 100 m -> 40 m/s
	return Distance / 2.5 + 460;
}


void UFlareSpacecraftNavigationSystem::DockingAutopilot(IFlareSpacecraftInterface* DockStationInterface, int32 DockId, float DeltaSeconds)
{
	// The dockin has multiple phase
	// - Rendez-vous : go at the entrance of the docking corridor.
	//     Its a large sphere  in front of the dock with a speed limit. During
	//     the approch phase the ship can go as phase is as he want.
	//
	// - Approch (500 m): The ship advance in the approch corridor and try to keep itself
	//    near the dock axis an align th ship.
	//    The approch corridor is a cone with speed limitation more and more strict
	//    approching the station. There is a prefered  speed and a limit speed. If
	//    the limit speed is reach, the ship must abord and return to the entrance
	//    of the docking corridor. The is also a angular limit.
	//
	// - Final Approch (5 m) : The ship slowly advance and wait for the docking trying to keep
	//    the speed and alignement.
	//
	// - Docking : As soon as the ship is in the docking limits, the ship is attached to the station


	AFlareSpacecraft* DockStation = Cast<AFlareSpacecraft>(DockStationInterface);
	if(!DockStation)
	{
		//TODO LOG
		return;
	}
	//FLOG("==============DockingAutopilot==============");

	float DockingDockToDockDistanceLimit = 20; // 20 cm of linear distance to dock
	float FinalApproachDockToDockDistanceLimit = 100; // 1 m of linear distance
	float ApproachDockToDockDistanceLimit = 10000; // 100 m approch distance

	float FinalApproachDockToDockLateralDistanceLimit = 100; // 50 cm of linear lateral distance

	float DockingAngleLimit = 1; // 1° of angle error to dock
	float FinalApproachAngleLimit = 10;// 10° of angle error to dock

	float DockingVelocityLimit = 100; // 1 m/s
	float FinalApproachVelocityLimit = 500; // 5 m/s

	float DockingLateralVelocityLimit = 10; // 10 cm/s
	float FinalApproachLateralVelocityLimit = 50; // 0.5 m/s
	float ApproachLateralVelocityLimit = 1000; // 10 m/s

	float DockingAngularVelocityLimit = 5; // 5 °/s
	float FinalApproachAngularVelocityLimit = 10; // 10 °/s


	FVector ShipDockAxis = Spacecraft->Airframe->GetComponentToWorld().GetRotation().RotateVector(FVector(1, 0, 0)); // Ship docking port are always at front
	FVector ShipDockLocation = GetDockLocation();
	FVector ShipDockOffset = ShipDockLocation - Spacecraft->GetActorLocation();
	FVector ShipAngularVelocity = Spacecraft->Airframe->GetPhysicsAngularVelocity();




	FFlareDockingInfo StationDockInfo = DockStation->GetDockingSystem()->GetDockInfo(DockId);
	FVector StationCOM = DockStation->Airframe->GetBodyInstance()->GetCOMPosition();
	FVector StationDockAxis = DockStation->Airframe->GetComponentToWorld().GetRotation().RotateVector(StationDockInfo.LocalAxis);
	FVector StationDockLocation =  DockStation->Airframe->GetComponentTransform().TransformPosition(StationDockInfo.LocalLocation);
	FVector StationDockOffset = StationDockLocation - DockStation->GetActorLocation();
	FVector StationAngularVelocity = DockStation->Airframe->GetPhysicsAngularVelocity();

	// Compute docking infos
	FVector DockToDockDeltaLocation = StationDockLocation - ShipDockLocation;
	float DockToDockDistance = DockToDockDeltaLocation.Size();

	// The linear velocity of the docking port induced by the station or ship rotation
	FVector ShipDockSelfRotationInductedLinearVelocity = (PI /  180.f) * FVector::CrossProduct(ShipAngularVelocity, ShipDockLocation-COM);
	FVector ShipDockLinearVelocity = ShipDockSelfRotationInductedLinearVelocity + Spacecraft->GetLinearVelocity() * 100;

	FVector StationDockSelfRotationInductedLinearVelocity = (PI /  180.f) * FVector::CrossProduct(StationAngularVelocity, StationDockLocation- StationCOM);
	FVector StationDockLinearVelocity = StationDockSelfRotationInductedLinearVelocity + DockStation->GetLinearVelocity() * 100;

	float InAxisDistance = FVector::DotProduct(DockToDockDeltaLocation, -StationDockAxis);
	FVector RotationInductedLinearVelocityAtShipDistance = (PI /  180.f) * FVector::CrossProduct(StationAngularVelocity, (StationDockLocation - StationCOM).GetUnsafeNormal() * (InAxisDistance + (StationDockLocation - StationCOM).Size()));
	FVector LinearVelocityAtShipDistance = RotationInductedLinearVelocityAtShipDistance + DockStation->GetLinearVelocity() * 100;


	AFlareSpacecraft* AnticollisionDockStation = DockStation;
	FVector RelativeDockToDockLinearVelocity = StationDockLinearVelocity - ShipDockLinearVelocity;
	float DockToDockAngle = FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(-ShipDockAxis, StationDockAxis)));

	// Angular velocity must be the same
	FVector RelativeDockAngularVelocity = ShipAngularVelocity - StationAngularVelocity;

/*
	FLOGV("ShipLocation=%s", *(Spacecraft->GetActorLocation().ToString()));

	FLOGV("ShipDockAxis=%s", *ShipDockAxis.ToString());
	FLOGV("ShipDockOffset=%s", *ShipDockOffset.ToString());
	FLOGV("ShipDockLocation=%s", *ShipDockLocation.ToString());
	FLOGV("ShipAngularVelocity=%s", *ShipAngularVelocity.ToString());

	FLOGV("StationLocation=%s", *(DockStation->GetActorLocation().ToString()));
	FLOGV("StationCOM=%s", *StationCOM.ToString());
	FLOGV("LocalAxis=%s", *(StationDockInfo.LocalAxis.ToString()));
	FLOGV("StationDockAxis=%s", *StationDockAxis.ToString());
	FLOGV("StationDockOffset=%s", *StationDockOffset.ToString());
	FLOGV("StationDockLocation=%s", *StationDockLocation.ToString());
	FLOGV("StationAngularVelocity=%s", *StationAngularVelocity.ToString());

	FLOGV("DockToDockDeltaLocation=%s", *DockToDockDeltaLocation.ToString());
	FLOGV("DockToDockDistance=%f", DockToDockDistance);

	FLOGV("ShipDockSelfRotationInductedLinearVelocity=%s", *ShipDockSelfRotationInductedLinearVelocity.ToString());
	FLOGV("ShipDockLinearVelocity=%s", *ShipDockLinearVelocity.ToString());
	FLOGV("StationDockSelfRotationInductedLinearVelocity=%s", *StationDockSelfRotationInductedLinearVelocity.ToString());
	FLOGV("StationDockLinearVelocity=%s", *StationDockLinearVelocity.ToString());

	FLOGV("RelativeDockToDockLinearVelocity=%s", *RelativeDockToDockLinearVelocity.ToString());


	FLOGV("RotationInductedLinearVelocityAtShipDistance=%s", *RotationInductedLinearVelocityAtShipDistance.ToString());
	FLOGV("LinearVelocityAtShipDistance=%s", *LinearVelocityAtShipDistance.ToString());
	FLOGV("InAxisDistance=%f", InAxisDistance);

	FLOGV("DockToDockAngle=%f", DockToDockAngle);
*/
	//DrawDebugSphere(Spacecraft->GetWorld(), ShipDockLocation, 100, 12, FColor::Red, false,0.03);
	//DrawDebugSphere(Spacecraft->GetWorld(), StationDockLocation, 100, 12, FColor::Blue, false,0.03);
	// Output
	float MaxVelocity = 0;
	FVector LocationTarget = StationDockLocation - ShipDockOffset;
	FVector AxisTarget = -StationDockAxis;
	FVector AngularVelocityTarget = StationAngularVelocity;
	FVector VelocityTarget = LinearVelocityAtShipDistance - ShipDockSelfRotationInductedLinearVelocity;
	bool Anticollision = true;
	/*FLOGV("Initial LocationTarget=%s", *LocationTarget.ToString());
	FLOGV("Initial AxisTarget=%s", *AxisTarget.ToString());
	FLOGV("Initial AngularVelocityTarget=%s", *AngularVelocityTarget.ToString());
	FLOGV("Initial VelocityTarget=%s", *VelocityTarget.ToString());
*/
	// First find the current docking phase

	// Check if dockable
	bool OkForDocking = true;
	if (DockToDockDistance > DockingDockToDockDistanceLimit)
	{
		/*FLOG("OkForDocking ? Too far");
		FLOGV("  - limit= %f", DockingDockToDockDistanceLimit);
		FLOGV("  - distance= %f", DockToDockDistance);*/
		// Too far
		OkForDocking = false;
	}
	else if (DockToDockAngle > DockingAngleLimit)
	{
		//FLOG("OkForDocking ? Not aligned");
		// Not aligned
		OkForDocking = false;
	}
	else if (RelativeDockToDockLinearVelocity.Size() > DockingVelocityLimit)
	{
		//FLOG("OkForDocking ? Too fast");
		// Too fast
		OkForDocking = false;
	}
	else if (FVector::VectorPlaneProject(RelativeDockToDockLinearVelocity, StationDockAxis).Size()  > DockingLateralVelocityLimit)
	{
		//FLOG("OkForDocking ? Too much lateral velocity");
		// Too much lateral velocity
		OkForDocking = false;
	}
	else if (RelativeDockAngularVelocity.Size() > DockingAngularVelocityLimit)
	{
		//FLOG("OkForDocking ? Too much angular velocity");
		// Too much angular velocity
		OkForDocking = false;
	}

	if (OkForDocking)
	{
		//FLOG("-> OK for docking");
		ConfirmDock(DockStation, DockId);
		return;
	}

	bool InFinalApproach = true;
	// Not dockable, check if in final approch
	if (DockToDockDistance > FinalApproachDockToDockDistanceLimit)
	{
		//FLOG("InFinalApproach ? Too far");
		// Too far
		InFinalApproach = false;
	}
	else if (FVector::VectorPlaneProject(DockToDockDeltaLocation, StationDockAxis).Size() > FinalApproachDockToDockLateralDistanceLimit)
	{
		//FLOG("InFinalApproach ? Too far in lateral axis");
		// Too far in lateral axis
		InFinalApproach = false;
	}
	else if (DockToDockAngle > FinalApproachAngleLimit)
	{
		//FLOG("InFinalApproach ? Not aligned");
		// Not aligned
		InFinalApproach = false;
	}
	else if (RelativeDockToDockLinearVelocity.Size() > FinalApproachVelocityLimit)
	{
		/*FLOG("InFinalApproach ? Too fast");
		FLOGV("  - limit= %f", FinalApproachVelocityLimit);
		FLOGV("  - velocity= %f", RelativeDockToDockLinearVelocity.Size());*/
		// Too fast
		InFinalApproach = false;
	}
	else if (FVector::VectorPlaneProject(RelativeDockToDockLinearVelocity, StationDockAxis).Size()  > FinalApproachLateralVelocityLimit)
	{
		//FLOG("InFinalApproach ? Too much lateral velocity");
		// Too much lateral velocity
		InFinalApproach = false;
	}
	else if (RelativeDockAngularVelocity.Size() > FinalApproachAngularVelocityLimit)
	{
		//FLOG("InFinalApproach ? Too much angular velocity");
		// Too much angular velocity
		InFinalApproach = false;
	}

	if (InFinalApproach)
	{
		//FLOG("-> In final approach");
		MaxVelocity = DockingVelocityLimit / 200;
	}
	else
	{
		bool InApproach = true;
		// Not in final approch, check if in approch
		if (DockToDockDistance > ApproachDockToDockDistanceLimit)
		{
			//FLOG("InApproch ? Too far");
			// Too far
			InApproach = false;
		}
		else if (FVector::VectorPlaneProject(DockToDockDeltaLocation, StationDockAxis).Size() > GetApproachDockToDockLateralDistanceLimit(DockToDockDistance))
		{
			/*FLOG("InApproch ? Too far in lateral axis");
			FLOGV("  - limit= %f", GetApproachDockToDockLateralDistanceLimit(DockToDockDistance));
			FLOGV("  - distance= %f", FVector::VectorPlaneProject(DockToDockDeltaLocation, StationDockAxis).Size());*/

			// Too far in lateral axis
			InApproach = false;
		}
		else if (RelativeDockToDockLinearVelocity.Size() > GetApproachVelocityLimit(DockToDockDistance))
		{
			/*FLOG("InApproch ? Too fast");
			FLOGV("  - limit= %f",  GetApproachVelocityLimit(DockToDockDistance));
			FLOGV("  - velocity= %f", RelativeDockToDockLinearVelocity.Size());*/

			// Too fast
			InApproach = false;
		}

		if(InApproach)
		{
			//FLOG("-> In approach");
			MaxVelocity = GetApproachVelocityLimit(DockToDockDistance) / 200 ;
			LocationTarget += StationDockAxis * (FinalApproachDockToDockDistanceLimit / 2);
			//FLOGV("Location offset=%s", *((StationDockAxis * (FinalApproachDockToDockDistanceLimit / 2)).ToString()));
		}
		else
		{
			//FLOG("-> Rendez-vous");
			MaxVelocity = LinearMaxVelocity;
			LocationTarget += StationDockAxis * (ApproachDockToDockDistanceLimit / 2);
			if(DockToDockDistance > ApproachDockToDockDistanceLimit)
			{
				AxisTarget = LocationTarget - ShipDockLocation;
				AngularVelocityTarget = FVector::ZeroVector;
			}
			// During rendez-vous avoid the station if not in axis
			if(FVector::DotProduct((ShipDockLocation - ShipDockLocation).GetUnsafeNormal(), StationDockAxis) < 0.5)
			{
				AnticollisionDockStation = NULL;
			}
			Anticollision = false;

			//FLOGV("Location offset=%s", *((StationDockAxis * (ApproachDockToDockDistanceLimit / 2)).ToString()));
		}
	}
	/*FLOGV("MaxVelocity=%f", MaxVelocity);
	FLOGV("LocationTarget=%s", *LocationTarget.ToString());
	FLOGV("AxisTarget=%s", *AxisTarget.ToString());
	FLOGV("AngularVelocityTarget=%s", *AngularVelocityTarget.ToString());
	FLOGV("VelocityTarget=%s", *VelocityTarget.ToString());


	DrawDebugSphere(Spacecraft->GetWorld(), LocationTarget, 100, 12, FColor::Green, false,0.03);
*/
	//UpdateLinearAttitudeAuto(DeltaSeconds, (CurrentCommand.PreciseApproach ? LinearMaxDockingVelocity : LinearMaxVelocity));
	//UpdateAngularAttitudeAuto(DeltaSeconds);

	// Not in approach, just go to the docking entrance point
	UpdateLinearAttitudeAuto(DeltaSeconds, LocationTarget, VelocityTarget/100, MaxVelocity);
	AngularTargetVelocity = GetAngularVelocityToAlignAxis(FVector(1,0,0), AxisTarget, AngularVelocityTarget, DeltaSeconds);

	if(Anticollision)
	{
		// During docking, lets the others avoid me
		LinearTargetVelocity = AnticollisionCorrection(LinearTargetVelocity, AnticollisionDockStation);
	}

	/*FLOGV("AngularTargetVelocity=%s", *AngularTargetVelocity.ToString());
	FLOGV("LinearTargetVelocity=%s", *LinearTargetVelocity.ToString());
*/
	// TODO refactor to get the position in parameter
}


void UFlareSpacecraftNavigationSystem::ConfirmDock(IFlareSpacecraftInterface* DockStation, int32 DockId)
{
	FLOG("AFlareSpacecraft::ConfirmDock");
	ClearCurrentCommand();

	// Set as docked
	DockStation->GetDockingSystem()->Dock(Spacecraft, DockId);
	SetStatus(EFlareShipStatus::SS_Docked);
	Data->DockedTo = *DockStation->_getUObject()->GetName();
	Data->DockedAt = DockId;


	// Attach to station
	AFlareSpacecraft* Station = Cast<AFlareSpacecraft>(DockStation);
	if(Station)
	{
		Spacecraft->AttachRootComponentToActor(Station,"", EAttachLocation::KeepWorldPosition, true);
	}


	// Cut engines
	TArray<UActorComponent*> Engines = Spacecraft->GetComponentsByClass(UFlareEngine::StaticClass());
	for (int32 EngineIndex = 0; EngineIndex < Engines.Num(); EngineIndex++)
	{
		UFlareEngine* Engine = Cast<UFlareEngine>(Engines[EngineIndex]);
		Engine->SetAlpha(0.0f);
	}

	Spacecraft->OnDocked();
}

/*----------------------------------------------------
	Navigation commands and helpers
----------------------------------------------------*/

void UFlareSpacecraftNavigationSystem::PushCommandLinearBrake()
{
	FFlareShipCommandData Command;
	Command.Type = EFlareCommandDataType::CDT_BrakeLocation;
	PushCommand(Command);
}

void UFlareSpacecraftNavigationSystem::PushCommandAngularBrake()
{
	FFlareShipCommandData Command;
	Command.Type = EFlareCommandDataType::CDT_BrakeRotation;
	PushCommand(Command);
}

void UFlareSpacecraftNavigationSystem::PushCommandLocation(const FVector& Location, bool Precise)
{
	FFlareShipCommandData Command;
	Command.Type = EFlareCommandDataType::CDT_Location;
	Command.LocationTarget = Location;
	Command.PreciseApproach = Precise;
	PushCommand(Command);
}

void UFlareSpacecraftNavigationSystem::PushCommandRotation(const FVector& RotationTarget, const FVector& LocalShipAxis)
{
	FFlareShipCommandData Command;
	Command.Type = EFlareCommandDataType::CDT_Rotation;
	Command.RotationTarget = RotationTarget;
	Command.LocalShipAxis = LocalShipAxis;
	FLOGV("PushCommandRotation RotationTarget '%s'", *RotationTarget.ToString());
	FLOGV("PushCommandRotation LocalShipAxis '%s'", *LocalShipAxis.ToString());
	PushCommand(Command);
}

void UFlareSpacecraftNavigationSystem::PushCommandDock(const FFlareDockingInfo& DockingInfo)
{
	FFlareShipCommandData Command;
	Command.Type = EFlareCommandDataType::CDT_Dock;
	Command.ActionTarget = Cast<AFlareSpacecraft>(DockingInfo.Station);
	Command.ActionTargetParam = DockingInfo.DockId;
	PushCommand(Command);
}

void UFlareSpacecraftNavigationSystem::PushCommand(const FFlareShipCommandData& Command)
{
	SetStatus(EFlareShipStatus::SS_AutoPilot);
	CommandData.Enqueue(Command);

	FLOGV("Pushed command '%s'", *EFlareCommandDataType::ToString(Command.Type));
}

void UFlareSpacecraftNavigationSystem::ClearCurrentCommand()
{
	FFlareShipCommandData Command;
	CommandData.Dequeue(Command);

	FLOGV("Cleared command '%s'", *EFlareCommandDataType::ToString(Command.Type));

	if (!CommandData.Peek(Command))
	{
		SetStatus(EFlareShipStatus::SS_Manual);
	}
}

void UFlareSpacecraftNavigationSystem::AbortAllCommands()
{
	FFlareShipCommandData Command;

	while (CommandData.Dequeue(Command))
	{
		FLOGV("Abort command '%s'", *EFlareCommandDataType::ToString(Command.Type));
		if (Command.Type == EFlareCommandDataType::CDT_Dock)
		{
			// Release dock grant
			IFlareSpacecraftInterface* Station = Cast<IFlareSpacecraftInterface>(Command.ActionTarget);
			Station->GetDockingSystem()->ReleaseDock(Spacecraft, Command.ActionTargetParam);
		}
	}
	SetStatus(EFlareShipStatus::SS_Manual);
}

FVector UFlareSpacecraftNavigationSystem::GetDockLocation()
{
	return Spacecraft->GetRootComponent()->GetSocketLocation(FName("Dock"));
}

FVector UFlareSpacecraftNavigationSystem::GetDockOffset()
{
	return Spacecraft->GetRootComponent()->GetComponentTransform().InverseTransformPosition(GetDockLocation());
}

bool UFlareSpacecraftNavigationSystem::ComputePath(TArray<FVector>& Path, TArray<AActor*>& PossibleColliders, FVector OriginLocation, FVector TargetLocation, float ShipSize)
{
	// Travel information
	float TravelLength;
	FVector TravelDirection;
	FVector Travel = TargetLocation - OriginLocation;
	Travel.ToDirectionAndLength(TravelDirection, TravelLength);

	for (int32 i = 0; i < PossibleColliders.Num(); i++)
	{
		// Get collider info
		FVector ColliderLocation;
		FVector ColliderExtent;
		PossibleColliders[i]->GetActorBounds(true, ColliderLocation, ColliderExtent);
		float ColliderSize = ShipSize + ColliderExtent.Size();

		// Colliding : split the travel
		if (FMath::LineSphereIntersection(OriginLocation, TravelDirection, TravelLength, ColliderLocation, ColliderSize))
		{
			//DrawDebugSphere(GetWorld(), ColliderLocation, ColliderSize, 12, FColor::Blue, true);

			// Get an orthogonal plane
			FPlane TravelOrthoPlane = FPlane(ColliderLocation, TargetLocation - ColliderLocation);
			FVector IntersectedLocation = FMath::LinePlaneIntersection(OriginLocation, TargetLocation, TravelOrthoPlane);

			// Relocate intersection inside the sphere
			FVector Intersector = IntersectedLocation - ColliderLocation;
			Intersector.Normalize();
			IntersectedLocation = ColliderLocation + Intersector * ColliderSize;

			// Collisions
			bool IsColliding = IsPointColliding(IntersectedLocation, PossibleColliders[i]);
			//DrawDebugPoint(GetWorld(), IntersectedLocation, 8, IsColliding ? FColor::Red : FColor::Green, true);

			// Dead end, go back
			if (IsColliding)
			{
				return false;
			}

			// Split travel
			else
			{
				Path.Add(IntersectedLocation);
				PossibleColliders.RemoveAt(i, 1);
				bool FirstPartOK = ComputePath(Path, PossibleColliders, OriginLocation, IntersectedLocation, ShipSize);
				bool SecondPartOK = ComputePath(Path, PossibleColliders, IntersectedLocation, TargetLocation, ShipSize);
				return FirstPartOK && SecondPartOK;
			}

		}
	}

	// No collision found
	return true;
}

void UFlareSpacecraftNavigationSystem::UpdateColliders()
{
	PathColliders.Empty();
	for (TActorIterator<AActor> ActorItr(Spacecraft->GetWorld()); ActorItr; ++ActorItr)
	{
		FVector Unused;
		FVector ColliderExtent;
		ActorItr->GetActorBounds(true, Unused, ColliderExtent);

		if (ColliderExtent.Size() < 100000 && ActorItr->IsRootComponentMovable())
		{
			PathColliders.Add(*ActorItr);
		}
	}
}

bool UFlareSpacecraftNavigationSystem::IsPointColliding(FVector Candidate, AActor* Ignore)
{
	for (int32 i = 0; i < PathColliders.Num(); i++)
	{
		FVector ColliderLocation;
		FVector ColliderExtent;
		PathColliders[i]->GetActorBounds(true, ColliderLocation, ColliderExtent);

		if ((Candidate - ColliderLocation).Size() < ColliderExtent.Size() && PathColliders[i] != Ignore)
		{
			return true;
		}
	}

	return false;
}

bool UFlareSpacecraftNavigationSystem::NavigateTo(FVector TargetLocation)
{
	// Pathfinding data
	TArray<FVector> Path;
	FVector Unused;
	FVector ShipExtent;
	FVector Temp = Spacecraft->GetActorLocation();

	// Prepare data
	FLOG("AFlareSpacecraft::NavigateTo");
	Spacecraft->GetActorBounds(true, Unused, ShipExtent);
	UpdateColliders();

	// Compute path
	if (ComputePath(Path, PathColliders, Temp, TargetLocation, ShipExtent.Size()))
	{
		FLOGV("AFlareSpacecraft::NavigateTo : generating path (%d stops)", Path.Num());

		// Generate commands for travel
		for (int32 i = 0; i < Path.Num(); i++)
		{
			PushCommandRotation((Path[i] - Temp), FVector(1,0,0)); // Front
			PushCommandLocation(Path[i]);
			Temp = Path[i];
		}

		// Move toward objective for pre-final approach
		PushCommandRotation((TargetLocation - Temp), FVector(1,0,0));
		PushCommandLocation(TargetLocation);
		return true;
	}

	// Failed
	FLOG("AFlareSpacecraft::NavigateTo failed : no path found");
	return false;
}

FVector UFlareSpacecraftNavigationSystem::AnticollisionCorrection(FVector InitialVelocity,  AFlareSpacecraft* DockingStation) const
{
	AFlareSpacecraft* NearestShip = GetNearestShip(DockingStation);

	if(NearestShip)
	{
		FVector DeltaLocation = NearestShip->GetActorLocation() - Spacecraft->GetActorLocation();
		float Distance = FMath::Abs(DeltaLocation.Size() - NearestShip->GetMeshScale() *4) / 100.f; // Distance in meters




		if (Distance < 50.f)
		{

			FQuat AvoidQuat = FQuat(DeltaLocation.GetUnsafeNormal(), AnticollisionAngle);
			FVector Avoid =  AvoidQuat.RotateVector(FVector(0,0,NearestShip->GetMeshScale() *4. / 50. ));



			// Below 100m begin avoidance maneuver
			float Alpha = 1 - Distance/50.f;
			return InitialVelocity * (1.f - Alpha) + Alpha * ((Avoid - DeltaLocation) .GetUnsafeNormal() * Spacecraft->GetNavigationSystem()->GetLinearMaxVelocity());
		}
	}

	return InitialVelocity;
}

AFlareSpacecraft* UFlareSpacecraftNavigationSystem::GetNearestShip(AFlareSpacecraft* DockingStation) const
{
	// For now an host ship is a the nearest host ship with the following critera:
	// - Alive or not
	// - From any company
	// - Is the nearest
	// - Is not me

	FVector PilotLocation = Spacecraft->GetActorLocation();
	float MinDistanceSquared = -1;
	AFlareSpacecraft* NearestShip = NULL;

	for (TActorIterator<AActor> ActorItr(Spacecraft->GetWorld()); ActorItr; ++ActorItr)
	{
		// Ship
		AFlareSpacecraft* ShipCandidate = Cast<AFlareSpacecraft>(*ActorItr);
		if (ShipCandidate && ShipCandidate != Spacecraft && ShipCandidate != DockingStation)
		{

			if(DockingStation && (DockingStation->GetDockingSystem()->IsGrantedShip(ShipCandidate) || DockingStation->GetDockingSystem()->IsDockedShip(ShipCandidate)))
			{
				// Ignore ship docked or docking at the same station
				continue;
			}

			float DistanceSquared = (PilotLocation - ShipCandidate->GetActorLocation()).SizeSquared();

			if(NearestShip == NULL || DistanceSquared < MinDistanceSquared)
			{
				MinDistanceSquared = DistanceSquared;
				NearestShip = ShipCandidate;
			}
		}
	}
	return NearestShip;
}

/*----------------------------------------------------
	Attitude control : linear version
----------------------------------------------------*/


bool UFlareSpacecraftNavigationSystem::UpdateLinearAttitudeAuto(float DeltaSeconds, FVector TargetLocation, FVector TargetVelocity, float MaxVelocity)
{
	TArray<UActorComponent*> Engines = Spacecraft->GetComponentsByClass(UFlareEngine::StaticClass());

	FVector DeltaPosition = (TargetLocation - Spacecraft->GetActorLocation()) / 100; // Distance in meters
	FVector DeltaPositionDirection = DeltaPosition;
	DeltaPositionDirection.Normalize();
	float Distance = FMath::Max(0.0f, DeltaPosition.Size() - LinearDeadDistance);

	FVector DeltaVelocity = TargetVelocity - Spacecraft->GetLinearVelocity();
	FVector DeltaVelocityAxis = DeltaVelocity;
	DeltaVelocityAxis.Normalize();

	float TimeToFinalVelocity;

	if (FMath::IsNearlyZero(DeltaVelocity.SizeSquared()))
	{
		TimeToFinalVelocity = 0;
	}
	else
	{

		FVector Acceleration = GetTotalMaxThrustInAxis(Engines, DeltaVelocityAxis, false) / Spacecraft->Airframe->GetMass();
		float AccelerationInAngleAxis =  FMath::Abs(FVector::DotProduct(Acceleration, DeltaPositionDirection));

		TimeToFinalVelocity = (DeltaVelocity.Size() / AccelerationInAngleAxis);
	}

	float DistanceToStop = (DeltaVelocity.Size() / 2) * (TimeToFinalVelocity + DeltaSeconds);

	FVector RelativeResultSpeed;

	if (DistanceToStop > Distance)
	{
		RelativeResultSpeed = TargetVelocity;
	}
	else
	{

		float MaxPreciseSpeed = FMath::Min((Distance - DistanceToStop) / DeltaSeconds, MaxVelocity);

		RelativeResultSpeed = DeltaPositionDirection;
		RelativeResultSpeed *= MaxPreciseSpeed;
		RelativeResultSpeed += TargetVelocity;
		/*FLOGV("DeltaPositionDirection %s", *DeltaPositionDirection.ToString());
		FLOGV("MaxPreciseSpeed %f", MaxPreciseSpeed);
		FLOGV("TargetVelocity %s", *TargetVelocity.ToString());*/
	}

	// Under this distance we consider the variation negligible, and ensure null delta + null speed
	if (Distance < LinearDeadDistance && DeltaVelocity.Size() < NegligibleSpeedRatio * MaxVelocity)
	{
		LinearTargetVelocity = TargetVelocity;
		return true;
	}
	//FLOGV("RelativeResultSpeed %s", *RelativeResultSpeed.ToString());
	LinearTargetVelocity = RelativeResultSpeed;
	return false;
}

void UFlareSpacecraftNavigationSystem::UpdateLinearBraking(float DeltaSeconds)
{
	LinearTargetVelocity = FVector::ZeroVector;
	FVector LinearVelocity = Spacecraft->Airframe->GetPhysicsLinearVelocity();

	// Null speed detection
	if (LinearVelocity.Size() < NegligibleSpeedRatio * LinearMaxVelocity)
	{
		Spacecraft->Airframe->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
		ClearCurrentCommand();
	}
}


/*----------------------------------------------------
	Attitude control : angular version
----------------------------------------------------*/

void UFlareSpacecraftNavigationSystem::UpdateAngularAttitudeAuto(float DeltaSeconds)
{
	TArray<UActorComponent*> Engines = Spacecraft->GetComponentsByClass(UFlareEngine::StaticClass());

	// Rotation data
	FFlareShipCommandData Command;
	CommandData.Peek(Command);
	FVector TargetAxis = Command.RotationTarget;
	FVector LocalShipAxis = Command.LocalShipAxis;

	FVector AngularVelocity = Spacecraft->Airframe->GetPhysicsAngularVelocity();
	FVector WorldShipAxis = Spacecraft->Airframe->GetComponentToWorld().GetRotation().RotateVector(LocalShipAxis);

	WorldShipAxis.Normalize();
	TargetAxis.Normalize();

	FVector RotationDirection = FVector::CrossProduct(WorldShipAxis, TargetAxis);
	RotationDirection.Normalize();
	float Dot = FVector::DotProduct(WorldShipAxis, TargetAxis);
	float angle = FMath::RadiansToDegrees(FMath::Acos(Dot));

	FVector DeltaVelocity = -AngularVelocity;
	FVector DeltaVelocityAxis = DeltaVelocity;
	DeltaVelocityAxis.Normalize();

	float TimeToFinalVelocity;

	if (FMath::IsNearlyZero(DeltaVelocity.SizeSquared()))
	{
		TimeToFinalVelocity = 0;
	}
	else {
		FVector SimpleAcceleration = DeltaVelocityAxis * AngularAccelerationRate;
		// Scale with damages
		float DamageRatio = GetTotalMaxTorqueInAxis(Engines, DeltaVelocityAxis, true) / GetTotalMaxTorqueInAxis(Engines, DeltaVelocityAxis, false);
		FVector DamagedSimpleAcceleration = SimpleAcceleration * DamageRatio;

		FVector Acceleration = DamagedSimpleAcceleration;
		float AccelerationInAngleAxis =  FMath::Abs(FVector::DotProduct(DamagedSimpleAcceleration, RotationDirection));

		TimeToFinalVelocity = (DeltaVelocity.Size() / AccelerationInAngleAxis);
	}

	float AngleToStop = (DeltaVelocity.Size() / 2) * (TimeToFinalVelocity + DeltaSeconds);

	FVector RelativeResultSpeed;

	if (AngleToStop > angle) {
		RelativeResultSpeed = FVector::ZeroVector;
	}
	else {

		float MaxPreciseSpeed = FMath::Min((angle - AngleToStop) / DeltaSeconds, AngularMaxVelocity);

		RelativeResultSpeed = RotationDirection;
		RelativeResultSpeed *= MaxPreciseSpeed;
	}

	// Under this angle we consider the variation negligible, and ensure null delta + null speed
	if (angle < AngularDeadAngle && DeltaVelocity.Size() < AngularDeadAngle)
	{
		Spacecraft->Airframe->SetPhysicsAngularVelocity(FVector::ZeroVector, false); // TODO remove
		ClearCurrentCommand();
		RelativeResultSpeed = FVector::ZeroVector;
	}
	AngularTargetVelocity = RelativeResultSpeed;
}


FVector UFlareSpacecraftNavigationSystem::GetAngularVelocityToAlignAxis(FVector LocalShipAxis, FVector TargetAxis, FVector TargetAngularVelocity, float DeltaSeconds) const
{
	TArray<UActorComponent*> Engines = Spacecraft->GetComponentsByClass(UFlareEngine::StaticClass());

	FVector AngularVelocity = Spacecraft->Airframe->GetPhysicsAngularVelocity();
	FVector WorldShipAxis = Spacecraft->Airframe->GetComponentToWorld().GetRotation().RotateVector(LocalShipAxis);

	WorldShipAxis.Normalize();
	TargetAxis.Normalize();

	FVector RotationDirection = FVector::CrossProduct(WorldShipAxis, TargetAxis);
	RotationDirection.Normalize();
	float Dot = FVector::DotProduct(WorldShipAxis, TargetAxis);
	float angle = FMath::RadiansToDegrees(FMath::Acos(Dot));

	FVector DeltaVelocity = TargetAngularVelocity - AngularVelocity;
	FVector DeltaVelocityAxis = DeltaVelocity;
	DeltaVelocityAxis.Normalize();

	float TimeToFinalVelocity;

	if (FMath::IsNearlyZero(DeltaVelocity.SizeSquared()))
	{
		TimeToFinalVelocity = 0;
	}
	else {
		FVector SimpleAcceleration = DeltaVelocityAxis * GetAngularAccelerationRate();
		// Scale with damages
		float DamageRatio = GetTotalMaxTorqueInAxis(Engines, DeltaVelocityAxis, true) / GetTotalMaxTorqueInAxis(Engines, DeltaVelocityAxis, false);
		FVector DamagedSimpleAcceleration = SimpleAcceleration * DamageRatio;

		FVector Acceleration = DamagedSimpleAcceleration;
		float AccelerationInAngleAxis =  FMath::Abs(FVector::DotProduct(DamagedSimpleAcceleration, RotationDirection));

		TimeToFinalVelocity = (DeltaVelocity.Size() / AccelerationInAngleAxis);
	}

	float AngleToStop = (DeltaVelocity.Size() / 2) * (FMath::Max(TimeToFinalVelocity,DeltaSeconds));

	FVector RelativeResultSpeed;

	if (AngleToStop > angle) {
		RelativeResultSpeed = TargetAngularVelocity;
	}
	else
	{
		float MaxPreciseSpeed = FMath::Min((angle - AngleToStop) / (DeltaSeconds * 0.75f), GetAngularMaxVelocity());

		RelativeResultSpeed = RotationDirection;
		RelativeResultSpeed *= MaxPreciseSpeed;
	}

	return RelativeResultSpeed;
}

void UFlareSpacecraftNavigationSystem::UpdateAngularBraking(float DeltaSeconds)
{
	AngularTargetVelocity = FVector::ZeroVector;
	FVector AngularVelocity = Spacecraft->Airframe->GetPhysicsAngularVelocity();
	// Null speed detection
	if (AngularVelocity.Size() < NegligibleSpeedRatio * AngularMaxVelocity)
	{
		AngularTargetVelocity = FVector::ZeroVector;
		Spacecraft->Airframe->SetPhysicsAngularVelocity(FVector::ZeroVector, false); // TODO remove
		ClearCurrentCommand();
	}
}


/*----------------------------------------------------
	Physics
----------------------------------------------------*/

void UFlareSpacecraftNavigationSystem::PhysicSubTick(float DeltaSeconds)
{
	TArray<UActorComponent*> Engines = Spacecraft->GetComponentsByClass(UFlareEngine::StaticClass());
	if (Spacecraft->GetDamageSystem()->IsPowered())
	{
		// Clamp speed
		float MaxVelocity = LinearMaxVelocity;
		if (UseOrbitalBoost)
		{
			FVector FrontDirection = Spacecraft->Airframe->GetComponentToWorld().GetRotation().RotateVector(FVector(1,0,0));
			MaxVelocity = FVector::DotProduct(LinearTargetVelocity.GetUnsafeNormal(), FrontDirection) * GetLinearMaxBoostingVelocity();
		}
		LinearTargetVelocity = LinearTargetVelocity.GetClampedToMaxSize(MaxVelocity);

		// Linear physics
		FVector DeltaV = LinearTargetVelocity - Spacecraft->GetLinearVelocity();
		FVector DeltaVAxis = DeltaV;
		DeltaVAxis.Normalize();

		if (!DeltaV.IsNearlyZero())
		{
			FVector Acceleration = DeltaVAxis * GetTotalMaxThrustInAxis(Engines, -DeltaVAxis, UseOrbitalBoost).Size() / Spacecraft->Airframe->GetMass();
			FVector ClampedAcceleration = Acceleration.GetClampedToMaxSize(DeltaV.Size() / DeltaSeconds);

			Spacecraft->Airframe->SetPhysicsLinearVelocity(ClampedAcceleration * DeltaSeconds * 100, true); // Multiply by 100 because UE4 works in cm
		}

		// Angular physics
		FVector DeltaAngularV = AngularTargetVelocity - Spacecraft->Airframe->GetPhysicsAngularVelocity();
		FVector DeltaAngularVAxis = DeltaAngularV;
		DeltaAngularVAxis.Normalize();

		if (!DeltaAngularV.IsNearlyZero())
		{
			FVector SimpleAcceleration = DeltaAngularVAxis * AngularAccelerationRate;

			// Scale with damages
			float DamageRatio = GetTotalMaxTorqueInAxis(Engines, DeltaAngularVAxis, true) / GetTotalMaxTorqueInAxis(Engines, DeltaAngularVAxis, false);
			FVector DamagedSimpleAcceleration = SimpleAcceleration * DamageRatio;
			FVector ClampedSimplifiedAcceleration = DamagedSimpleAcceleration.GetClampedToMaxSize(DeltaAngularV.Size() / DeltaSeconds);

			Spacecraft->Airframe->SetPhysicsAngularVelocity(ClampedSimplifiedAcceleration  * DeltaSeconds, true);
		}

		// Update engine alpha
		for (int32 EngineIndex = 0; EngineIndex < Engines.Num(); EngineIndex++)
		{
			UFlareEngine* Engine = Cast<UFlareEngine>(Engines[EngineIndex]);
			FVector ThrustAxis = Engine->GetThrustAxis();
			float LinearAlpha = 0;
			float AngularAlpha = 0;

			if (Spacecraft->IsPresentationMode()) {
				LinearAlpha = true;
			} else if (!DeltaV.IsNearlyZero()) {
				if (!(!UseOrbitalBoost && Engine->IsA(UFlareOrbitalEngine::StaticClass()))) {
					LinearAlpha = -FVector::DotProduct(ThrustAxis, DeltaVAxis);
				}
			}

			FVector EngineOffset = (Engine->GetComponentLocation() - COM) / 100;
			FVector TorqueDirection = FVector::CrossProduct(EngineOffset, ThrustAxis);
			TorqueDirection.Normalize();

			if (!DeltaAngularV.IsNearlyZero() && !Engine->IsA(UFlareOrbitalEngine::StaticClass())) {
				AngularAlpha = -FVector::DotProduct(TorqueDirection, DeltaAngularVAxis);
			}

			Engine->SetAlpha(FMath::Clamp(LinearAlpha + AngularAlpha, 0.0f, 1.0f));
		}
	}
	else
	{
		// Shutdown engines
		for (int32 EngineIndex = 0; EngineIndex < Engines.Num(); EngineIndex++)
		{
			UFlareEngine* Engine = Cast<UFlareEngine>(Engines[EngineIndex]);
			Engine->SetAlpha(0);
		}
	}
}

void UFlareSpacecraftNavigationSystem::UpdateCOM()
{
	COM = Spacecraft->Airframe->GetBodyInstance()->GetCOMPosition();
}


/*----------------------------------------------------
		Getters (Attitude)
----------------------------------------------------*/

FVector UFlareSpacecraftNavigationSystem::GetTotalMaxThrustInAxis(TArray<UActorComponent*>& Engines, FVector Axis, bool WithOrbitalEngines) const
{
	Axis.Normalize();
	FVector TotalMaxThrust = FVector::ZeroVector;
	for (int32 i = 0; i < Engines.Num(); i++)
	{
		UFlareEngine* Engine = Cast<UFlareEngine>(Engines[i]);

		if (Engine->IsA(UFlareOrbitalEngine::StaticClass()) && !WithOrbitalEngines)
		{
			continue;
		}

		FVector WorldThrustAxis = Engine->GetThrustAxis();

		float Ratio = FVector::DotProduct(WorldThrustAxis, Axis);
		if (Ratio > 0)
		{
			TotalMaxThrust += WorldThrustAxis * Engine->GetMaxThrust() * Ratio;
		}
	}

	return TotalMaxThrust;
}

float UFlareSpacecraftNavigationSystem::GetTotalMaxTorqueInAxis(TArray<UActorComponent*>& Engines, FVector TorqueAxis, bool WithDamages) const
{
	TorqueAxis.Normalize();
	float TotalMaxTorque = 0;

	for (int32 i = 0; i < Engines.Num(); i++) {
		UFlareEngine* Engine = Cast<UFlareEngine>(Engines[i]);

		// Ignore orbital engines for torque computation
		if (Engine->IsA(UFlareOrbitalEngine::StaticClass())) {
		  continue;
		}

		float MaxThrust = (WithDamages ? Engine->GetMaxThrust() : Engine->GetInitialMaxThrust());

		if (MaxThrust == 0)
		{
			// Not controlable engine
			continue;
		}

		FVector EngineOffset = (Engine->GetComponentLocation() - COM) / 100;

		FVector WorldThrustAxis = Engine->GetThrustAxis();
		WorldThrustAxis.Normalize();
		FVector TorqueDirection = FVector::CrossProduct(EngineOffset, WorldThrustAxis);
		TorqueDirection.Normalize();

		float ratio = FVector::DotProduct(TorqueAxis, TorqueDirection);

		if (ratio > 0) {
			TotalMaxTorque += FVector::CrossProduct(EngineOffset, WorldThrustAxis).Size() * MaxThrust * ratio;
		}

	}

	return TotalMaxTorque;
}




#undef LOCTEXT_NAMESPACE

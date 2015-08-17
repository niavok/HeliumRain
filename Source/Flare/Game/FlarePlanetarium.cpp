
#include "../Flare.h"
#include "FlarePlanetarium.h"
#include "FlareGame.h"


/*----------------------------------------------------
	Constructor
----------------------------------------------------*/

AFlarePlanetarium::AFlarePlanetarium(const class FObjectInitializer& PCIP)
	: Super(PCIP)
{
	PrimaryActorTick.bCanEverTick = true;
	CurrentTime = -1;
}

void AFlarePlanetarium::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	AFlareGame* Game =  Cast<AFlareGame>(GetWorld()->GetAuthGameMode());

	if(Game)
	{
		if(!Game->GetActiveSector())
		{
			// No active sector, do nothing
			return;
		}

		UFlareWorld* World = Game->GetGameWorld();

		if (World)
		{
			if(CurrentTime == World->GetTime() && CurrentSector == Game->GetActiveSector()->GetIdentifier())
			{
				// Already up-to-date
				return;
			}

			CurrentTime = World->GetTime();
			CurrentSector = Game->GetActiveSector()->GetIdentifier();

			if (Sky == NULL)
			{
				for (TActorIterator<AActor> ActorItr(GetWorld()); ActorItr; ++ActorItr)
				{
					FLOGV("Is Sky ? %s", *(*ActorItr)->GetName());
					if ((*ActorItr)->GetName().StartsWith("SM_Sky"))
					{
						FLOG("Sky found");
						Sky = Cast<AStaticMeshActor>(*ActorItr);
						break;
					}
				}
			}

			if (Light == NULL)
			{
				TArray<UActorComponent*> Components = GetComponentsByClass(UDirectionalLightComponent::StaticClass());
				for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ComponentIndex++)
				{
					UDirectionalLightComponent* LightCandidate = Cast<UDirectionalLightComponent>(Components[ComponentIndex]);
					if(LightCandidate)
					{
						Light = LightCandidate;
						break;
					}
				}
			}

			Sun = World->GetPlanerarium()->GetSnapShot(World->GetTime());

			// Draw Player
			FFlareSectorOrbitParameters* PlayerOrbit = Game->GetActiveSector()->GetSimulatedSector()->GetOrbitParameters();


			FFlareCelestialBody* CurrentParent = World->GetPlanerarium()->FindCelestialBody(PlayerOrbit->CelestialBodyIdentifier);
			if (CurrentParent)
			{
				FPreciseVector ParentLocation = CurrentParent->AbsoluteLocation;
				FPreciseVector PlayerLocation =  ParentLocation + World->GetPlanerarium()->GetRelativeLocation(CurrentParent, World->GetTime(), PlayerOrbit->Altitude + CurrentParent->Radius, 0, PlayerOrbit->Phase);
				/*FLOGV("Parent location = %s", *CurrentParent->AbsoluteLocation.ToString());
				FLOGV("PlayerLocation = %s", *PlayerLocation.ToString());*/
	/*			DrawDebugLine(GetWorld(), FVector(1000, 0 ,0), FVector(- 1000, 0 ,0), FColor::Red, false);
				DrawDebugLine(GetWorld(), FVector(0, 1000 ,0), FVector(0,- 1000 ,0), FColor::Green, false);
				DrawDebugLine(GetWorld(), FVector(0, 0, 900), FVector(0, 0, -1000), FColor::Blue, false);
				DrawDebugLine(GetWorld(), FVector(0, 0, 900), FVector(0, 0, 1000), FColor::Cyan, false);
*/
				FPreciseVector DeltaLocation = ParentLocation - PlayerLocation;
				FPreciseVector SunDeltaLocation = Sun.AbsoluteLocation - PlayerLocation;

				float AngleOffset =  90 + FMath::RadiansToDegrees(FMath::Atan2(DeltaLocation.Z,DeltaLocation.X));
				/*FLOGV("DeltaLocation = %s", *DeltaLocation.ToString());
				FLOGV("FMath::Atan2(DeltaLocation.Y,DeltaLocation.X)  = %f", FMath::Atan2(DeltaLocation.Z,DeltaLocation.X));
				FLOGV("AngleOffset  = %f", AngleOffset);*/

				FPreciseVector SunDirection = -(SunDeltaLocation.RotateAngleAxis(AngleOffset, FPreciseVector(0,1,0))).GetUnsafeNormal();

				// Reset sun occlusion;
				SunOcclusion = 0;

				MoveCelestialBody(&Sun, -PlayerLocation, AngleOffset, SunDirection);

				if (Sky)
				{
					Sky->SetActorRotation(FRotator(-AngleOffset, 0 , 0));
					//FLOGV("Sky %s rotation= %s",*Sky->GetName(),  *Sky->GetActorRotation().ToString());
				}
				else
				{
					FLOG("Error: No sky found");
				}

				//FLOGV("SunOcclusion %f", SunOcclusion);
				if (Light)
				{
					float Intensity = 10 * FMath::Pow((1.0 - SunOcclusion), 2);
					//FLOGV("Light Intensity %f", Intensity);
					Light->SetIntensity(Intensity);
				}
				else
				{

					FLOG("Error: No sun light found");
				}


			}
			else
			{
				FLOGV("Error: Failed to find the current sector: '%s' in planetarium", *(PlayerOrbit->CelestialBodyIdentifier.ToString()));
			}
		}
	}
}

void AFlarePlanetarium::MoveCelestialBody(FFlareCelestialBody* Body, FPreciseVector Offset, double AngleOffset, FPreciseVector SunDirection)
{

	//float BaseDistance = 1e9;
	double BaseDistance = 1e7;



	double DistanceScaleRatio = 100./10000;
	//float RadiusScaleRatio = 100./70000;
	double RadiusScaleRatio = 100./10000;
	FPreciseVector Location = Offset + Body->AbsoluteLocation;
	FPreciseVector AlignedLocation = Location.RotateAngleAxis(AngleOffset, FPreciseVector(0,1,0));


	double AngularRadius = FMath::Asin(Body->Radius / AlignedLocation.Size());

	double DisplayDistance = BaseDistance + AlignedLocation.Size() / 100;


	double VisibleRadius = FMath::Sin(AngularRadius) * DisplayDistance;


	/*FLOGV("MoveCelestialBody %s VisibleRadius = %f", *Body->Name, VisibleRadius);
	FLOGV("MoveCelestialBody %s AngularRadius = %f", *Body->Name, AngularRadius);
	FLOGV("MoveCelestialBody %s DisplayDistance = %f", *Body->Name, DisplayDistance);


	FLOGV("MoveCelestialBody %s Location = %s", *Body->Name, *Location.ToString());
	FLOGV("MoveCelestialBody %s AlignedLocation = %s", *Body->Name, *AlignedLocation.ToString());*/

	// Find the celestial body component
	TArray<UActorComponent*> Components = GetComponents();
	UStaticMeshComponent* BodyComponent = NULL;
	for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ComponentIndex++)
	{
		UStaticMeshComponent* ComponentCandidate = Cast<UStaticMeshComponent>(Components[ComponentIndex]);
		if(ComponentCandidate && ComponentCandidate->GetName() == Body->Identifier.ToString()	)
		{
			BodyComponent = ComponentCandidate;
			break;
		}
	}

	if (BodyComponent)
	{
		BodyComponent->SetRelativeLocation((DisplayDistance * AlignedLocation.GetUnsafeNormal()).ToVector());
		float Scale = VisibleRadius / 512; // Mesh size is 1024;
		BodyComponent->SetRelativeScale3D(FPreciseVector(Scale).ToVector());

		//BodyComponent->SetRelativeRotation(FRotator(90, Body->RotationAngle + AngleOffset ,0));
		//BodyComponent->SetRelativeRotation(FRotator(0, -90 ,0));

		/*FLOGV("MoveCelestialBody %s Body->RotationAngle = %f", *Body->Name, Body->RotationAngle);
		FLOGV("MoveCelestialBody %s AngleOffset = %f", *Body->Name, AngleOffset);
		FLOGV("MoveCelestialBody %s Body->RotationAngle + AngleOffset = %f", *Body->Name, (Body->RotationAngle + AngleOffset));*/

		FTransform BaseRotation = FTransform(FRotator(0, 0 ,90));
		FTransform TimeRotation = FTransform(FRotator(0, Body->RotationAngle + AngleOffset, 0));

		FRotator Rotation = (TimeRotation * BaseRotation).Rotator();
		//FLOGV("MoveCelestialBody %s Rotation = %s", *Body->Name, *Rotation.ToString());
		BodyComponent->SetRelativeRotation(Rotation);

		UMaterialInstanceDynamic* ComponentMaterial = Cast<UMaterialInstanceDynamic>(BodyComponent->GetMaterial(0));
		if(!ComponentMaterial)
		{
			ComponentMaterial = UMaterialInstanceDynamic::Create(BodyComponent->GetMaterial(0) , GetWorld());
			BodyComponent->SetMaterial(0, ComponentMaterial);
		}
		ComponentMaterial->SetVectorParameterValue("SunDirection", SunDirection.ToVector());
	}
	else
	{
		FLOGV("ERROR: No planetarium component for '%s' celestial body", *(Body->Identifier.ToString()));
	}

	/*DrawDebugLine(GetWorld(), FVector(0, 0, 0), AlignedLocation * 100000, FColor::Blue, false, 1.f);
	DrawDebugLine(GetWorld(), FVector(0, 0, 0), AlignedLocation.RotateAngleAxis(FMath::RadiansToDegrees(AngularRadius), FVector(0,1,0)) * 100000, FColor::Red, false, 1.f);
	DrawDebugLine(GetWorld(), FVector(0, 0, 0), AlignedLocation.RotateAngleAxis(-FMath::RadiansToDegrees(AngularRadius), FVector(0,1,0)) * 100000, FColor::Green, false, 1.f);
*/
	// Compute sun occlusion
	if(Body != &Sun)
	{
		float BodyPhase =  FMath::UnwindRadians(FMath::Atan2(AlignedLocation.Z, AlignedLocation.X));



		float CenterAngularDistance = FMath::Abs(FMath::UnwindRadians(SunPhase - BodyPhase));
		float AngleSum = (SunAnglularRadius + AngularRadius);
		float AngleDiff = FMath::Abs(SunAnglularRadius - AngularRadius);

		if(CenterAngularDistance < AngleSum)
		{
			// There is occlusion
			float OcclusionRatio;

			if(CenterAngularDistance < AngleDiff)
			{
				// Maximum occlusion
				OcclusionRatio = 1.0;
			}
			else
			{
				// Partial occlusion
				OcclusionRatio = (AngleSum - CenterAngularDistance) / (2* FMath::Min(SunAnglularRadius, AngularRadius));

				//OcclusionRatio = ((SunAnglularRadius + AngularRadius) + FMath::Max(SunAnglularRadius, AngularRadius) - FMath::Min(SunAnglularRadius, AngularRadius)) / (2 * CenterAngularDistance);
			}
			//FLOGV("MoveCelestialBody %s OcclusionRatio = %f", *Body->Name, OcclusionRatio);

			//Now, find the surface occlusion
			float SunAngularSurface = PI*FMath::Square(SunAnglularRadius);
			float MaxOcclusionAngularSurface = PI*FMath::Square(FMath::Min(SunAnglularRadius, AngularRadius));
			float MaxOcclusion = MaxOcclusionAngularSurface/SunAngularSurface;
			float Occlusion = OcclusionRatio * MaxOcclusion;


			//FLOGV("MoveCelestialBody %s OcclusionRatioSmooth = %f", *Body->Name, OcclusionRatioSmooth);
			/*FLOGV("MoveCelestialBody %s CenterAngularDistance = %f", *Body->Name, CenterAngularDistance);
			FLOGV("MoveCelestialBody %s SunAnglularRadius = %f", *Body->Name, SunAnglularRadius);
			FLOGV("MoveCelestialBody %s AngularRadius = %f", *Body->Name, AngularRadius);
			FLOGV("MoveCelestialBody %s SunAngularSurface = %f", *Body->Name, SunAngularSurface);
			FLOGV("MoveCelestialBody %s MaxOcclusionAngularSurface = %f", *Body->Name, MaxOcclusionAngularSurface);
			FLOGV("MoveCelestialBody %s MaxOcclusion = %f", *Body->Name, MaxOcclusion);

			FLOGV("MoveCelestialBody %s Occlusion = %f", *Body->Name, Occlusion);*/


			if(Occlusion > SunOcclusion)
			{
				// Keep only best occlusion
				SunOcclusion = Occlusion;
			}

		}
	}
	else
	{
		SunAnglularRadius = AngularRadius;
		SunPhase = FMath::UnwindRadians(FMath::Atan2(AlignedLocation.Z, AlignedLocation.X));
	}

	for(int SatteliteIndex = 0; SatteliteIndex < Body->Sattelites.Num(); SatteliteIndex++)
	{
		FFlareCelestialBody* CelestialBody = &Body->Sattelites[SatteliteIndex];
		MoveCelestialBody(CelestialBody, Offset, AngleOffset, SunDirection);
	}
}



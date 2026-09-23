#include "Body.h"

#include <Ext/Techno/Body.h>

// Contains ScriptExt::Mission_Move and its helper functions.

void ScriptExt::Mission_Move(TeamClass* pTeam, int calcThreatMode, bool pickAllies, int attackAITargetType, int idxAITargetTypeItem)
{
	bool noWaitLoop = false;
	bool bAircraftsWithoutAmmo = false;
	const auto pTeamData = TeamExt::ExtMap.Find(pTeam);

	// When the new target wasn't found it sleeps some few frames before the new attempt. This can save cycles and cycles of unnecessary executed lines.
	if (pTeamData->WaitNoTargetCounter > 0)
	{
		if (pTeamData->WaitNoTargetTimer.InProgress())
			return;

		pTeamData->WaitNoTargetTimer.Stop();
		noWaitLoop = true;
		pTeamData->WaitNoTargetCounter = 0;

		if (pTeamData->WaitNoTargetAttempts > 0)
			pTeamData->WaitNoTargetAttempts--;
	}

	for (auto pFoot = pTeam->FirstUnit; pFoot; pFoot = pFoot->NextTeamMember)
	{
		if (pFoot && pFoot->IsAlive && !pFoot->InLimbo)
		{
			const auto pTechnoType = pFoot->GetTechnoType();

			if (pFoot->WhatAmI() == AbstractType::Aircraft
				&& !pFoot->IsInAir()
				&& static_cast<AircraftTypeClass*>(pTechnoType)->AirportBound
				&& pFoot->Ammo < pTechnoType->Ammo)
			{
				bAircraftsWithoutAmmo = true;
			}
		}
	}

	// Find the Leader
	auto pLeaderUnit = pTeamData->TeamLeader;

	if (!ScriptExt::IsUnitAvailable(pLeaderUnit, true))
	{
		pLeaderUnit = ScriptExt::FindTheTeamLeader(pTeam);
		pTeamData->TeamLeader = pLeaderUnit;
	}

	const auto pScript = pTeam->CurrentScript;
	const auto pScriptType = pScript->Type;

	if (!pLeaderUnit || bAircraftsWithoutAmmo)
	{
		pTeamData->IdxSelectedObjectFromAIList = -1;

		if (pTeamData->CloseEnough > 0)
			pTeamData->CloseEnough = -1;

		if (pTeamData->WaitNoTargetAttempts != 0)
		{
			pTeamData->WaitNoTargetTimer.Stop();
			pTeamData->WaitNoTargetCounter = 0;
			pTeamData->WaitNoTargetAttempts = 0;
		}

		// This action finished
		pTeam->StepCompleted = true;

		const auto& node = pScriptType->ScriptActions[pScript->CurrentMission];
		const int nextMission = pScript->CurrentMission + 1;
		const auto& nextNode = pScriptType->ScriptActions[nextMission];
		ScriptExt::Log("AI Scripts - Move: [%s] [%s] (line: %d = %d,%d) Jump to next line: %d = %d,%d -> (Reasons: No Leader | Aircrafts without ammo)\n",
			pTeam->Type->ID,
			pScriptType->ID,
			pScript->CurrentMission,
			node.Action,
			node.Argument,
			nextMission,
			nextNode.Action,
			nextNode.Argument);

		return;
	}

	const auto pLeaderUnitType = pLeaderUnit->GetTechnoType();
	const auto pFocus = abstract_cast<TechnoClass*>(pTeam->Focus);

	if (!pFocus && !bAircraftsWithoutAmmo)
	{
		// This part of the code is used for picking a new target.
		const auto& node = pScriptType->ScriptActions[pScript->CurrentMission];
		const int targetMask = node.Argument; // This is the target type
		const auto pSelectedTarget = ScriptExt::FindBestObject(pLeaderUnit, targetMask, calcThreatMode, pickAllies, attackAITargetType, idxAITargetTypeItem);

		if (pSelectedTarget)
		{
			ScriptExt::Log("AI Scripts - Move: [%s] [%s] (line: %d = %d,%d) Leader [%s] (UID: %lu) selected [%s] (UID: %lu) as destination target.\n",
				pTeam->Type->ID,
				pScriptType->ID,
				pScript->CurrentMission,
				node.Action,
				node.Argument,
				pLeaderUnitType->get_ID(),
				pLeaderUnit->UniqueID,
				pSelectedTarget->GetTechnoType()->get_ID(),
				pSelectedTarget->UniqueID);

			pTeam->Focus = pSelectedTarget;
			pTeamData->WaitNoTargetAttempts = 0; // Disable Script Waits if there are any because a new target was selected
			pTeamData->WaitNoTargetTimer.Stop();
			pTeamData->WaitNoTargetCounter = 0; // Disable Script Waits if there are any because a new target was selected

			for (auto pFoot = pTeam->FirstUnit; pFoot; pFoot = pFoot->NextTeamMember)
			{
				if (!pFoot)
					continue;

				const auto pTechnoType = pFoot->GetTechnoType();

				if (ScriptExt::IsUnitAvailable(pFoot, true))
				{
					if (pTechnoType->Underwater && pTechnoType->LandTargeting == LandTargetingType::Land_Not_OK && pSelectedTarget->GetCell()->LandType != LandType::Water) // Land not OK for the Naval unit
					{
						// Naval units like Submarines are unable to target ground targets except if they have anti-ground weapons. Ignore the attack
						pFoot->SetTarget(nullptr);
						pFoot->SetDestination(nullptr, false);
						pFoot->QueueMission(Mission::Area_Guard, true);

						continue;
					}

					// Reset previous command
					pFoot->SetTarget(nullptr);
					pFoot->SetDestination(nullptr, false);
					pFoot->ForceMission(Mission::Guard);

					// Get a cell near the target
					pFoot->QueueMission(Mission::Move, false);
					CoordStruct coord = TechnoExt::PassengerKickOutLocation(pSelectedTarget, pFoot, 10);
					coord = coord != CoordStruct::Empty ? coord : pSelectedTarget->Location;
					CellClass* pCellDestination = MapClass::Instance.TryGetCellAt(coord);
					pFoot->SetDestination(pCellDestination, true);

					// Aircraft hack. I hate how this game auto-manages the aircraft missions.
					if (pFoot->WhatAmI() == AbstractType::Aircraft && pFoot->Ammo > 0 && !pFoot->IsInAir())
						pFoot->QueueMission(Mission::Move, false);
				}
			}
		}
		else
		{
			// No target was found with the specific criteria.

			if (!noWaitLoop && pTeamData->WaitNoTargetTimer.Completed())
			{
				pTeamData->WaitNoTargetCounter = 30;
				pTeamData->WaitNoTargetTimer.Start(30);
			}

			if (pTeamData->IdxSelectedObjectFromAIList >= 0)
				pTeamData->IdxSelectedObjectFromAIList = -1;

			if (pTeamData->WaitNoTargetAttempts != 0 && pTeamData->WaitNoTargetTimer.Completed())
			{
				pTeamData->WaitNoTargetCounter = 30;
				pTeamData->WaitNoTargetTimer.Start(30); // No target? let's wait some frames

				return;
			}

			if (pTeamData->CloseEnough >= 0)
				pTeamData->CloseEnough = -1;

			// This action finished
			pTeam->StepCompleted = true;

			const int nextMission = pScript->CurrentMission + 1;
			const auto& nextNode = pScriptType->ScriptActions[nextMission];
			ScriptExt::Log("AI Scripts - Move: [%s] [%s] (line: %d = %d,%d) Jump to next line: %d = %d,%d (new target NOT FOUND)\n",
				pTeam->Type->ID,
				pScriptType->ID,
				pScript->CurrentMission,
				node.Action,
				node.Argument,
				nextMission,
				nextNode.Action,
				nextNode.Argument);

			return;
		}
	}
	else
	{
		// This part of the code is used for updating the "Move" mission in each team unit
		if (ScriptExt::MoveMissionEndStatus(pTeam, pFocus, pLeaderUnit, pTeamData->MoveMissionEndMode))
		{
			pTeamData->MoveMissionEndMode = 0;
			pTeamData->IdxSelectedObjectFromAIList = -1;

			if (pTeamData->CloseEnough >= 0)
				pTeamData->CloseEnough = -1;

			// This action finished
			pTeam->StepCompleted = true;

			const auto& node = pScriptType->ScriptActions[pScript->CurrentMission];
			const int nextMission = pScript->CurrentMission + 1;
			const auto& nextNode = pScriptType->ScriptActions[nextMission];
			ScriptExt::Log("AI Scripts - Move: [%s] [%s] (line: %d = %d,%d) Jump to next line: %d = %d,%d (Reason: Reached destination)\n",
				pTeam->Type->ID,
				pScriptType->ID,
				pScript->CurrentMission,
				node.Action,
				node.Argument,
				nextMission,
				nextNode.Action,
				nextNode.Argument);

			return;
		}
	}
}

TechnoClass* ScriptExt::FindBestObject(TechnoClass* pTechno, int method, int calcThreatMode, bool pickAllies, int attackAITargetType, int idxAITargetTypeItem)
{
	TechnoClass* pBestObject = nullptr;
	double bestVal = -1;
	HouseClass* pEnemyHouse = nullptr;
	const auto pTechnoType = pTechno->GetTechnoType();

	// Favorite Enemy House case. If set, AI will focus against that House
	if (!pickAllies && pTechno->BelongsToATeam())
	{
		if (const auto pFoot = abstract_cast<FootClass*>(pTechno))
		{
			const auto pTeam = pFoot->Team;
			const int enemyHouseIndex = pTeam->FirstUnit->Owner->EnemyHouseIndex;

			if (pTeam->Type->OnlyTargetHouseEnemy && enemyHouseIndex >= 0)
				pEnemyHouse = HouseClass::Array.GetItem(enemyHouseIndex);
		}
	}

	// Generic method for targeting
	for (int i = 0; i < TechnoClass::Array.Count; i++)
	{
		const auto pTarget = TechnoClass::Array.GetItem(i);

		if (pickAllies != pTechno->Owner->IsAlliedWith(pTarget->Owner))
			continue;

		if (pEnemyHouse && pEnemyHouse != pTarget->Owner)
			continue;

		// Exclude most of invalid target first
		if (!ScriptExt::EvaluateObjectWithMask(pTarget, method, attackAITargetType, idxAITargetTypeItem, pTechno))
			continue;

		const auto pTargetType = pTarget->GetTechnoType();

		// Discard invisible structures
		const auto pTargetBuildingType = abstract_cast<BuildingTypeClass*, true>(pTargetType);

		if (pTargetBuildingType && pTargetBuildingType->InvisibleInGame)
			continue;

		if (pTarget == pTechno)
			continue;

		if (pTargetType->Naval)
		{
			// Submarines aren't a valid target
			if (pTarget->CloakState == CloakState::Cloaked
				&& pTargetType->Underwater)
			{
				const auto navalTargeting = pTechnoType->NavalTargeting;

				if (navalTargeting == NavalTargetingType::Underwater_Never
					|| navalTargeting == NavalTargetingType::Naval_None)
				{
					continue;
				}
			}

			// Land not OK for the Naval unit
			if (pTechnoType->LandTargeting == LandTargetingType::Land_Not_OK
				&& (pTarget->GetCell()->LandType != LandType::Water))
			{
				continue;
			}
		}

		// Stealth check.
		if (pTarget->CloakState == CloakState::Cloaked)
		{
			const auto pCell = pTarget->GetCell();

			if (!pCell->Sensors_InclHouse(pTechno->Owner->ArrayIndex))
				continue;
		}

		if (!ScriptExt::IsUnitAvailable(pTarget, true))
			continue;

		double value = 0;
		bool isGoodTarget = false;

		switch (calcThreatMode)
		{
		case 0:
		case 1:
		{
			// Threat affected by distance
			double threatMultiplier = 128.0;
			double objectThreatValue = pTargetType->ThreatPosed;

			if (pTargetType->SpecialThreatValue > 0)
			{
				double const& TargetSpecialThreatCoefficientDefault = RulesClass::Instance->TargetSpecialThreatCoefficientDefault;
				objectThreatValue += pTargetType->SpecialThreatValue * TargetSpecialThreatCoefficientDefault;
			}

			// Is Defender house targeting Attacker House? if "yes" then more Threat
			if (pTechno->Owner == HouseClass::Array.GetItem(pTarget->Owner->EnemyHouseIndex))
			{
				double const& EnemyHouseThreatBonus = RulesClass::Instance->EnemyHouseThreatBonus;
				objectThreatValue += EnemyHouseThreatBonus;
			}

			// Extra threat based on current health. More damaged == More threat (almost destroyed objects gets more priority)
			objectThreatValue += pTarget->Health * (1 - pTarget->GetHealthPercentage());
			value = (objectThreatValue * threatMultiplier) / ((pTechno->DistanceFrom(pTarget) / (double)Unsorted::LeptonsPerCell) + 1.0);

			if (calcThreatMode == 0)
			{
				// Is this object very FAR? then LESS THREAT against pTechno.
				// More CLOSER? MORE THREAT for pTechno.
				if (value > bestVal || bestVal < 0)
					isGoodTarget = true;
			}
			else
			{
				// Is this object very FAR? then MORE THREAT against pTechno.
				// More CLOSER? LESS THREAT for pTechno.
				if (value < bestVal || bestVal < 0)
					isGoodTarget = true;
			}

			break;
		}
		case 2:
		case 3:
		{
			// Selection affected by distance
			value = pTechno->DistanceFrom(pTarget); // Note: distance is in leptons (*256)

			if (calcThreatMode == 2)
			{
				// Is this object very FAR? then LESS THREAT against pTechno.
				// More CLOSER? MORE THREAT for pTechno.
				if (value < bestVal || bestVal < 0)
					isGoodTarget = true;
			}
			else
			{
				// Is this object very FAR? then MORE THREAT against pTechno.
				// More CLOSER? LESS THREAT for pTechno.
				if (value > bestVal || bestVal < 0)
					isGoodTarget = true;
			}

			break;
		}
		default:
		{
			break;
		}
		}

		if (isGoodTarget)
		{
			pBestObject = pTarget;
			bestVal = value;
		}
	}

	return pBestObject;
}

void ScriptExt::Mission_Move_List(TeamClass* pTeam, int calcThreatMode, bool pickAllies, int attackAITargetType)
{
	const auto pTeamData = TeamExt::ExtMap.Find(pTeam);
	pTeamData->IdxSelectedObjectFromAIList = -1;

	if (attackAITargetType < 0)
	{
		const auto pScript = pTeam->CurrentScript;
		attackAITargetType = pScript->Type->ScriptActions[pScript->CurrentMission].Argument;
	}

	if (RulesExt::Global()->AITargetTypesLists.size() > 0
		&& RulesExt::Global()->AITargetTypesLists[attackAITargetType].size() > 0)
	{
		ScriptExt::Mission_Move(pTeam, calcThreatMode, pickAllies, attackAITargetType, -1);
	}
}

void ScriptExt::Mission_Move_List_Enter(TeamClass* pTeam, int calcThreatMode, bool pickAllies, int attackAITargetType)
{
	const auto pTeamData = TeamExt::ExtMap.Find(pTeam);
	pTeamData->IdxSelectedObjectFromAIList = -1;

	if (attackAITargetType < 0)
	{
		const auto pScript = pTeam->CurrentScript;
		attackAITargetType = pScript->Type->ScriptActions[pScript->CurrentMission].Argument;
	}

	if (RulesExt::Global()->AITargetTypesLists.size() > 0
		&& (size_t)attackAITargetType < RulesExt::Global()->AITargetTypesLists.size()
		&& RulesExt::Global()->AITargetTypesLists[attackAITargetType].size() > 0)
	{
		ScriptExt::Mission_Move_Enter(pTeam, calcThreatMode, pickAllies, attackAITargetType, -1);
	}
	else
	{
		// Nothing to pick the target from, so there is nothing this action could do.
		// Do not leave the team stuck on a broken action, just end it.
		pTeam->StepCompleted = true;

		const auto pScript = pTeam->CurrentScript;
		ScriptExt::Log("AI Scripts - Move Enter: [%s] [%s] (line: %d = %d,%d) Jump to next line: empty or missing [AITargetTypes] list index %d\n",
			pTeam->Type->ID,
			pScript->Type->ID,
			pScript->CurrentMission,
			pScript->Type->ScriptActions[pScript->CurrentMission].Action,
			pScript->Type->ScriptActions[pScript->CurrentMission].Argument,
			attackAITargetType);
	}
}

void ScriptExt::Mission_Move_Enter(TeamClass* pTeam, int calcThreatMode, bool pickAllies, int attackAITargetType, int idxAITargetTypeItem)
{
	const auto pTeamData = TeamExt::ExtMap.Find(pTeam);
	const auto pScript = pTeam->CurrentScript;
	const auto pScriptType = pScript->Type;
	const auto& node = pScriptType->ScriptActions[pScript->CurrentMission];

	// Find the Leader
	auto pLeaderUnit = pTeamData->TeamLeader;

	if (!ScriptExt::IsUnitAvailable(pLeaderUnit, true))
	{
		pLeaderUnit = ScriptExt::FindTheTeamLeader(pTeam);
		pTeamData->TeamLeader = pLeaderUnit;
	}

	if (!pLeaderUnit)
	{
		pTeamData->DockActionTimeout = 0;
		pTeam->StepCompleted = true;

		ScriptExt::Log("AI Scripts - Move Enter: [%s] [%s] (line: %d = %d,%d) Jump to next line (Reason: No Leader)\n",
			pTeam->Type->ID,
			pScriptType->ID,
			pScript->CurrentMission,
			node.Action,
			node.Argument);

		return;
	}

	auto pFocus = abstract_cast<TechnoClass*>(pTeam->Focus);

	if (!pFocus)
	{
		// This part of the code is used for picking a new target, the closest friendly object from the list.
		const auto pSelectedTarget = ScriptExt::FindBestObject(pLeaderUnit, node.Argument, calcThreatMode, pickAllies, attackAITargetType, idxAITargetTypeItem);

		if (!pSelectedTarget)
		{
			// No target was found with the specific criteria.
			pTeamData->DockActionTimeout = 0;

			if (!pTeamData->WaitNoTargetTimer.InProgress() && pTeamData->WaitNoTargetTimer.Completed())
			{
				pTeamData->WaitNoTargetCounter = 30;
				pTeamData->WaitNoTargetTimer.Start(30);
			}

			if (pTeamData->WaitNoTargetAttempts != 0 && pTeamData->WaitNoTargetTimer.Completed())
			{
				pTeamData->WaitNoTargetCounter = 30;
				pTeamData->WaitNoTargetTimer.Start(30); // No target? let's wait some frames

				return;
			}

			// This action finished
			pTeam->StepCompleted = true;

			const int nextMission = pScript->CurrentMission + 1;
			const auto& nextNode = pScriptType->ScriptActions[nextMission];
			ScriptExt::Log("AI Scripts - Move Enter: [%s] [%s] (line: %d = %d,%d) Jump to next line: %d = %d,%d (new target NOT FOUND)\n",
				pTeam->Type->ID,
				pScriptType->ID,
				pScript->CurrentMission,
				node.Action,
				node.Argument,
				nextMission,
				nextNode.Action,
				nextNode.Argument);

			return;
		}

		if (!ScriptExt::IsTargetObjectEntrable(pSelectedTarget))
		{
			// The object can not be entered or docked into by anything, so picking it would only
			// stall the team. End this action instead.
			pTeamData->DockActionTimeout = 0;
			pTeam->StepCompleted = true;

			ScriptExt::Log("AI Scripts - Move Enter: [%s] [%s] (line: %d = %d,%d) Jump to next line: [%s] (UID: %lu) is not an enterable object\n",
				pTeam->Type->ID,
				pScriptType->ID,
				pScript->CurrentMission,
				node.Action,
				node.Argument,
				pSelectedTarget->GetTechnoType()->get_ID(),
				pSelectedTarget->UniqueID);

			return;
		}

		pFocus = pSelectedTarget;
		pTeam->Focus = pSelectedTarget;
		pTeamData->DockActionTimeout = 1800; // 60 seconds: the harvester may have to cross the map
		pTeamData->WaitNoTargetAttempts = 0; // Disable Script Waits if there are any because a new target was selected
		pTeamData->WaitNoTargetTimer.Stop();
		pTeamData->WaitNoTargetCounter = 0; // Disable Script Waits if there are any because a new target was selected

		ScriptExt::Log("AI Scripts - Move Enter: [%s] [%s] (line: %d = %d,%d) Leader [%s] (UID: %lu) selected [%s] (UID: %lu) to move in and unload.\n",
			pTeam->Type->ID,
			pScriptType->ID,
			pScript->CurrentMission,
			node.Action,
			node.Argument,
			pLeaderUnit->GetTechnoType()->get_ID(),
			pLeaderUnit->UniqueID,
			pSelectedTarget->GetTechnoType()->get_ID(),
			pSelectedTarget->UniqueID);
	}
	else
	{
		// This part of the code is used for updating the "Enter" mission in each team unit.
		if (ScriptExt::HandleTargetEntryTimeout(pTeam))
		{
			pTeamData->DockActionTimeout = 0;
			pTeamData->IdxSelectedObjectFromAIList = -1;
			pTeam->Focus = nullptr;

			// This action finished
			pTeam->StepCompleted = true;

			const int nextMission = pScript->CurrentMission + 1;
			const auto& nextNode = pScriptType->ScriptActions[nextMission];
			ScriptExt::Log("AI Scripts - Move Enter: [%s] [%s] (line: %d = %d,%d) Jump to next line: %d = %d,%d (Reason: All team members have unloaded | timeout)\n",
				pTeam->Type->ID,
				pScriptType->ID,
				pScript->CurrentMission,
				node.Action,
				node.Argument,
				nextMission,
				nextNode.Action,
				nextNode.Argument);

			return;
		}
	}

	// Give every team member the order to move into the focused object and unload there.
	if (pFocus)
	{
		for (auto pFoot = pTeam->FirstUnit; pFoot; pFoot = pFoot->NextTeamMember)
		{
			if (!ScriptExt::IsUnitAvailable(pFoot, false))
			{
				// Units still inside a transport are neither docked nor unloaded, but they can not be
				// ordered yet either. Skip them until they are deployed.
				if (pFoot && !pFoot->InLimbo && !pFoot->Absorbed && pFoot->Transporter)
					pTeam->StepCompleted = false;

				continue;
			}

			auto const pUnit = abstract_cast<UnitClass*, true>(pFoot);

			if (!pUnit || !pUnit->Type->Harvester)
			{
				// Only harvesters have anything to unload, other members simply wait for them.
				continue;
			}

			if (pUnit->GetStoragePercentage() < 0.999)
			{
				// Only a full harvester has a load worth delivering. A half loaded one keeps
				// mining by itself (its Harvest mission is persistent), so this action does not
				// touch it - it just keeps waiting for it to fill up.
				continue;
			}

			// Docked into the target and unloading there, do not interrupt it.
			if (pUnit->HasAnyLink() && pUnit->GetNthLink(0) == pFocus)
				continue;

			const auto currentMission = pUnit->GetCurrentMission();

			// The "Enter" order stays active (and keeps retrying on its own) until the object is
			// actually entered, so it must not be re-issued every frame. Only re-issue while the
			// harvester is still on its way there.
			if (currentMission != Mission::Enter && currentMission != Mission::Unload)
			{
				const CoordStruct coord = TechnoExt::PassengerKickOutLocation(pFocus, pUnit, 10);
				auto const pDestination = MapClass::Instance.TryGetCellAt(coord != CoordStruct::Empty ? coord : pFocus->Location);

				if (!pDestination)
				{
					// Nowhere to dock at, this action can not be done by this member.
					continue;
				}

				const CellStruct destinationCell = pDestination->MapCoords;

				if (pUnit->Locomotor->Can_Enter_Cell(destinationCell) != Move::OK)
				{
					// The place next to the object is taken right now (the pads of a refinery are
					// often busy), keep the harvester waiting nearby instead of killing the action.
					continue;
				}

				// A harvester sits inside its Harvest mission, and that mission never ends.
				// An order that is merely queued would therefore wait behind it forever and
				// never actually run - which is exactly what made this action look like it did
				// nothing while the harvester walked back to its own refinery instead.
				//
				// So a harvester that is still harvesting gets interrupted first, exactly like
				// the official Move action (ScriptExt::Mission_Move) does before it queues
				// anything. Once it is on its way (Move) the orders below are only re-issued
				// gently - re-forcing every frame would keep resetting its path.
				if (currentMission == Mission::Harvest)
				{
					pUnit->SetTarget(nullptr);
					pUnit->SetDestination(nullptr, false);
					pUnit->ForceMission(Mission::Guard);
				}

				pUnit->SetArchiveTarget(pFocus);
				pUnit->QueueMission(Mission::Move, false);
				pUnit->SetDestination(pDestination, true);

				// Now enter (dock into) the target object.
				pUnit->QueueMission(Mission::Enter, false);

				// Aircraft hack. I hate how this game auto-manages the aircraft missions.
				if (pFoot->WhatAmI() == AbstractType::Aircraft && pFoot->Ammo > 0 && !pFoot->IsInAir())
					pFoot->QueueMission(Mission::Move, false);
			}
			else if (currentMission == Mission::Enter && !pUnit->Locomotor->Is_Moving())
			{
				// Driving towards the object was interrupted. Give the order again.
				pUnit->QueueMission(Mission::Enter, false);
			}

			// This member still has ore to deliver, so the action is not finished yet.
			pTeam->StepCompleted = false;
		}
	}
}

bool ScriptExt::HandleTargetEntryTimeout(TeamClass* pTeam)
{
	const auto pTeamData = TeamExt::ExtMap.Find(pTeam);

	if (pTeamData->DockActionTimeout > 0)
		pTeamData->DockActionTimeout--;

	bool allUnloaded = true;

	for (auto pFoot = pTeam->FirstUnit; pFoot; pFoot = pFoot->NextTeamMember)
	{
		if (!ScriptExt::IsUnitAvailable(pFoot, false))
		{
			// A member that is still inside a transport has not delivered anything yet.
			if (pFoot && !pFoot->InLimbo && !pFoot->Absorbed && pFoot->Transporter)
				allUnloaded = false;

			continue;
		}

		auto const pUnit = abstract_cast<UnitClass*, true>(pFoot);

		// "Done" means: nobody is still carrying a full load any more. It used to be
		// "carrying any ore at all", which does not match what this action now orders -
		// it only ever sends out harvesters that are full.
		if (pUnit && pUnit->Type->Harvester && pUnit->GetStoragePercentage() >= 0.999)
			allUnloaded = false;
	}

	// The countdown is a safety net: without it a team whose target can not be reached would
	// stay on this action line forever.
	return allUnloaded || pTeamData->DockActionTimeout <= 0;
}

bool ScriptExt::IsTargetObjectEntrable(TechnoClass* pTarget)
{
	if (auto const pBuilding = abstract_cast<BuildingClass*, true>(pTarget))
	{
		// Refineries and other enterable structures are handled by Mission::Enter,
		// buildings without anything to enter would stall the team.
		return pBuilding->Type->Refinery
			|| pBuilding->Type->DockUnload
			|| pBuilding->Type->Grinding
			|| pBuilding->Type->NumberOfDocks > 0;
	}

	return false;
}

void ScriptExt::Mission_Move_List1Random(TeamClass* pTeam, int calcThreatMode, bool pickAllies, int attackAITargetType, int idxAITargetTypeItem)
{
	bool selected = false;
	int idxSelectedObject = -1;
	std::vector<int> validIndexes;
	const auto pTeamData = TeamExt::ExtMap.Find(pTeam);

	if (pTeamData->IdxSelectedObjectFromAIList >= 0)
	{
		idxSelectedObject = pTeamData->IdxSelectedObjectFromAIList;
		selected = true;
	}

	const auto pScript = pTeam->CurrentScript;
	const auto pScriptType = pScript->Type;

	if (attackAITargetType < 0)
		attackAITargetType = pScriptType->ScriptActions[pScript->CurrentMission].Argument;

	if (attackAITargetType >= 0
		&& (size_t)attackAITargetType < RulesExt::Global()->AITargetTypesLists.size())
	{
		const auto& objectsList = RulesExt::Global()->AITargetTypesLists[attackAITargetType];

		// Still no random target selected
		if (idxSelectedObject < 0 && objectsList.size() > 0 && !selected)
		{
			const auto pFirstUnit = pTeam->FirstUnit;
			validIndexes.reserve(TechnoClass::Array.Count * objectsList.size());

			// Finding the objects from the list that actually exists in the map
			for (int i = 0; i < TechnoClass::Array.Count; i++)
			{
				const auto pTechno = TechnoClass::Array.GetItem(i);
				const auto pTechnoType = pTechno->GetTechnoType();
				bool found = false;

				for (auto j = 0u; j < objectsList.size() && !found; j++)
				{
					if (pTechnoType == objectsList[j]
						&& ScriptExt::IsUnitAvailable(pTechno, true)
						&& pickAllies == pFirstUnit->Owner->IsAlliedWith(pTechno->Owner))
					{
						validIndexes.push_back(j);
						found = true;
					}
				}
			}

			if (validIndexes.size() > 0)
			{
				idxSelectedObject = validIndexes[ScenarioClass::Instance->Random.RandomRanged(0, validIndexes.size() - 1)];
				selected = true;

				const auto& node = pScriptType->ScriptActions[pScript->CurrentMission];
				ScriptExt::Log("AI Scripts - Move: [%s] [%s] (line: %d = %d,%d) Picked a random Techno from the list index [AITargetTypes][%d][%d] = %s\n",
					pTeam->Type->ID,
					pScriptType->ID,
					pScript->CurrentMission,
					node.Action,
					node.Argument,
					attackAITargetType,
					idxSelectedObject,
					objectsList[idxSelectedObject]->ID);
			}
		}

		if (selected)
			pTeamData->IdxSelectedObjectFromAIList = idxSelectedObject;

		ScriptExt::Mission_Move(pTeam, calcThreatMode, pickAllies, attackAITargetType, idxSelectedObject);
	}

	// This action finished
	if (!selected)
	{
		pTeam->StepCompleted = true;

		const auto& node = pScriptType->ScriptActions[pScript->CurrentMission];
		ScriptExt::Log("AI Scripts - Move: [%s] [%s] (line: %d = %d,%d) Failed to pick a random Techno from the list index [AITargetTypes][%d]! Valid Technos in the list: %d\n",
			pTeam->Type->ID,
			pScriptType->ID,
			pScript->CurrentMission,
			node.Action,
			node.Argument,
			attackAITargetType,
			validIndexes.size());
	}
}

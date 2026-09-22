#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <FootClass.h>
#include <BuildingClass.h>
#include <HouseClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <MapClass.h>
#include <CellClass.h>

#include <Ext/Script/Body.h>
#include <Ext/Techno/Body.h>

#include <Utilities/Macro.h>

// ============================================================================
// 10062 配套：让"正在给盟友倒矿"的矿车，把停靠目标换成盟友的精炼厂
//
// 背景
//   引擎里决定"回哪座精炼厂"的是 FootClass::Find_Dock（0x4DEE80）。
//   它按"所属方"过滤候选建筑（0x4DEEFA: cmp eax, edx / jne），所以矿车装满后
//   只找自家的精炼厂 —— 这就是 10062 交了目标却仍然回家的原因。
//
// 关键：Find_Dock 有两条出口，都要照顾
//   出口 A（正常走完循环）：
//       0x4DF021  mov eax,[esp+0x14]   ; 把"选中的建筑"装进 EAX（4 字节）
//       0x4DF025  5B  pop ebx          ; 1 字节  <<< 钩点 A（长度 1、返回 0x4DF026）
//   出口 B（提前放弃，EAX = EDI，而循环没跑过 → 返回空）：
//       0x4DEEAC  je  0x4DF02F         ; 6 字节  <<< 钩点 B1
//       0x4DEEC3  jle 0x4DF02F         ; 6 字节  <<< 钩点 B2
//   出口 B 就是"场上没有自家精炼厂"时走的路 —— 车会原地不动。
//
// 做法
//   两条出口都不改引擎的搜索逻辑，只在它给出答案之后改答案。
//   由于出口 B 的 EAX 来自 EDI（此时无效），必须连"结果栈槽" [esp+0x14] 一起写，
//   两条出口都是从这个槽取最终结果的。
//
// 生效条件（四个全满足才动手，缺一即完全放行）
//   ① 是矿车（UnitTypeClass::Harvester）
//   ② 满载（GetStoragePercentage() >= 0.999）
//   ③ 所属小队此刻正停在 10062 这一行（ScriptExt::IsTeamRunningMoveEnterAction）
//   ④ 在场上找得到"盟友的、不是自家的、可达的"精炼厂
//
// 因此：其它矿车、其它阵营、其它脚本动作 —— 一律不受影响。
// ============================================================================

namespace AlliedRefineryDock
{
	// 挑"最近的盟友精炼厂"：排除自家、排除敌人、必须能倒矿
	static BuildingClass* FindNearestAlliedRefinery(FootClass* pFoot)
	{
		BuildingClass* pBest = nullptr;
		int bestDistance = 0;

		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			BuildingClass* const pBuilding = BuildingClass::Array.GetItem(i);

			if (!pBuilding || !pBuilding->Type || !pBuilding->Owner || pBuilding->Health <= 0)
				continue;

			// 必须不是自家的
			if (pBuilding->Owner == pFoot->Owner)
				continue;

			// 必须是盟友的
			if (!pFoot->Owner->IsAlliedWith(pBuilding->Owner))
				continue;

			// 必须是能倒矿的建筑
			if (!pBuilding->Type->Refinery && !pBuilding->Type->DockUnload)
				continue;

			const int distance = pFoot->DistanceFrom(pBuilding);

			if (!pBest || distance < bestDistance)
			{
				pBest = pBuilding;
				bestDistance = distance;
			}
		}

		if (!pBest)
			return nullptr;

		// 确认开得进去（避免"目的地过不去"导致来回抖动）
		const CoordStruct coord = TechnoExt::PassengerKickOutLocation(pBest, pFoot, 10);
		const CellClass* const pDestination = MapClass::Instance.TryGetCellAt(
			coord != CoordStruct::Empty ? coord : pBest->Location);

		if (!pDestination)
			return nullptr;

		if (pFoot->Locomotor->Can_Enter_Cell(pDestination->MapCoords) != Move::OK)
			return nullptr;

		return pBest;
	}

	// 统一判断：这辆载具此刻该不该被改道到盟友精炼厂？该 → 返回目标建筑
	static BuildingClass* GetRedirectTarget(FootClass* pFoot)
	{
		if (!pFoot)
			return nullptr;

		// ① 是矿车（FootClass 无 Type，要用 GetTechnoType()；
		//    Harvester 在 UnitTypeClass 上，需转型）
		TechnoTypeClass* const pFootType = pFoot->GetTechnoType();
		UnitTypeClass* const pUnitType = abstract_cast<UnitTypeClass*>(pFootType);

		if (!pUnitType || !pUnitType->Harvester)
			return nullptr;

		// ② 满载
		if (pFoot->GetStoragePercentage() < 0.999)
			return nullptr;

		// ③ 所属小队正在执行 10062
		if (!ScriptExt::IsTeamRunningMoveEnterAction(pFoot->Team))
			return nullptr;

		// ④ 找得到盟友精炼厂
		return FindNearestAlliedRefinery(pFoot);
	}
}

// ---- 出口 A：循环走完，EAX 刚刚被写入（0x4DF021 之后）------------------------
//   004DF021  8B 44 24 14   mov  eax,[esp+0x14]
//   004DF025  5B            pop  ebx          <<< 钩这里（长度 1，返回 0x4DF026）
// 读 ESI（this，函数头 mov esi,ecx），刻意不读 EDI（循环里会被覆写）。
DEFINE_HOOK(0x4DF025, FootClass_FindDock_PreferAlliedRefinery_Main, 0x1)
{
	enum { Continue = 0x4DF026 };

	GET(FootClass* const, pFoot, ESI);

	BuildingClass* const pTarget = AlliedRefineryDock::GetRedirectTarget(pFoot);

	if (!pTarget)
		return Continue;

	R->EAX(pTarget);

	return Continue;
}

// ---- 出口 B：提前放弃（场上没有自家精炼厂时走这里）--------------------------
// 0x4DEEAC / 0x4DEEC3 都是 6 字节、都跳向出口 B 的 0x4DF02F。
// 这里不跳转、原地返回 —— 让函数继续跑它自己的出口，避免"跳到哪"出错。
DEFINE_HOOK(0x4DEEAC, FootClass_FindDock_PreferAlliedRefinery_Early1, 0x6)
{
	GET(FootClass* const, pFoot, ESI);

	BuildingClass* const pTarget = AlliedRefineryDock::GetRedirectTarget(pFoot);

	if (pTarget)
	{
		// 出口 B 的返回值取自 [esp+0x14]，必须连它一起写
		R->Stack(STACK_OFFSET(0x14, 0), pTarget);
		R->EAX(pTarget);
	}

	return 0;
}

DEFINE_HOOK(0x4DEEC3, FootClass_FindDock_PreferAlliedRefinery_Early2, 0x6)
{
	GET(FootClass* const, pFoot, ESI);

	BuildingClass* const pTarget = AlliedRefineryDock::GetRedirectTarget(pFoot);

	if (pTarget)
	{
		R->Stack(STACK_OFFSET(0x14, 0), pTarget);
		R->EAX(pTarget);
	}

	return 0;
}

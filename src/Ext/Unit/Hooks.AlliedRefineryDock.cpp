#include <UnitClass.h>
#include <FootClass.h>
#include <BuildingClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <TechnoClass.h>

#include <Ext/Script/Body.h>
#include <Ext/Techno/Body.h>

#include <Utilities/Macro.h>

// ============================================================================
// 10062 配套：让"正在给盟友倒矿"的矿车，把停靠目标换成盟友的精炼厂
//
// 背景
//   引擎里决定"回哪座精炼厂"的是 FootClass::Find_Dock（0x4DEE80）。
//   它按"所属方"过滤候选建筑（0x4DEEFA: cmp eax, edx / jne），所以矿车装满后
//   永远只找自家的精炼厂 —— 这就是 10062 交了目标却仍然回家的原因。
//
// 做法
//   不碰引擎的搜索逻辑，只在它给出答案之后改答案：
//   钩在函数返回处（ret 0x10，返回地址 0x4DF031），此时 ESI = 这辆载具。
//   注意：这里刻意**不读栈上的返回值**（偏移算错就会崩），
//   只用 ESI + 四个条件来限定作用域，找不到就完全放行。
//
// 生效条件（四个全满足才动手，缺一即完全放行）
//   ① 是矿车（Type->Harvester）
//   ② 满载（GetStoragePercentage() >= 0.999）
//   ③ 所属小队此刻正停在 10062 这一行（ScriptExt::IsTeamRunningMoveEnterAction）
//   ④ 场上找得到"盟友的、不是自家的、可达的"精炼厂
//
// 因此：其它矿车、其它阵营、其它脚本动作 —— 一律不受影响。
// ============================================================================

DEFINE_HOOK(0x4DF02C, FootClass_FindDock_PreferAlliedRefinery, 0x3)
{
	enum { Continue = 0x4DF02F };

	GET(FootClass* const, pFoot, ESI);

	// ① 是矿车
	if (!pFoot || !pFoot->Type || !pFoot->Type->Harvester)
		return Continue;

	// ② 满载
	if (pFoot->GetStoragePercentage() < 0.999)
		return Continue;

	// ③ 所属小队正在执行 10062
	if (!ScriptExt::IsTeamRunningMoveEnterAction(pFoot->Team))
		return Continue;

	// ④ 找"最近的盟友精炼厂"（排除自家、排除自己）
	BuildingClass* pBest = nullptr;
	int bestDistance = 0;

	for (int i = 0; i < BuildingClass::Array.Count; ++i)
	{
		BuildingClass* const pBuilding = BuildingClass::Array.GetItem(i);

		if (!pBuilding || !pBuilding->Type || !pBuilding->Owner || pBuilding->Health <= 0)
			continue;

		// 必须是盟友的（自家排除）
		if (pBuilding->Owner == pFoot->Owner)
			continue;

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
		return Continue;

	// 确认开得进去（避开"目的地过不去"导致来回抖动）
	const CoordStruct coord = TechnoExt::PassengerKickOutLocation(pBest, pFoot, 10);
	const CellClass* const pDestination = MapClass::Instance.TryGetCellAt(
		coord != CoordStruct::Empty ? coord : pBest->Location);

	if (!pDestination)
		return Continue;

	if (pFoot->Locomotor->Can_Enter_Cell(pDestination->MapCoords) != Move::OK)
		return Continue;

	// 改答案：让矿车去盟友的精炼厂倒矿（钱进精炼厂所属方 = 盟友）
	R->EAX(pBest);

	return Continue;
}

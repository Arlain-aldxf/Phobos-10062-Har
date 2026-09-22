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
// 10062 配套：让"给盟友倒矿"的矿车，把落点换成盟友的精炼厂
//
// 【真正的钩点】Harvest 任务里"挑一个可用建筑"的那次虚函数调用
//
//   0073EB49  call dword ptr [edx+0x528]   ; 另一处同样的调用（提前返回支路）
//   0073EB51  je   0x73EB5A
//   0073EB55  call 0x4DF0D0                ; 清空 [this+0x5A0] / [this+0x5A4]
//   0073EB5A  mov  eax,[ebp+0x5A4]         ; 读回 ArchiveTarget
//   0073EB60  test eax,eax
//   0073EB62  jne  0x73EF77                ; 已有目标 → 跳过（"回自家"走的就是这条）
//   0073EB68  ...                          ; 没有目标 → 继续往下挑
//   0073EB7E  call dword ptr [eax+0x528]   ; ★★ 挑建筑的虚函数调用（返回值在 EAX）
//   0073EB84  test bl,bl                   ; 2 字节  <<< 钩这里
//   0073EB86  mov  esi,eax                 ; ★ 2 字节，无条件执行！EAX → ESI
//   0073EB88  jne  0x73EDC0                ; BL≠0 → 走"已有目标"支路（那里也用 ESI）
//   0073EB8E  test esi,esi
//   0073EB90  je   0x73EC1F
//   0073EB96  mov  edx,[esi]               ; 从这里开始 ESI 就是落点建筑
//
//   ebp = 这辆载具（调用点前一条是 `mov ecx,ebp`，把 this 传给虚函数）
//
//   —— 三条支路最终都要 ESI 装"落点建筑"：
//        (a) 0x73EB8E→0x73EB96 直接用 ESI
//        (b) 0x73EDC0 支路用 ESI
//        (c) 0x73EC1F 支路在 0x73EC4D 重新 `mov esi,eax`
//
// 早期版本的错误（记录以免重犯）：
//   ① 曾把钩子挂在 FootClass::Find_Dock（0x4DEE80）的两条出口上 —— 但那个函数
//      在整份 gamemd.exe 里没有任何 call（只有 4 处虚表引用），根本不在这条路上，
//      所以"毫无反应"。
//   ② 曾写 ESI —— **完全无效**。因为紧跟着的 0x73EB86 `mov esi,eax` 是无条件执行的
//      （它和 0x73EB84 的 `test bl,bl` 没有任何条件关系），刚写的 ESI 立刻被引擎
//      用 EAX 覆盖掉。**要改的是 EAX**，让引擎自己那条 `mov esi,eax` 把我们的
//      目标搬进 ESI —— 这样三条支路就都对了。
//
// 生效条件（四个全满足才动手，缺一即完全放行）
//   ① 是矿车（UnitTypeClass::Harvester）
//   ② 满载（GetStoragePercentage() >= 0.999）
//   ③ 所属小队的脚本里启用了 10062（ScriptExt::IsTeamUsingMoveEnterAction）
//      —— 注意不是"此刻正停在这一行"：矿车满载时小队往往已在后续行上
//   ④ 场上找得到"盟友的、不是自家的、可达的"精炼厂
// ============================================================================

namespace AlliedRefineryDock
{
	// 挑"最近的盟友精炼厂"：排除自家、排除敌人、必须能倒矿、必须开得进去
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

		// ① 是矿车（FootClass 无 Type，用 GetTechnoType()；
		//    Harvester 在 UnitTypeClass 上，需转型）
		TechnoTypeClass* const pFootType = pFoot->GetTechnoType();
		UnitTypeClass* const pUnitType = abstract_cast<UnitTypeClass*>(pFootType);

		if (!pUnitType || !pUnitType->Harvester)
			return nullptr;

		// ② 满载
		if (pFoot->GetStoragePercentage() < 0.999)
			return nullptr;

		// ③ 所属小队的脚本启用了 10062
		if (!ScriptExt::IsTeamUsingMoveEnterAction(pFoot->Team))
			return nullptr;

		// ④ 找到盟友精炼厂
		return FindNearestAlliedRefinery(pFoot);
	}
}

// 紧跟"挑建筑的虚函数"之后：EAX = 它挑中的建筑，我们按条件换成盟友的
//
// ⚠️ 两个必须守住的细节（第一版就在这里崩了）：
//   ① 钩点必须"跳过 test bl,bl"、返回 0x73EB88 —— 让引擎自己执行那条 test，
//      这样紧接着的 `jne 0x73EDC0` 读到的是它该有的标志位。
//      第一版钩在 0x73EB84（把 test 吃掉了），jne 读到的是我 C++ 代码留下的
//      垃圾标志位 → 走错分支 → 在 0x73EB9F 拿错指针调虚函数 → 崩溃。
//   ② 必须改写 EAX（不是 ESI）：0x73EB86 的 `mov esi,eax` 是无条件执行的，
//      写在 ESI 上的值会被它立刻覆盖，等于什么都没做（这正是 ed7a6f5 无效的原因）。
//      改 EAX 之后由引擎自己把它搬进 ESI，三条支路都拿到正确的落点建筑。
DEFINE_HOOK(0x73EB84, FootClass_HarvestReturn_PreferAlliedRefinery, 0x2)
{
	enum { Continue = 0x73EB88 }; // → 让引擎自己执行 test bl,bl（标志位不能被破坏）

	GET(FootClass* const, pFoot, EBP);

	BuildingClass* const pTarget = AlliedRefineryDock::GetRedirectTarget(pFoot);

	if (!pTarget)
		return Continue;

	// 交给引擎的 `mov esi,eax`（0x73EB86）搬进 ESI —— 那是三条支路共用的落点寄存器
	R->EAX(pTarget);

	return Continue;
}

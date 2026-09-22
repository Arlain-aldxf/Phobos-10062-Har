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
// 【问题的本质】
//   长征方【没有任何自家精炼厂】时，矿车采满矿后引擎要"回自家倒矿"，
//   却找不到任何自家矿场 → 没有目的地 → 矿车原地停下。
//   脚本的 10062 是在动作执行那一刻发出的，矿车装满时那一刻早已过去，
//   所以没人再命令它 → 这就是"引擎抢在脚本前面"的现象。
//
// 【解法：替引擎把它缺的那个字段填上】
//
//   0073EB2C  mov  ecx,[ebp+0x5A4]         ; ← ArchiveTarget（目的地字段）
//   0073EB32  test ecx,ecx
//   0073EB34  je   0x73EB5A                ; 有目标 → 用它；没有 → 往下挑建筑
//   0073EB49  call dword ptr [edx+0x528]   ; 挑建筑的虚函数
//   0073EB51  je   0x73EB5A
//   0073EB55  call 0x4DF0D0                ; 清空 [this+0x5A0] / [this+0x5A4]
//   0073EB5A  mov  eax,[ebp+0x5A4]         ; 再读一次 ArchiveTarget
//   0073EB60  test eax,eax
//   0073EB62  jne  0x73EF77                ; 有目标 → 前往该目标
//   0073EB68  ...                          ; 还是没有 → 继续挑
//   0073EB7E  call dword ptr [eax+0x528]   ; 挑建筑的虚函数调用（返回值在 EAX）
//   0073EB84  test bl,bl                   ; 2 字节  <<< 钩这里
//   0073EB86  mov  esi,eax
//   0073EB88  jne  0x73EDC0
//   0073EB8E  test esi,esi
//   0073EB90  je   0x73EC1F
//   0073EB96  mov  edx,[esi]
//
//   ArchiveTarget 就是 SetArchiveTarget() 写的字段（JMP_THIS(0x70C610)），
//   也正是引擎自己在 0x73EAF2 / 0x73EA7B 调用的那个函数。
//
//   **只要在它读之前把 ArchiveTarget 填好，引擎自己就会走完整条"前往倒矿"的路**
//   —— 不需要碰任何寄存器、任何分支。
//
// 【为什么不改寄存器】（实测结果，2026-09-22 夜）
//
//   | 版本    | 写了什么  | 结果                     |
//   |---------|-----------|--------------------------|
//   | 3cae517 | 只写 EAX  | 不崩，但矿车【不动】     |
//   | ed7a6f5 | 只写 ESI  | 【不崩】，但矿车【停下】 |
//
//   结论：**只写 ESI 不会崩，但也不生效** → 引擎判断目的地不看寄存器，看字段。
//   （详见下方钩子处的"同一坑摔两次"更正：早期以为"双写会崩"，其实是探针自己崩的。）
//
// 早期版本的错误（记录以免重犯）：
//   ① 曾把钩子挂在 FootClass::Find_Dock（0x4DEE80）的两条出口上 —— 但那个函数
//      在整份 gamemd.exe 里没有任何 call（只有 4 处虚表引用），根本不在这条路上，
//      所以"毫无反应"。
//   ② 曾连续三代都在纠结"该写 EAX 还是 ESI" —— 方向本身就错了，见上表。
//   ③ 钩点必须"跳过 test bl,bl"、返回 0x73EB88，让引擎自己执行那条 test；
//      把它吃掉会让 `jne` 读到 C++ 留下的垃圾标志位 → 走错分支 → 崩。
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

// ---------------------------------------------------------------------------
// 钩点：0x73EB84 —— 最终方案 = 【防崩】+【真的开过去】，两者缺一不可
//
// 【一、写 ESI 是为了防崩（这是硬需求，不是猜的）】
//
//   引擎在 0x73EB7E 调 `[eax+0x528]` 挑建筑。**在"长征方没有任何自家精炼厂"
//   这个场景下它找不到可回的建筑，返回的不是建筑指针，而是一个内部字段地址**
//   —— 崩溃时实测 esi = ebp + 0xC8，解引用出来是 0x651 这种垃圾值。
//
//   而接下来三条支路**全都**要 ESI 装"建筑"：
//     · BL=0  → 0x73EB86 mov esi,eax → 0x73EB8E test esi,esi → 0x73EB96 mov edx,[esi]
//     · BL≠0  → 0x73EB88 jne 0x73EDC0 → 0x73EDC8 mov eax,[esi]
//     · 挑不到 → 0x73EB90 je 0x73EC1F（只有这条会自己重新 mov esi,eax）
//   → ESI 是垃圾就崩在 0x73EB9F（实测两次都是这里，且与"改不改寄存器"无关：
//     连"一个寄存器都不碰"的版本也崩在同一个地址）。
//   → **把 ESI 换成合法建筑，崩溃就消失**（实测：只写 ESI 那版全程不崩）。
//
// 【二、还要自己下达指令，是为了真的开过去】
//
//   只写 ESI 那版**不崩、但矿车也不动**：引擎只是拿 ESI 去算距离/优先级，
//   并不会因此主动开过去。**目的地必须显式下达。**
//
//   所以照抄 Mission.Move.cpp 里 10062 主流程、**已在测试 02 走通过**的写法：
//     SetArchiveTarget(pTarget)         ← 告诉引擎"我要进这个建筑"
//     SetTarget(nullptr)                ← 清掉可能还指着矿田的目标
//     QueueMission(Move) + SetDestination(目标旁落点)
//     QueueMission(Enter)               ← 再"进入"
//   落点用 TechnoExt::PassengerKickOutLocation（与 10062 主流程完全一致）。
//
// 【走过弯路的记录（别再重走）】
//   · 钩 FootClass::Find_Dock（0x4DEE80）出口 —— 那函数全 exe 无 call，不在路上。
//   · 只在"该写 EAX 还是 ESI"之间来回换 —— 都不是关键，关键见上面两条。
//   · 曾以为"同时写 ESI+EAX 会崩" —— 其实是探针自己崩的：诊断用的入口钩子挂在
//     0x73EB32，吃掉了 `test ecx,ecx` 却没恢复标志位，导致紧随的 `je 0x73EB5A`
//     读到 C++ 留下的垃圾标志位 → 走错分支 → 崩在 0x4DF07A（与 Harvest 无关的
//     通用容器操作里）。
//     **教训（同一个坑摔了两次）**：钩掉一条"设标志位"的指令（`test`/`cmp`）后，
//     必须在钩子里把那条指令重做一遍，否则后续条件跳转读到的是垃圾标志位。
//
// 生效条件（四个全满足才动手，缺一即完全放行、退回原版行为）
//   ① 是矿车（UnitTypeClass::Harvester）
//   ② 满载（GetStoragePercentage() >= 0.999）
//   ③ 所属小队的脚本里启用了 10062（ScriptExt::IsTeamUsingMoveEnterAction）
//      —— 不是"此刻正停在这一行"：矿车满载时小队往往已在后续行上
//   ④ 场上找得到"盟友的、不是自家的、可达的"精炼厂
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x73EB84, FootClass_HarvestReturn_PreferAlliedRefinery, 0x2)
{
	enum { Continue = 0x73EB88 }; // → 让引擎自己执行 test bl,bl（标志位不能被破坏）

	GET(FootClass* const, pFoot, EBP);

	BuildingClass* const pTarget = AlliedRefineryDock::GetRedirectTarget(pFoot);

	if (!pTarget)
		return Continue;

	// ---- ① 防崩：把 ESI 换成合法建筑 ----
	// 引擎紧接着会把它当"建筑"解引用；不给它一个真的就会崩在 0x73EB9F。
	R->ESI(pTarget);

	// ---- ② 真的开过去：照抄 10062 主流程（测试 02 已验证走通）----
	//
	// ⚠️⚠️ 关键守卫（漏了会出事，实测教训）：
	//    这个钩子**每一帧都会被调用**。如果每帧都重新下达 Move/Enter，
	//    矿车刚要对接就被指令重置 → 永远进不了门 → 表现为"在矿场旁边疯狂抖动/
	//    反复移动"（盟军超时空矿车会留下一串传送残影，苏军矿车原地来回蹭）。
	//
	//    Mission.Move.cpp 里 10062 主流程早就有这个守卫，原话：
	//      "The Enter order stays active (and keeps retrying on its own) until the
	//       object is actually entered, so it must not be re-issued every frame.
	//       Only re-issue while the harvester is still on its way there."
	//    照抄它：Enter / Unload 进行中 → 什么都别做，让引擎自己走完。
	const auto currentMission = pFoot->GetCurrentMission();

	if (currentMission == Mission::Enter || currentMission == Mission::Unload)
		return Continue;

	// 已经挂上目标（正在倒矿）→ 更不要打扰
	if (pFoot->HasAnyLink() && pFoot->GetNthLink(0) == pTarget)
		return Continue;

	// ⚠️⚠️ 第二条守卫（实测教训，很关键）：
	//    目标一旦已经确定（ArchiveTarget 就是这座盟友精炼厂），就**彻底停止插手**，
	//    让引擎自己走完"走到 → 对接 → 倒矿"。
	//
	//    为什么需要：矿车开到停机坪时往往还处于 Move 状态，上面那条
	//    "Enter/Unload 就不打扰"覆盖不到它。若此时仍每帧重下
	//    SetDestination + Move + Enter，引擎"准备对接"的过程会被反复重置 ——
	//    表现为【矿车站在矿上却不倒矿】（实测：9bea8ae+守卫版就是这个现象）。
	//
	//    Mission.Move.cpp 的注释也是这个意思："Only re-issue while the harvester
	//    is still on its way there."
	if (pFoot->ArchiveTarget == pTarget)
		return Continue;

	// ⚠️⚠️ 目的地必须用【建筑本身】，不能用 PassengerKickOutLocation
	//
	//    PassageKickOutLocation 算的是"从这座建筑里被扔出来时落在哪"，也就是**旁边一格**。
	//    用它当 Destination，矿车开到旁边那格就当成终点停下了 ——
	//    表现为【占住矿口却不进门倒矿】（实测就是这个问题）。
	//
	//    玩家右键点建筑下达"进入"时，引擎发的就是"目的地 = 建筑本身"，
	//    由引擎自己去算该停到哪个停机位。
	//
	// 顺序也照玩家操作来：先 SetArchiveTarget（记住要进哪座），再 Move 过去，最后 Enter 进门。
	pFoot->SetArchiveTarget(pTarget);
	pFoot->SetTarget(nullptr);
	pFoot->SetDestination(pTarget, true);
	pFoot->QueueMission(Mission::Move, false);
	pFoot->QueueMission(Mission::Enter, false);

	return Continue;
}

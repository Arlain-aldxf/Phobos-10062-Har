#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <FootClass.h>
#include <BuildingClass.h>
#include <HouseClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>

#include <Ext/Script/Body.h>

#include <Utilities/Macro.h>

#include <cstdio>   // 诊断用的状态记录

// ============================================================================
// 10062 配套：让"脚本里启用了 10062 的小队"的矿车，把【回厂倒矿】的落点
//             换成盟友的精炼厂。
//
// ---------------------------------------------------------------------------
// 【零、🔴 钩子机制的硬规则（本轮用一次崩溃换来的，务必先看这条）】
//
//   **Syringe 在钩点处写的是固定 5 字节的 `jmp`；`DEFINE_HOOK` 的第 3 个参数
//     只决定它把多少字节的原始指令搬到别处。**
//
//   → 所以钩子返回的地址**必须落在 钩点+5 之外**。
//     返回 `钩点+2` 这种"跳过一条 2 字节指令"的写法会**执行到 jmp 的中间** → 崩。
//     实测：`DEFINE_HOOK(0x73EC4D, ..., 0x2)` + `return 0x73EC4F`
//           → `Exception 0xC0000005 at 0x0073EC4F`（`dec ecx` 一条绝不会崩的指令崩了，
//             正说明那里的字节已经被 jmp 覆盖）。
//
//   → 于是本文件所有钩点的返回地址都按"跳到下一段安全代码"来选，
//     并且**凡是依赖标志位的判断，一律由钩子自己显式完成**（见下）。
//
// ---------------------------------------------------------------------------
// 【一、引擎的回厂流水线（反汇编 0x73E6CF 起，YR 1.001；EBP = 这辆载具）】
//
//   MissionStatus == 2 → 0x73EB2C：
//     0073EB5A  mov  eax,[ebp+0x5A4]       ; ★ FootClass::Destination
//     0073EB60  test eax,eax
//     0073EB62  jne  0x73EF77              ; 已经有目的地 → 本帧什么都不做（等它开到）
//     0073EB68  mov  ecx,[ebp+0x6C4]       ; UnitTypeClass*
//     0073EB73  add  ecx,0x3E8             ; &UnitTypeClass::Dock
//     0073EB7E  call [eax+0x528]           ; ★★ 第一次挑建筑 EAX = FindDock(&Type->Dock,0,0)
//     0073EB84  test bl,bl                 ; ★★★ 钩点 A          bl = UnitTypeClass+0xCD4
//     0073EB86  mov  esi,eax
//     0073EB88  jne  0x73EDC0              ; bl≠0 支路
//     0073EB8E  test esi,esi               ; bl＝0 支路
//     0073EB90  je   0x73EC1F
//     …算距离 → 够近(≤Rules[0xD78]<<8) → 0x73EE51 开始对接 / 太远 → 0x73EC1F 重试
//
//     0073EDC0  （bl≠0 支路）…算距离 → 够近(≤Rules[0xD7C]<<8) → 0x73EE51 / 太远 → 0x73EC1F
//
//     0073EC1F  （重试支路）
//     0073EC41  call [edx+0x528]           ; ★★ 第二次挑建筑（同一次查询！）
//     0073EC47  mov  ecx,[0xa8e7ac]        ; 重试计数器
//     0073EC4D  mov  esi,eax
//     0073EC4F  dec  ecx
//     0073EC50  test esi,esi
//     0073EC52  mov  [0xa8e7ac],ecx
//     0073EC58  je   0x73EF77              ; ★★★ 钩点 B：挑不到 → 放弃，什么都不做
//     0073EC5E  …算距离…
//     0073ECD0  cmp  eax,0x300             ; 3 格
//     0073ECD5  jg   0x73ECDF
//     0073ECD7  test bl,bl                 ; ★★★ 钩点 C
//     0073ECD9  je   0x73EF77              ; bl＝0 且"很近" → 也放弃
//     0073ECDF  …算停机坪那一格…
//     0073EDB5  call [esi+0x480](cell, 1)  ; ★ 太远 → 把 Destination 设成停机坪，开过去
//
//     0073EE51  call [eax+0x278](2, esi)   ; 够近 → 开始对接
//     0073EE5F  cmp  eax,1
//     0073EE62  jne  0x73EC1F              ; 对接没成 → 重试
//     0073EE68  mov  [ebp+0xBC],3          ; MissionStatus = 3
//
//   MissionStatus == 3 → 0x73EE8A：QueueMission(Mission::Enter) → 进厂、卸货
//
//   **三个"出口"必须全部堵住**，否则引擎就在其中一处静悄悄地放弃：
//     · 第一次挑建筑 → 引擎返回 0（长征方没有自家精炼厂）→ 钩点 A 换成盟友的
//     · 重试再挑一次   → 引擎还是返回 0 → 钩点 B 换成盟友的（否则 0x73EC58 放弃）
//     · bl＝0 的类型    → 0x73ECD9 会"就近放弃" → 钩点 C 接管这个判断
//   实测（fad82a8）：只堵了前两处时，探针里 `dest=00000000` 一直不变
//   —— 引擎从头到尾没给矿车设过目的地，所以它一步不走。
//
// ---------------------------------------------------------------------------
// 【二、字段偏移（本次最重要的更正，前几轮一直搞错了）】
//
//   · FootClass::Destination    = FootClass  + 0x5A4  ← 引擎在 Harvest 里读的就是它
//   · TechnoClass::ArchiveTarget = TechnoClass + 0x218 ← 引擎在别处读它
//       `SetArchiveTarget` = 0x70C610，全文只有两句：
//         8B 44 24 04        mov eax,[esp+4]
//         89 81 18 02 00 00  mov [ecx+0x218],eax     ← 写的是 0x218，不是 0x5A4
//   → 前几轮把 [ebp+0x5A4] 当成 ArchiveTarget，守卫条件全建立在错误字段上。
//
// ---------------------------------------------------------------------------
// 【三、走过的弯路（别再重走）】
//
//   · 钩 FootClass::Find_Dock(0x4DEE80) —— 那是 [vtable+0x52C]「在某一类建筑里找
//     我方可用的那一座」，不是流水线入口。
//   · 只写 EAX / 只写 ESI / 两个都写 —— 只换寄存器并不会让引擎主动开过去。
//   · 自己 SetArchiveTarget / QueueMission(Harvest/Enter) / SetDestination 下命令 ——
//     和引擎的 Harvest 状态机抢时间线，实测：原地不动、开过去不倒矿、抖动、卡在矿里。
//   · 钩点返回 `钩点+2` —— 见【零】，会崩。
//
// ---------------------------------------------------------------------------
// 【四、生效条件（四个全满足才改答案，缺一即完全放行原版行为）】
//
//   ① 是矿车（UnitTypeClass::Harvester）
//   ② 满载（GetStoragePercentage() >= 0.999）
//   ③ 所属小队的脚本里启用了 10062（ScriptExt::IsTeamUsingMoveEnterAction）
//      —— 不是"此刻正停在这一行"：矿车满载时小队往往已经走到后面的行
//   ④ 场上找得到"盟友的、不是自家的、活着的"精炼厂
// ============================================================================

namespace AlliedRefineryDock
{
	// 引擎在 0x73E6DE 读的那个类型开关字节：`mov bl, byte ptr [eax+0xCD4]`
	// 它决定"太远/很近"时走哪条兜底支路（0x73EDC0 / 0x73EB8E / 0x73ECD9）。
	// 实测两台矿车取值不同（一台 0xFF、一台 0x00），所以**必须照原值分流**，
	// 不能假设所有矿车一样。
	static bool ReadTypeFlag0xCD4(FootClass* pFoot)
	{
		TechnoTypeClass* const pType = pFoot->GetTechnoType();

		if (!pType)
			return false;

		return *reinterpret_cast<const BYTE*>(reinterpret_cast<const char*>(pType) + 0xCD4) != 0;
	}

	// 挑"最近的盟友精炼厂"：排除自家、排除敌人、必须能倒矿。
	// 不做可达性预判 —— 引擎自己的选择流程也不做，
	// 它会在"太远"时先把 Destination 设成停机坪那一格，再靠近、再对接。
	static BuildingClass* FindNearestAlliedRefinery(FootClass* pFoot)
	{
		BuildingClass* pBest = nullptr;
		int bestDistance = 0;

		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			BuildingClass* const pBuilding = BuildingClass::Array.GetItem(i);

			if (!pBuilding || !pBuilding->Type || !pBuilding->Owner || pBuilding->Health <= 0)
				continue;

			// 必须不是自家的（否则就是原版行为，不用我们插手）
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

		// ③ 所属小队的脚本里启用了 10062
		if (!ScriptExt::IsTeamUsingMoveEnterAction(pFoot->Team))
			return nullptr;

		// ④ 找到盟友精炼厂
		return FindNearestAlliedRefinery(pFoot);
	}

	// 决定改道时，顺手把 ArchiveTarget 也指到那座精炼厂。
	// 理由：引擎自己决定回厂时（0x73EAF2 / 0x73EA7B）也会 SetArchiveTarget，
	// 而 10062 动作（唯一实测跑通过的那条路）同样是 SetArchiveTarget + Enter。
	// 只改 Destination 而不改 ArchiveTarget，卸货环节可能找不到该往哪座建筑卸。
	static void MarkAsDockTarget(FootClass* pFoot, BuildingClass* pRefinery)
	{
		if (pRefinery)
			pFoot->SetArchiveTarget(pRefinery);
	}

	// ---- 状态变化记录（诊断用；只在状态改变时写一行）----
	// 只记变化 + 每行 fflush，所以"崩了也不丢"，且不刷屏。
	// dest 是否被设上进 key —— "引擎有没有给矿车目的地"是判断卡在哪的关键。
	static void Trace(FootClass* pFoot, BuildingClass* pEngineAnswer, BuildingClass* pResult,
		bool redirected, int site)
	{
		static FILE* s_log = nullptr;
		static DWORD s_lastKey = 0xFFFFFFFF;
		static int   s_lines = 0;

		const DWORD curMission = static_cast<DWORD>(pFoot->GetCurrentMission());
		const DWORD curStatus = static_cast<DWORD>(pFoot->MissionStatus);
		const DWORD hasDest = pFoot->Destination ? 1 : 0;
		const DWORD linked = (pFoot->HasAnyLink() && pFoot->GetNthLink(0) == pResult) ? 1 : 0;
		const DWORD key = (curMission << 20) ^ (curStatus << 8) ^ (hasDest << 7)
			^ (linked << 6) ^ ((redirected ? 1u : 0u) << 5) ^ (static_cast<DWORD>(site) << 3);

		if (key != s_lastKey && s_lines < 60)
		{
			s_lastKey = key;
			++s_lines;

			if (!s_log)
			{
				s_log = fopen("D:\\Ra2 project\\2-开发环境\\Mod工作副本\\probe10062.log", "w");

				if (!s_log)
					s_log = fopen("probe10062.log", "w");

				if (s_log)
				{
					fprintf(s_log, "=== 10062 allied-refinery redirect trace ===\n");
					fflush(s_log);
				}
			}

			if (s_log)
			{
				fprintf(s_log,
					"#%-3d site=%d mission=%-3u status=%-3u storage=%.3f dest=%p archiveTarget=%p "
					"engineSaid=%p weReturned=%p redirected=%u linked=%u typeFlag=0x%02X dist=%d\n",
					s_lines, site,
					static_cast<unsigned>(curMission),
					static_cast<unsigned>(curStatus),
					pFoot->GetStoragePercentage(),
					pFoot->Destination,
					pFoot->ArchiveTarget,
					pEngineAnswer,
					pResult,
					static_cast<unsigned>(redirected ? 1 : 0),
					static_cast<unsigned>(linked),
					ReadTypeFlag0xCD4(pFoot) ? 0xFF : 0x00,
					pResult ? pFoot->DistanceFrom(pResult) : -1);
				fflush(s_log);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// 钩点 A：0x73EB84 —— 引擎【第一次】问完"我该去哪座精炼厂"，我们在这里改答案
//
//   Syringe 的 5 字节 jmp 覆盖 0x73EB84~0x73EB88（`test bl,bl` + `mov esi,eax`
//   + `jne` 的头一个字节），所以：
//     · `mov esi,eax` 不会执行 → ESI 必须由我们写；
//     · 标志位没了 → 必须自己显式跳 0x73EB8E / 0x73EDC0（两者都在 jmp 之外）。
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x73EB84, FootClass_HarvestReturn_PreferAlliedRefinery, 0x2)
{
	enum { Path_BlZero = 0x73EB8E, Path_BlNonZero = 0x73EDC0 };

	GET(FootClass* const, pFoot, EBP);
	GET(BuildingClass* const, pOriginalTarget, EAX);

	BuildingClass* const pAlliedRefinery = AlliedRefineryDock::GetRedirectTarget(pFoot);
	BuildingClass* const pResult = pAlliedRefinery ? pAlliedRefinery : pOriginalTarget;

	AlliedRefineryDock::MarkAsDockTarget(pFoot, pAlliedRefinery);

	R->EAX(pResult);
	R->ESI(pResult);

	AlliedRefineryDock::Trace(pFoot, pOriginalTarget, pResult, pAlliedRefinery != nullptr, 1);

	return AlliedRefineryDock::ReadTypeFlag0xCD4(pFoot) ? Path_BlNonZero : Path_BlZero;
}

// ---------------------------------------------------------------------------
// 钩点 B：0x73EC58 —— 重试支路里"这次挑到了吗？"的判断（`je 0x73EF77`）
//
//   这里替换的是一条 6 字节的 `je`，5 字节 jmp 之后还剩 1 个字节，
//   但因为我们**永远显式跳到 0x73EC5E 或 0x73EF77**，那一个残留字节不会被执行。
//   0x73EC4F 的 `dec ecx`（重试计数器还原）和 0x73EC50 的 `test esi,esi`
//   都在钩点之前，原封不动执行 —— 计数器不会漏，也不碰任何标志位。
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x73EC58, FootClass_HarvestReturn_PreferAlliedRefinery_Retry, 0x6)
{
	enum { ComputeDistance = 0x73EC5E, GiveUp = 0x73EF77 };

	GET(FootClass* const, pFoot, EBP);
	GET(BuildingClass* const, pEngineAnswer, ESI);

	BuildingClass* const pAlliedRefinery = AlliedRefineryDock::GetRedirectTarget(pFoot);

	if (!pAlliedRefinery && !pEngineAnswer)
		return GiveUp;                     // 原版行为：重试也挑不到 → 放弃

	BuildingClass* const pResult = pAlliedRefinery ? pAlliedRefinery : pEngineAnswer;

	AlliedRefineryDock::MarkAsDockTarget(pFoot, pAlliedRefinery);

	R->ESI(pResult);

	AlliedRefineryDock::Trace(pFoot, pEngineAnswer, pResult, pAlliedRefinery != nullptr, 2);

	return ComputeDistance;
}

// ---------------------------------------------------------------------------
// 钩点 C：0x73ECD7 —— `test bl,bl / je 0x73EF77`：bl＝0 的类型会"就近放弃"
//
//   引擎原意：bl＝0 的类型，只要在 3 格以内就不必专门开去停机坪了。
//   但对我们来说"已经在盟友矿场附近"恰恰是**最该继续走完对接**的时候，
//   而且实测两台矿车的 bl 取值不同 —— 不改这里，其中一台永远拿不到目的地。
//   （Syringe 的 jmp 覆盖 0x73ECD7~0x73ECDB，所以显式跳到 0x73ECDF / 0x73EF77。）
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x73ECD7, FootClass_HarvestReturn_PreferAlliedRefinery_DontGiveUp, 0x2)
{
	enum { GoToDockPad = 0x73ECDF, GiveUp = 0x73EF77 };

	GET(FootClass* const, pFoot, EBP);

	if (AlliedRefineryDock::GetRedirectTarget(pFoot))
		return GoToDockPad;

	// 原样重现引擎的 `test bl,bl ; je 0x73EF77`
	return AlliedRefineryDock::ReadTypeFlag0xCD4(pFoot) ? GoToDockPad : GiveUp;
}

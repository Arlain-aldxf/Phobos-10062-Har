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
// 【一、问题的本质：不要去跟引擎抢，要去回答引擎的问题】
//
//   矿车装满后，引擎自己的 UnitClass::Mission_Harvest（MissionStatus == 2，
//   即 "returning to refinery"）会做这些事（反汇编 0x73E6CF 起，版本 = YR 1.001）：
//
//     MissionStatus == 2 → 0x73EB2C：
//     0073EB5A  mov  eax,[ebp+0x5A4]       ; ★ FootClass::Destination
//     0073EB60  test eax,eax
//     0073EB62  jne  0x73EF77              ; 已经有目的地 → 本帧什么都不做
//     0073EB68  mov  ecx,[ebp+0x6C4]       ; UnitTypeClass*
//     0073EB73  add  ecx,0x3E8             ; &UnitTypeClass::Dock（TypeList<BuildingTypeClass*>）
//     0073EB7E  call [eax+0x528]           ; ★★ 第一次挑建筑 EAX = FindDock(&Type->Dock,0,0)
//     0073EB84  test bl,bl                 ; ★★★ 钩点 A（2 字节）
//     0073EB86  mov  esi,eax
//     0073EB88  jne  0x73EDC0              ; bl≠0 支路（矿车走这条）
//     0073EB8E  test esi,esi               ; bl＝0 支路
//     0073EB90  je   0x73EC1F
//     …算距离…
//     0073EC17  cmp  eax, edx              ; edx = Rules[0xD7C]<<8（或 0xD78）
//     0073EE51  call [eax+0x278](2, esi)   ; 够近 → 开始对接
//     0073EE68  mov  [ebp+0xBC],3          ; MissionStatus = 3
//
//     0073EC1F  （太远 / 对接没成 → 重试支路）
//     0073EC30  mov  eax,[ebp+0x6C4] / add eax,0x3E8
//     0073EC41  call [edx+0x528]           ; ★★ 第二次挑建筑（同样的调用！）
//     0073EC4D  mov  esi,eax               ; ★★★ 钩点 B（2 字节）
//     0073EC50  test esi,esi
//     0073EC58  je   0x73EF77              ; 挑不到 → 放弃，本帧什么都不做
//     0073EC5E  …算距离…
//     0073ECD0  cmp  eax,0x300             ; 3 格
//     0073ECD5  jg   0x73ECDF
//     0073ECD7  test bl,bl ; je 0x73EF77   ; bl＝0 且很近 → 放弃
//     0073ECDF  …算停机坪那一格…
//     0073EDB5  call [esi+0x480](cell, 1)  ; ★ 太远 → 把 Destination 设成停机坪，开过去
//     0073EDBB  jmp  0x73EF77
//
//     下一帧 MissionStatus == 3 → 0x73EE8A：QueueMission(Mission::Enter) → 进厂、卸货
//
//   **"挑建筑 → （太远先开去停机坪）→ 靠近 → 对接 → 进厂卸货"这条流水线引擎全有。**
//   长征方没有自家精炼厂时它之所以卡住，是因为这条流水线的**第一个输入**没了：
//   FindDock 只在自己的 Dock 清单里、按**自己的阵营**找建筑，找不到就返回 0。
//   于是 0x73EC58 直接放弃 → 没人给矿车设目的地 → **矿车原地不动**。
//
//   ⚠️⚠️ 所以**两次挑建筑都必须改答案**（钩点 A 与钩点 B），缺一不可：
//      第一次距离必然"太远"（矿车在矿田上、精炼厂隔好几格）→ 一定落到重试支路，
//      而重试支路那次挑建筑如果不改答案，引擎照样拿到 0 → 0x73EC58 放弃 → 还是不动。
//      实测（49fe8b3）就是这样：探针 `redirected=1`、`engineSaid=0`、**`dest=0`**，
//      矿车满载后一步不走。
//
// ---------------------------------------------------------------------------
// 【二、字段偏移（这是本次最重要的更正，前几轮一直搞错了）】
//
//   · FootClass::Destination  = FootClass + 0x5A4  ← 引擎在 Harvest 里读的就是它
//       YRpp/FootClass.h:175  AbstractClass* Destination;
//       0x4DF0D0（FootClass::AbortMotion）清的就是 [+0x5A0] 与 [+0x5A4]。
//
//   · TechnoClass::ArchiveTarget = TechnoClass + 0x218
//       YRpp/TechnoClass.h:448  void SetArchiveTarget(AbstractClass*) { JMP_THIS(0x70C610); }
//       反汇编 0x70C610 只有两句：8B 44 24 04  mov eax,[esp+4]
//                                 89 81 18 02 00 00  mov [ecx+0x218],eax   ← 写的是 0x218
//
//   → 前几轮把 [ebp+0x5A4] 当成 ArchiveTarget，守卫条件全建立在错误字段上。
//
// ---------------------------------------------------------------------------
// 【三、为什么必须显式跳分支，不能靠标志位】
//
//   钩点 A 钩掉的正是 0x73EB84 的 `test bl,bl`。这条指令被挪走后，紧随的
//   `jne 0x73EDC0` 读到的是 C++ 代码留下的**垃圾标志位**。
//   （本会话前几轮在同一个坑里摔了两次：test bl,bl、test ecx,ecx。）
//   → 钩子里必须**自己判断、自己跳**，bl 从 UnitTypeClass+0xCD4 原样读出。
//   钩点 B（0x73EC4D）不带条件跳转，只需照原样写回 EAX/ESI。
//
// ---------------------------------------------------------------------------
// 【四、走过的弯路（别再重走）】
//
//   · 钩 FootClass::Find_Dock(0x4DEE80) —— 那是 [vtable+0x52C]「在某一类建筑里找
//     我方可用的那一座」，不是流水线入口，钩它没意义。
//   · 只写 EAX / 只写 ESI / 两个都写 —— 只换寄存器并不会让引擎主动开过去。
//   · SetArchiveTarget / QueueMission(Harvest/Enter) / SetDestination 自己下命令 ——
//     和引擎的 Harvest 状态机抢时间线，实测：原地不动、开过去不倒矿、抖动、卡在矿里。
//   · 用 Mission::Enter 而不是 Harvest —— 对矿车是错的
//     （Phobos 自己挂 0x74312A 把 Enter 改成 Harvest，见 Hooks.Harvester.cpp）。
//   · 只钩第一次挑建筑（0x73EB84）—— 见上面【一】的 ⚠️。
//
// ---------------------------------------------------------------------------
// 【五、生效条件（四个全满足才改答案，缺一即完全放行）】
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
	// 它只负责在两条"距离判断/兜底"支路之间分流，这里照原值读出、照原值走。
	static bool ReadTypeFlag0xCD4(FootClass* pFoot)
	{
		TechnoTypeClass* const pType = pFoot->GetTechnoType();

		if (!pType)
			return false;

		return *reinterpret_cast<const BYTE*>(reinterpret_cast<const char*>(pType) + 0xCD4) != 0;
	}

	// 挑"最近的盟友精炼厂"：排除自家、排除敌人、必须能倒矿
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

	// ---- 状态变化记录（诊断用；只在状态改变时写一行）----
	// 只记变化 + 每行 fflush，所以"崩了也不丢"，且不刷屏。
	// dest 是否被设上也要进 key —— "引擎有没有给矿车目的地"是判断卡在哪的关键。
	static void Trace(FootClass* pFoot, const void* pEngineAnswer, const void* pResult,
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
					"engineSaid=%p weReturned=%p redirected=%u linked=%u typeFlag=0x%02X\n",
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
					ReadTypeFlag0xCD4(pFoot) ? 0xFF : 0x00);
				fflush(s_log);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// 钩点 A：0x73EB84 —— 引擎【第一次】问完"我该去哪座精炼厂"，我们在这里改答案
//
//   进入时：EAX = 引擎自己找到的建筑（自家精炼厂，或 0 = 没找到）
//   离开时：EAX/ESI = 我们要它去的建筑，并按 bl 走引擎原本的分支
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x73EB84, FootClass_HarvestReturn_PreferAlliedRefinery, 0x2)
{
	// 引擎原本在这条指令之后的两条支路（必须自己显式跳，不能靠标志位）：
	//   0x73EB88  jne 0x73EDC0   ; bl ≠ 0
	//   0x73EB8E  test esi,esi   ; bl ＝ 0
	enum { Path_BlZero = 0x73EB8E, Path_BlNonZero = 0x73EDC0 };

	GET(FootClass* const, pFoot, EBP);
	GET(BuildingClass* const, pOriginalTarget, EAX);

	BuildingClass* const pAlliedRefinery = AlliedRefineryDock::GetRedirectTarget(pFoot);
	BuildingClass* const pResult = pAlliedRefinery ? pAlliedRefinery : pOriginalTarget;

	// 引擎接下来三条支路全都从 ESI 取"落点建筑"（0x73EB96 mov edx,[esi] /
	// 0x73EDC8 mov eax,[esi] / 0x73EC4D mov esi,eax）。
	// 0x73EB86 的 `mov esi,eax` 已被本钩子跳过，所以 ESI 由我们写。
	R->EAX(pResult);
	R->ESI(pResult);

	AlliedRefineryDock::Trace(pFoot, pOriginalTarget, pResult, pAlliedRefinery != nullptr, 1);

	return AlliedRefineryDock::ReadTypeFlag0xCD4(pFoot) ? Path_BlNonZero : Path_BlZero;
}

// ---------------------------------------------------------------------------
// 钩点 B：0x73EC4D —— 引擎【第二次】挑建筑（重试支路）之后，同样要改答案
//
//   为什么必须有这个钩子：矿车在矿田上、精炼厂隔好几格 → 第一次必然判定"太远"
//   → 走 0x73EC1F 重试支路 → 那里又调一次 FindDock（0x73EC41）。
//   只钩第一次的话，引擎在重试里照样拿到 0 → 0x73EC58 放弃 → 矿车原地不动。
//
//   0x73EC4D 是 `mov esi,eax`（2 字节），紧随其后是 0x73EC4F `dec ecx`（重试计数器的
//   还原）、0x73EC50 `test esi,esi`、0x73EC52 `mov [0xa8e7ac],ecx`、0x73EC58 `je 0x73EF77`。
//   所以这里回 **0x73EC4F**（钩点 + 2）让后续原封不动执行：计数器照常还原、
//   `test esi,esi` 拿的是我们写好的 ESI，标志位由它自己产生 —— 没有任何坑。
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x73EC4D, FootClass_HarvestReturn_PreferAlliedRefinery_Retry, 0x2)
{
	enum { Continue = 0x73EC4F }; // → dec ecx ; test esi,esi ; je 0x73EF77 ; 继续算距离

	GET(FootClass* const, pFoot, EBP);
	GET(BuildingClass* const, pOriginalTarget, EAX);

	BuildingClass* const pAlliedRefinery = AlliedRefineryDock::GetRedirectTarget(pFoot);
	BuildingClass* const pResult = pAlliedRefinery ? pAlliedRefinery : pOriginalTarget;

	R->EAX(pResult);
	R->ESI(pResult);

	AlliedRefineryDock::Trace(pFoot, pOriginalTarget, pResult, pAlliedRefinery != nullptr, 2);

	return Continue;
}

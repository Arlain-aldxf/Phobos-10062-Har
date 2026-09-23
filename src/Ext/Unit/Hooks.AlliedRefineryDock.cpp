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
//     0073E6DB  cmp ecx, 4                 ; ecx = MissionStatus
//     0073E6EA  jmp [ecx*4 + 0x73EFAC]     ; 跳表：0→0x73E6F1 1→0x73E931 2→0x73EB2C 3→0x73EE8A 4→0x73EEA6
//
//     MissionStatus == 2 → 0x73EB2C：
//     0073EB2C  mov  ecx,[ebp+0x5A4]       ; FootClass::Destination（见【二】）
//     0073EB32  test ecx,ecx
//     0073EB34  je   0x73EB5A
//     0073EB36  test bl,bl                 ; bl = UnitTypeClass+0xCD4（类型上的一个开关字节）
//     0073EB38  je   0x73EB5A
//     0073EB49  call [edx+0x528]           ; 有目的地时：确认这目的地还有效吗
//     0073EB55  call 0x4DF0D0              ; 无效 → AbortMotion()（清 Destination/unknown_5A0）
//     0073EB5A  mov  eax,[ebp+0x5A4]       ; ★ Destination
//     0073EB60  test eax,eax
//     0073EB62  jne  0x73EF77              ; 已经有目的地 → 本帧什么都不做
//     0073EB68  mov  ecx,[ebp+0x6C4]       ; UnitTypeClass*
//     0073EB73  add  ecx,0x3E8             ; &UnitTypeClass::Dock（TypeList<BuildingTypeClass*>）
//     0073EB7E  call [eax+0x528]           ; ★★ EAX = FindDock(&Type->Dock, 0, 0)
//     0073EB84  test bl,bl                 ; ★★★ 我们钩这里
//     0073EB86  mov  esi,eax
//     0073EB88  jne  0x73EDC0              ; bl≠0 分支
//     0073EB8E  test esi,esi
//     0073EB90  je   0x73EC1F
//     0073EB96  ...                        ; 算"我"到 ESI 的距离
//     0073EC19  jle  0x73EE51              ; 够近 → 0x73EE51 开始对接
//     0073EC1F  ...                        ; 太远 → 走停机坪（0x73ECDF 起）
//     0073EE51  push esi / push 2 / call [eax+0x278]   ; 开始对接
//     0073EE68  mov  [ebp+0xBC],3          ; MissionStatus = 3
//
//     下一帧 MissionStatus == 3 → 0x73EE8A：
//     0073EE8F  push 7                     ; Mission::Enter
//     0073EE93  call [edx+0x1E8]           ; QueueMission(Enter) —— 进厂、卸货
//
//   也就是说：**"挑哪座建筑 → 开过去 → 进厂卸货"这一整条流水线，引擎自己全都有，
//   而且能跨地图距离工作**（太远时它会把 Destination 设成停机坪那一格）。
//   长征方没有自家精炼厂时它之所以卡住/崩，是因为这条流水线的**第一个输入**没了：
//   FindDock 只在自己的 Dock 清单（[HARV]/[CMIN] 的 Dock= 那几个建筑类型）里、
//   按**自己的阵营**找建筑，找不到就返回 0 → 整条流水线空转 → 矿车原地不动。
//   （旧版曾经在这里写出"垃圾指针"，其实那是我们自己钩子破坏了标志位/指令边界造成的，
//     不是引擎返回垃圾 —— 见【四】。）
//
//   所以我们**只需要在流水线入口把答案换掉**：
//   钩 0x73EB84，把 EAX/ESI 换成盟友精炼厂，然后照原样走引擎的分支。
//   不再自己去 SetArchiveTarget / QueueMission / SetDestination ——
//   那些"自己下命令"的做法会被引擎的 Harvest 状态机反复冲掉（实测多轮都是这个死法）。
//
// ---------------------------------------------------------------------------
// 【二、字段偏移（这一条是本次最重要的更正，前几轮一直搞错了）】
//
//   · FootClass::Destination  = FootClass + 0x5A4  ← 引擎在 Harvest 里读的就是它
//       YRpp/FootClass.h:175  AbstractClass* Destination;
//       0x4DF0D0（FootClass::AbortMotion）清的就是 [+0x5A0] 与 [+0x5A4]。
//
//   · TechnoClass::ArchiveTarget = TechnoClass + 0x218
//       YRpp/TechnoClass.h:448  void SetArchiveTarget(AbstractClass*) { JMP_THIS(0x70C610); }
//       反汇编 0x70C610 只有一句：8B 44 24 04  mov eax,[esp+4]
//                              89 81 18 02 00 00  mov [ecx+0x218],eax   ← 写的是 0x218，不是 0x5A4
//
//   → 前几轮把 [ebp+0x5A4] 当成 ArchiveTarget，于是"守卫条件"全建立在错误字段上：
//     探针里 pFoot->ArchiveTarget（= 0x218）打印出矿田地址（非空），
//     而被钩的那段代码里 [ebp+0x5A4]（= Destination）其实是空的 —— 两者根本不是一回事。
//     ArchiveTarget 对矿车来说确实也用来记矿田（YRpp 注释原文），但**不是**这段代码读的字段。
//
// ---------------------------------------------------------------------------
// 【三、为什么必须显式跳分支，不能靠标志位】
//
//   我们钩掉的是 0x73EB84 的 `test bl,bl`（2 字节）。这条指令被挪走后，
//   紧随其后的 `jne 0x73EDC0`(0x73EB88) 读到的就是 C++ 代码留下的**垃圾标志位**。
//   （本会话前几轮在同一个坑里摔了两次：0x73EB84 的 test bl,bl、0x73EB32 的 test ecx,ecx。）
//
//   唯一可靠的做法：钩子里**自己判断、自己跳**。
//     bl ＝ UnitTypeClass + 0xCD4（引擎在 0x73E6DE 读的那个字节），
//     照原值决定回 0x73EDC0（bl≠0）还是 0x73EB8E（bl＝0），语义与引擎完全一致。
//   （0x73EB86 的 `mov esi,eax` 也一并被跳过，所以 ESI 由我们自己写。）
//
//   bl 这个字节 YRpp 没有命名，但它的作用只是"分流两条距离判断支路"，
//   我们**不解释它、只照抄它**，因此没有语义风险。
//
// ---------------------------------------------------------------------------
// 【四、走过的弯路（别再重走）】
//
//   · 钩 FootClass::Find_Dock(0x4DEE80) 的出口 —— 0x4DEE80 其实是
//     "在某一类建筑里找我方可用的那一座"（[vtable+0x52C]），不是整条流水线的入口，
//     钩它没有意义。
//   · 只写 EAX / 只写 ESI / 两个都写 —— 都无效或崩。原因有二：
//     ① 写 EAX 会被 0x73EB86 的 `mov esi,eax` 覆盖（如果那条指令还在）；
//     ② 更根本的是：**只换寄存器并不会让引擎主动开过去**，
//        而这段代码恰好是"换掉答案后引擎就会自己开过去"的地方 —— 关键在钩点，不在寄存器。
//   · SetArchiveTarget / QueueMission(Harvest/Enter) / SetDestination 自己下命令 ——
//     和引擎的 Harvest 状态机抢时间线，实测：原地不动、开过去不倒矿、抖动、卡在矿里。
//   · 用 Mission::Enter 而不是 Harvest —— 对矿车是错的（Phobos 自己挂了
//     0x74312A 的钩子把 Enter 改成 Harvest，见 Hooks.Harvester.cpp）。
//     本方案不再碰任务，所以这条坑自然绕开了。
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
}

// ---------------------------------------------------------------------------
// 钩点：0x73EB84 —— 引擎刚问完"我该去哪座精炼厂"，我们在这里改答案
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

	// ---- 状态变化记录（诊断用；只在状态改变时写一行）----
	// 只记变化 + 每行 fflush，所以"崩了也不丢"，且不刷屏。
	{
		static FILE* s_log = nullptr;
		static DWORD s_lastKey = 0xFFFFFFFF;
		static int   s_lines = 0;

		const DWORD curMission = static_cast<DWORD>(pFoot->GetCurrentMission());
		const DWORD curStatus = static_cast<DWORD>(pFoot->MissionStatus);
		const DWORD linked = (pFoot->HasAnyLink() && pFoot->GetNthLink(0) == pResult) ? 1 : 0;
		const DWORD redirected = pAlliedRefinery ? 1 : 0;
		const DWORD key = (curMission << 20) ^ (curStatus << 8) ^ (linked << 4) ^ redirected;

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
					"#%-3d mission=%-3u status=%-3u storage=%.3f dest=%p archiveTarget=%p "
					"engineSaid=%p weReturned=%p redirected=%u linked=%u typeFlag=0x%02X\n",
					s_lines,
					static_cast<unsigned>(curMission),
					static_cast<unsigned>(curStatus),
					pFoot->GetStoragePercentage(),
					pFoot->Destination,
					pFoot->ArchiveTarget,
					pOriginalTarget,
					pResult,
					static_cast<unsigned>(redirected),
					static_cast<unsigned>(linked),
					AlliedRefineryDock::ReadTypeFlag0xCD4(pFoot) ? 0xFF : 0x00);
				fflush(s_log);
			}
		}
	}

	return AlliedRefineryDock::ReadTypeFlag0xCD4(pFoot) ? Path_BlNonZero : Path_BlZero;
}

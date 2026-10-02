#include <FootClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <HouseClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Utilities/Macro.h>
#include <Utilities/EnumFunctions.h>

#include <Ext/Script/Body.h>

#include <cstdio>   // 诊断用（出成品时连同下面的 Trace 一起删）

// ============================================================================
// 10062 配套：为【原始的满载返程目标选取规则】添加【非所有者所属方】的支持
//
// ---------------------------------------------------------------------------
// 【维护者的原话 —— 本文件的唯一依据】
//
//   「或许不应该从循环打断的角度入手，而是应该去看原始的满载返程目标选取规则，
//     尝试为其添加非所有者所属方的支持。」
//
//   逐句对应到代码：
//     · "原始的满载返程目标选取规则" → Mission_Harvest 在 MissionStatus==2 时调用的
//                                      FindDock / TryNearestDockBuilding(0x4DF040 / 0x4DEE80)
//     · "非所有者所属方"             → 矿车【自己 House】以外的、与之同盟的 House
//     · "不应该从循环打断的角度入手"  → 不去下命令、不去堵出口、不去维持循环
//
// ---------------------------------------------------------------------------
// 【一、原始规则长什么样（反汇编 0x4DEE80，YR 1.001）】
//
//   它做的事只有一件：
//     "在这一类建筑里，找一座我能用的，返回它"
//
//   而它遍历的名单，是【矿车自己 House 的建筑列表】：
//
//     004DEE93  8b8e1c020000   mov ecx,[esi+0x21c]    ; esi = 矿车 → 矿车自己的 HouseClass*
//     004DEEB2  8b961c020000   mov edx,[esi+0x21c]    ; 再一次：矿车自己的 House
//     004DEEBA  8b4278         mov eax,[edx+0x78]     ; House+0x78 = 建筑个数
//     004DEED4  8b486c         mov ecx,[eax+0x6c]     ; ★ House+0x6C = 【这个 House 的】建筑列表
//     004DEED7  8b3ca9         mov edi,[ecx+ebp*4]    ; 逐座遍历
//
//   ⇒ 长征方没有自家精炼厂 → 名单为空 → 返回 0 → 满载返程流水线空转 → 矿车原地不动。
//     这就是"缺什么"的全部。
//
// 【二、因此，本文件只做一件事】
//
//   保留原规则，另外补上"自己 House 找不到时，去盟友 House 的建筑列表里找"。
//
//     改之前：  自己House → 找到？→ 返回它 / 找不到 → 返回 0
//     改之后：  自己House → 找到？→ 返回它            （原版行为，完全不变）
//                        └ 找不到 ↓
//               盟友House → 找到？→ 返回它            ★ 只加这一层
//
//   不改的：对接（0x65A970）、卸货（0x73E3xx）、循环 —— 均无阵营判定，无需改动。
//
// 【三、为什么"循环"不需要我们写】
//
//   改好之后引擎自己会转：
//     挑到盟友精炼厂 → 开过去 → 对接 → 进厂 → 卸货 → 回去采矿 → 采满 → 又落到这里
//   循环是引擎满载返程状态机自带的，我们只是把它"挑不出来"这一处治好了。
//
// 【四、怎么保证"只影响 10062 的矿车"】
//
//   四个条件全满足才接管，缺一即完全放行原版行为：
//     ① 是矿车（UnitTypeClass::Harvester）
//     ② 满载（GetStoragePercentage() >= 0.999）
//     ③ 所属小队的脚本里启用了 10062（ScriptExt::IsTeamUsingMoveEnterAction）
//     ④ 场上找得到"盟友的、不是自家的、活着的"精炼厂
//   ③ 就是"10062 控制对应矿车"的开关本身 —— 未被 10062 影响的矿车，脚本里没有 10062，
//   条件为假，走的完全是原版逻辑，一个字节都不改。
//
// ============================================================================

namespace AlliedRefineryDock
{
	// ------------------------------------------------------------------------
	// 门控：这辆矿车此刻该不该获得"非所有者所属方"的支持？
	// 条件全部来自维护者那句话的语义，没有额外猜测。
	// ------------------------------------------------------------------------
	static bool ShouldSupportNonOwner(FootClass* pFoot)
	{
		if (!pFoot)
			return false;

		// ① 是矿车
		UnitTypeClass* const pUnitType = abstract_cast<UnitTypeClass*>(pFoot->GetTechnoType());

		if (!pUnitType || !pUnitType->Harvester)
			return false;

		// ② 满载（只有满载才有"回厂倒矿"这件事）
		if (pFoot->GetStoragePercentage() < 0.999)
			return false;

		// ③ 所属小队的脚本里启用了 10062
		return ScriptExt::IsTeamUsingMoveEnterAction(pFoot->Team);
	}

	// ------------------------------------------------------------------------
	// 非所有者所属方的候选中枢
	//   · 必须是别的阵营（排除自己）
	//   · 必须与之同盟
	//   · 必须是活着的、在场的精炼厂类建筑
	//   · 类型必须与调用方要找的 arg1 一致（与原规则的匹配口径完全相同）
	//   · 取最近的一座
	//
	//   ⚠️ 如实标注一处【有意的简化】：
	//      原规则在"距离相同"时还会看一个占用标志（BuildingClass+0x3D3），
	//      用来避免选中"停靠位已满"的建筑。本实现暂未复刻这一步 ——
	//      因为该字段在 YRpp 里没有对应命名，猜名字等于引入猜测。
	//      影响范围：只在"多座距离相同的盟友精炼厂"时可能选中已满的那座；
	//      而引擎在后续对接失败时会自行重试（0x73EE62 jne 0x73EC1F），所以不会死锁。
	//      若实测发现卡顿，再回来处理这里，不预先猜。
	// ------------------------------------------------------------------------
	static BuildingClass* FindNonOwnerDock(FootClass* pFoot, BuildingTypeClass* pDockType)
	{
		if (!pFoot || !pFoot->Owner)
			return nullptr;

		BuildingClass* pBest = nullptr;
		int bestDistance = 0;

		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			HouseClass* const pHouse = HouseClass::Array.GetItem(i);

			if (!pHouse || pHouse == pFoot->Owner)
				continue;                                     // 排除自己

			// 非所有者所属方 —— 不与自己同盟的直接出局
			if (!pFoot->Owner->IsAlliedWith(pHouse))
				continue;

			for (BuildingClass* const pBuilding : pHouse->Buildings)
			{
				if (!pBuilding || !pBuilding->IsAlive || !pBuilding->IsOnMap)
					continue;

				BuildingTypeClass* const pType = pBuilding->Type;

				if (!pType)
					continue;

				// 与原规则的匹配口径一致：类型必须就是调用方要找的那一类
				if (pType != pDockType)
					continue;

				if (!pType->Refinery && !pType->DockUnload)
					continue;                                 // 得是能倒矿的建筑

				const int distance = pFoot->DistanceFrom(pBuilding);

				if (!pBest || distance < bestDistance)
				{
					pBest = pBuilding;
					bestDistance = distance;
				}
			}
		}

		return pBest;
	}

	// ------------------------------------------------------------------------
	// 原始规则的完整副本（按调用约定），供钩子调用
	//
	//   ⚠️⚠️ 入口地址是 0x4DEE91，不是 0x4DEE90 —— 这一字节之差曾导致一次崩溃。
	//      反汇编边界（已逐条核对，不要再手数）：
	//        004DEE89  8bf1            mov  esi, ecx        (2B)
	//        004DEE8B  8b88f80d0000    mov  ecx,[eax+0xdf8] (6B)  ← 结束于 0x4DEE91
	//        004DEE91  57              push edi             (1B) ← ★ 函数体真正起点
	//      0x4DEE90 是上面那条 6 字节指令的**最后一个字节**，
	//      从那里进入会读到错位的操作数（实测表现为读 [0+0x2C] → 访问违例）。
	//
	//   ⚠️ 调用约定必须是 __fastcall，不能是 __cdecl：
	//      · 原函数体用 `ret 0x10` —— 由【被调方】清 3 个栈参
	//      · 参数顺序是 (this, arg1, arg2, arg3)，this 走 ECX
	//      __fastcall 正好匹配。x86 下没有 __thiscall 可显式写，__fastcall 等价可用。
	//
	//   ✅ 寄存器安全：函数体的尾声会 `pop edi/esi/ebp` 并 `add esp,0x2c`、
	//      成对还原它自己压的栈；且它【从不写 EBX】。所以调用方寄存器不受影响。
	// ------------------------------------------------------------------------
	using OriginalTryNearestDockBuilding = BuildingClass*(__fastcall*)(
		FootClass*, BuildingTypeClass*, DWORD, DWORD);

	static constexpr DWORD TryNearestDockBuilding_Body = 0x4DEE91;

	// ------------------------------------------------------------------------
	// 自己 House 里有没有"这一类建筑"？
	//
	// 用途：先问一句"原规则能不能自己搞定"。能搞定就完全不接管，
	//       让原版逻辑原样跑 —— 自家有精炼厂的场景因此零扰动。
	//
	// 判据刻意做得比原规则**宽松**（只看类型，不看对象标志/占用）：
	//   宁可误判为"有"（于是交还原版），也不误判为"无"（于是抢过来）。
	//   最坏情况只是"没帮上忙"，绝不会改变原版行为。
	// ------------------------------------------------------------------------
	static bool OwnHouseHasDockBuilding(FootClass* pFoot, BuildingTypeClass* pDockType)
	{
		if (!pFoot || !pFoot->Owner || !pDockType)
			return false;

		for (BuildingClass* const pBuilding : pFoot->Owner->Buildings)
		{
			if (pBuilding && pBuilding->Type == pDockType)
				return true;
		}

		return false;
	}

	// ------------------------------------------------------------------------
	// 诊断探针（出成品时删掉）
	//
	//   ★ 关键设计：在【钩子入口】就无条件记录，而不是等到门控之后。
	//     上一版的教训：探针放在门控后面，结果"钩子没被调用"和"门控返回 false"
	//     在日志里长得一模一样，白白多花一轮编译去猜。
	//     现在每一条都把五个门控判据的真实值打出来，一眼就能看出卡在哪。
	//
	//   记录策略：前 40 条全记（覆盖"钩子被调用了多少次"这个事实），
	//             之后每 200 条记一次（避免刷屏，但持续可见）。
	// ------------------------------------------------------------------------
	static void Trace(FootClass* pFoot, BuildingTypeClass* pDockType,
		bool ownHas, bool supported, BuildingClass* pEngineSaid, BuildingClass* pWeReturned)
	{
		static FILE* s_log = nullptr;
		static int   s_calls = 0;

		++s_calls;

		if (s_calls > 40 && (s_calls % 200) != 0)
			return;

		if (!s_log)
		{
			s_log = fopen("probe10062.log", "w");

			if (s_log)
			{
				fprintf(s_log, "=== 10062 non-owner dock support trace (v2) ===\n");
				fprintf(s_log, "列说明: calls=钩子被调用次数 type=arg1(要找的建筑类型)\n");
				fprintf(s_log, "        storage=装载率 ownHas=自己House有该类建筑? supported=门控\n");
				fprintf(s_log, "        engineSaid=原规则结果 weReturned=我们补的结果 dest=最终目的地\n");
				fflush(s_log);
			}
		}

		if (!s_log)
			return;

		const char* const typeName = (pDockType && pDockType->ID) ? pDockType->ID : "?";

		fprintf(s_log,
			"#%-4d storage=%.3f ownHas=%u supported=%u type=%s "
			"engineSaid=%p weReturned=%p dest=%p mission=%u status=%u\n",
			s_calls,
			pFoot ? pFoot->GetStoragePercentage() : -1.0f,
			static_cast<unsigned>(ownHas ? 1 : 0),
			static_cast<unsigned>(supported ? 1 : 0),
			typeName,
			pEngineSaid, pWeReturned,
			pFoot ? pFoot->Destination : nullptr,
			pFoot ? static_cast<unsigned>(pFoot->GetCurrentMission()) : 0u,
			pFoot ? static_cast<unsigned>(pFoot->MissionStatus) : 0u);
		fflush(s_log);
	}
}

// ---------------------------------------------------------------------------
// 【唯一的改动点】原始满载返程目标选取规则的入口
//
//   钩点 0x4DEE80，拷贝长度 0xB —— 逐条核对过的指令边界（别再手数）：
//     004DEE80  83ec2c          sub  esp,0x2c            (3B) → 83
//     004DEE83  8b442430        mov  eax,[esp+0x30]      (4B) → 87
//     004DEE87  55              push ebp                 (1B) → 88
//     004DEE88  56              push esi                 (1B) → 89
//     004DEE89  8bf1            mov  esi,ecx             (2B) → 8B
//     004DEE8B  8b88f80d0000    mov  ecx,[eax+0xdf8]     (6B) → 91
//                                                        合计 0xB，落在 0x4DEE8B 边界上
//   ⇒ 所以拷贝长度取 **0xB**，而不是 0xA（0xA 会切进 `mov esi,ecx` 中间，
//     这正是上一次崩溃的直接原因：Syringe 日志里那条
//     "Failed to decode instruction at 0x004DEE89 … faulty return 0 hook at 0x004DEE80"）。
//
//   两条返回路径：
//     · 返回 0        → 执行 trampoline 里的序言，再回到 0x4DEE8B 继续原版逻辑。
//                      **序言只跑这一遍**，栈与寄存器都是原版的，最安全。
//     · 返回 ResumeAt → 跳过 trampoline，直接进函数体（序言被跳过）。
//
//   我们的策略是**尽量走"返回 0"**：先问一句"自己 House 有没有这一类建筑"，
//   有就完全交给原版（连一次调用都不多）。只有"自己家确实没有"时才接管。
//
//   ⚠️ 调用函数体时入口必须是 0x4DEE91（见上面 API 声明处的说明），
//      不是 0x4DEE90 —— 后者是 `mov ecx,[eax+0xdf8]` 的最后一个字节。
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x4DEE80, FootClass_TryNearestDockBuilding_SupportNonOwner, 0xB)
{
	enum { ResumeAt = 0x4DEE91 };   // 钩点 + 0xB 再往后 6 字节：函数体真正的起点

	GET(FootClass*, pThis, ECX);
	GET_STACK(BuildingTypeClass*, pDockType, 0x4);
	GET_STACK(DWORD, arg2, 0x8);
	GET_STACK(DWORD, arg3, 0xC);

	// ---- 探针：入口无条件记录，把五个判据的真实值全打出来 ----
	const bool ownHas = AlliedRefineryDock::OwnHouseHasDockBuilding(pThis, pDockType);
	const bool supported = AlliedRefineryDock::ShouldSupportNonOwner(pThis);

	// ---- 第一步：原版能搞定吗？能 → 完全不管 ----
	if (ownHas)
	{
		AlliedRefineryDock::Trace(pThis, pDockType, ownHas, supported, nullptr, nullptr);
		return 0;                       // 走 trampoline，原版逻辑一字不改
	}

	// ---- 第二步：这是"满载返程、且小队脚本带 10062"的矿车吗？不是 → 也不管 ----
	if (!supported)
	{
		AlliedRefineryDock::Trace(pThis, pDockType, ownHas, supported, nullptr, nullptr);
		return 0;                       // 同样交还原版
	}

	// ---- 第三步：让原规则自己再走一遍（可能因为占用等原因它另有答案）----
	using OriginalFn = AlliedRefineryDock::OriginalTryNearestDockBuilding;
	BuildingClass* const pEngineSaid =
		reinterpret_cast<OriginalFn>(AlliedRefineryDock::TryNearestDockBuilding_Body)(
			pThis, pDockType, arg2, arg3);

	// ---- 第四步：补上"非所有者所属方" ----
	BuildingClass* const pAlly = pEngineSaid
		? nullptr
		: AlliedRefineryDock::FindNonOwnerDock(pThis, pDockType);

	AlliedRefineryDock::Trace(pThis, pDockType, ownHas, supported, pEngineSaid, pAlly);

	if (pEngineSaid)
	{
		R->EAX<BuildingClass*>(pEngineSaid);
		return ResumeAt;                // 原版自己能挑到 → 用它的
	}

	R->EAX<BuildingClass*>(pAlly);
	return ResumeAt;
}

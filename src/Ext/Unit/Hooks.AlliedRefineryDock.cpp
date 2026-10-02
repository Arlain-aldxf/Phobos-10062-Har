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
	//   定义在 0x4DEE90（序言 sub esp,0x2c; mov eax,[esp+0x30] 之后的函数体）
	//
	//   ⚠️ 调用约定必须是 __fastcall，不能是 __cdecl：
	//      · 原函数体用 `ret 0x10` —— 由【被调方】清 3 个栈参
	//      · 参数顺序是 (this, arg1, arg2, arg3)，this 走 ECX
	//      __fastcall 正好匹配：ECX = 第 1 个参数，其余右到左入栈，被调方清栈。
	//      （x86 下没有 __thiscall 可显式写，__fastcall 是等价且可用的写法。）
	// ------------------------------------------------------------------------
	using OriginalTryNearestDockBuilding = BuildingClass*(__fastcall*)(
		FootClass*, BuildingTypeClass*, DWORD, DWORD);

	static constexpr DWORD TryNearestDockBuilding_Original = 0x4DEE90;

	// ------------------------------------------------------------------------
	// 诊断探针（出成品时删掉）
	//   只记"状态发生变化"的行，每行 fflush —— 崩了也不丢，且不刷屏。
	//   判据看 dest：引擎有没有为这辆矿车真的设出目的地。
	// ------------------------------------------------------------------------
	static void Trace(FootClass* pFoot, BuildingClass* pEngineSaid, BuildingClass* pWeReturned,
		bool supported, bool hitNonOwner)
	{
		static FILE* s_log = nullptr;
		static DWORD s_lastKey = 0xFFFFFFFF;
		static int   s_lines = 0;

		const DWORD mission = static_cast<DWORD>(pFoot->GetCurrentMission());
		const DWORD status = static_cast<DWORD>(pFoot->MissionStatus);
		const DWORD hasDest = pFoot->Destination ? 1u : 0u;
		const DWORD key = (mission << 16) ^ (status << 8) ^ (hasDest << 7)
			^ ((pEngineSaid ? 1u : 0u) << 6) ^ ((pWeReturned ? 1u : 0u) << 5)
			^ ((supported ? 1u : 0u) << 4) ^ ((hitNonOwner ? 1u : 0u) << 3);

		if (key == s_lastKey || s_lines >= 80)
			return;

		s_lastKey = key;
		++s_lines;

		if (!s_log)
		{
			s_log = fopen("probe10062.log", "w");

			if (s_log)
			{
				fprintf(s_log, "=== 10062 non-owner dock support trace ===\n");
				fflush(s_log);
			}
		}

		if (s_log)
		{
			fprintf(s_log,
				"#%-3d mission=%-3u status=%-3u storage=%.3f dest=%p "
				"engineSaid=%p weReturned=%p supported=%u hitNonOwner=%u\n",
				s_lines, static_cast<unsigned>(mission), static_cast<unsigned>(status),
				pFoot->GetStoragePercentage(), pFoot->Destination,
				pEngineSaid, pWeReturned,
				static_cast<unsigned>(supported ? 1 : 0),
				static_cast<unsigned>(hitNonOwner ? 1 : 0));
			fflush(s_log);
		}
	}
}

// ---------------------------------------------------------------------------
// 【唯一的改动点】原始满载返程目标选取规则的入口
//
//   钩点选 0x4DEE80，拷贝长度 0xA —— 正好覆盖原函数 6 条序言指令
//   （sub esp,0x2c / mov eax,[esp+0x30] / push ebp / push esi / mov esi,ecx / push edi），
//   落在一条完整指令边界上（0x4DEE8A），不会切进指令中间。
//
//   进入时寄存器布局 = 原函数被调用时的布局：
//     ecx = 矿车（this），[esp+4] = arg1，[esp+8] = arg2，[esp+0xC] = arg3
//   而原函数体（0x4DEE90）自己会再做一遍 `sub esp,0x2c` 并补上 push ebp/esi/edi，
//   所以我们直接以 __fastcall 语义调它 —— 参数与栈的顺序天然对上，无需手工搬栈。
//
//   ✅ 不存在"寄存器污染"问题：
//      原函数体里的 `pop edi/esi/ebp` 会成对还原它自己那三次 push；
//      而它【从不写 EBX】（全文只读 [ebx+…]），所以调用方的 EBX 也不会被动。
//      因此这里不需要额外保存/恢复任何寄存器，直接调用即可。
//
//   🔴 必须显式返回 ResumeAt(0x4DEE8A)，不能 `return 0`：
//      DEFINE_HOOK 的 `return 0` 语义是"**执行 trampoline 里打包好的原指令**"。
//      而 trampoline 会执行那 5 条序言（sub esp,0x2c / push ebp / push esi / push edi）
//      然后跳回 0x4DEE8A —— 可我们已经调过 0x4DEE90（它自己做过同样的序言并已 ret），
//      序言执行两遍 + 手工填过 3 个栈参 ⇒ 栈立刻错位。
//      ⇒ 所以这里**跳过 trampoline**，直接返回序言之后的位置。
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x4DEE80, FootClass_TryNearestDockBuilding_SupportNonOwner, 0xA)
{
	enum { ResumeAt = 0x4DEE8A };   // 钩点 + 0xA：序言之后、函数体第一条指令

	GET(FootClass*, pThis, ECX);
	GET_STACK(BuildingTypeClass*, pDockType, 0x4);
	GET_STACK(DWORD, arg2, 0x8);
	GET_STACK(DWORD, arg3, 0xC);

	// ---- 先按原规则走一遍（自己 House）----
	using OriginalFn = AlliedRefineryDock::OriginalTryNearestDockBuilding;
	BuildingClass* const pResult =
		reinterpret_cast<OriginalFn>(AlliedRefineryDock::TryNearestDockBuilding_Original)(
			pThis, pDockType, arg2, arg3);

	if (pResult)
	{
		// ★ 自己家有 → 原版行为，完全不变
		R->EAX<BuildingClass*>(pResult);
		return ResumeAt;
	}

	// ---- 自己 House 里没有 —— 补上"非所有者所属方" ----
	const bool supported = AlliedRefineryDock::ShouldSupportNonOwner(pThis);
	BuildingClass* pAlly = nullptr;

	if (supported)
		pAlly = AlliedRefineryDock::FindNonOwnerDock(pThis, pDockType);

	AlliedRefineryDock::Trace(pThis, pResult, pAlly, supported, pAlly != nullptr);

	R->EAX<BuildingClass*>(pAlly);
	return ResumeAt;
}

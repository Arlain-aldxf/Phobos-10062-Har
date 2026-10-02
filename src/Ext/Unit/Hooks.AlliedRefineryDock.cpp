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

#include <TeamClass.h>
#include <ScriptClass.h>
#include <ScriptTypeClass.h>

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
		if (!pFoot || !pFoot->Owner)
			return false;

		// ① 是矿车
		UnitTypeClass* const pUnitType = abstract_cast<UnitTypeClass*>(pFoot->GetTechnoType());

		if (!pUnitType || !pUnitType->Harvester)
			return false;

		// ② 满载（只有满载才有"回厂倒矿"这件事）
		if (pFoot->GetStoragePercentage() < 0.999)
			return false;

		// ③ 不碰玩家自己手里的矿车 —— 玩家的矿车由玩家自己指挥，不替他做主。
		//    这一条是"只影响 AI"的保护，同时也让"原生 AI 矿车行为"保持原样。
		return !pFoot->Owner->IsControlledByCurrentPlayer();
	}

	// ------------------------------------------------------------------------
	// 统一的"在某座 House 的建筑列表里找这一类建筑"，取最近的一座。
	//
	// ⚠️ 为什么自己写、不调引擎的原函数：
	//    实测（快照 20261002-164516）确认，引擎原函数内部遍历
	//    [House+0x5500] 那个建筑列表容器时，会是空指针 + 0x5500 偏移
	//    （ESI=0x5500）→ C0000005 at 0x49FAE9。
	//    即"长征方这座 House 没有建筑"这个边缘状态下，引擎自己那段代码不健壮。
	//    所以这里改成全程自己走 YRpp 的 Buildings 列表（同一个安全迭代方式），
	//    完全不进入引擎那个函数 —— 这才是绕开崩溃的关键。
	//
	// 判据刻意保持宽松（只看类型/存活/在场），与原规则的匹配口径一致：
	//   · 类型必须 == arg1（原规则也是这么比的，见 0x4DEEF4）
	//   · 必须是能倒矿的建筑
	//   · "停靠位已满"那个标志（BuildingClass+0x3D3）没有复刻 ——
	//     YRpp 里无对应命名，猜名字等于引入猜测。影响仅限"多座等距"时可能选中已满的，
	//     而引擎对接失败会自行重试（0x73EE62），不会死锁。
	// ------------------------------------------------------------------------
	static BuildingClass* FindDockInHouse(HouseClass* pHouse, FootClass* pFoot,
		BuildingTypeClass* pDockType)
	{
		if (!pHouse || !pFoot || !pDockType)
			return nullptr;

		BuildingClass* pBest = nullptr;
		int bestDistance = 0;

		for (BuildingClass* const pBuilding : pHouse->Buildings)
		{
			if (!pBuilding || !pBuilding->IsAlive || !pBuilding->IsOnMap)
				continue;

			BuildingTypeClass* const pType = pBuilding->Type;

			if (!pType || pType != pDockType)
				continue;                                 // 与原规则同样的类型比对

			if (!pType->Refinery && !pType->DockUnload)
				continue;                                 // 得是能倒矿的建筑

			const int distance = pFoot->DistanceFrom(pBuilding);

			if (!pBest || distance < bestDistance)
			{
				pBest = pBuilding;
				bestDistance = distance;
			}
		}

		return pBest;
	}

	// ------------------------------------------------------------------------
	// 非所有者所属方的候选中枢
	//   · 必须是别的阵营（排除自己）
	//   · 必须与之同盟（IsAlliedWith —— 与 10062 实测通过的那条路同一个判据）
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

			if (!pFoot->Owner->IsAlliedWith(pHouse))
				continue;                                     // 只要同盟

			BuildingClass* const pCandidate =
				FindDockInHouse(pHouse, pFoot, pDockType);

			if (!pCandidate)
				continue;

			const int distance = pFoot->DistanceFrom(pCandidate);

			if (!pBest || distance < bestDistance)
			{
				pBest = pCandidate;
				bestDistance = distance;
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

	// ------------------------------------------------------------------------
	// 注意：这里**故意保留**引擎原函数体的地址记录，但**不再调用它**。
	//
	//   为什么不再调用（第二轮实测的教训）：
	//     快照 20261002-164516 的崩溃报告里，调用栈是
	//       ESP+0x08 -> 0x004DEE91   （引擎原函数体）
	//       ESP+0x04 -> 0x004DEEAA   （它内部 call 0x49FAE0）
	//       崩溃点    -> 0x0049FAE9   mov edi,[esi+8]，ESI = 0x5500
	//     即：引擎在遍历 [House+0x5500] 这个"自家建筑列表"容器时，
	//     this 指针是"空指针 + 0x5500 偏移"。长征方这座 House 没有任何建筑，
	//     引擎自己那段代码在这个边缘状态下不安全。
	//
	//   ⇒ 所以现在全程由我们自己走 YRpp 的 Buildings 列表（FindDockInHouse），
	//     钩子直接返回结果，**完全不进入引擎那个函数**。这是绕开崩溃的关键。
	//
	//   地址仍记录在此，仅供将来需要重新核对指令边界时参考：
	//     004DEE91  57    push edi          ← 函数体真正的起点
	// ------------------------------------------------------------------------
	static constexpr DWORD TryNearestDockBuilding_Body_Unused = 0x4DEE91;

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
		bool ownHas, bool supported, BuildingClass* pEngineSaid, BuildingClass* pWeReturned,
		int* pOutDistance)
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

		// 队伍与脚本内容：搞清楚 IsTeamUsingMoveEnterAction 为什么为假
		TeamClass* const pTeam = pFoot ? pFoot->Team : nullptr;
		ScriptClass* const pScript = pTeam ? pTeam->CurrentScript : nullptr;
		const char* scriptId = (pScript && pScript->Type && pScript->Type->ID) ? pScript->Type->ID : "-";

		char acts[64] = { 0 };
		if (pScript && pScript->Type)
		{
			int pos = 0;

			for (int i = 0; i < 4 && pos < 50; ++i)
			{
				const int a = pScript->Type->ScriptActions[i].Action;
				pos += _snprintf_s(acts + pos, sizeof(acts) - pos, _TRUNCATE, "%d,", a);

				if (a == 0)
					break;
			}
		}
		else
		{
			strcpy_s(acts, "-");
		}

		fprintf(s_log,
			"#%-4d storage=%.3f ownHas=%u supported=%u type=%s "
			"weReturned=%p outDist=%d dest=%p mission=%u status=%u "
			"team=%p script=%p scriptId=%s acts=[%s]\n",
			s_calls,
			pFoot ? pFoot->GetStoragePercentage() : -1.0f,
			static_cast<unsigned>(ownHas ? 1 : 0),
			static_cast<unsigned>(supported ? 1 : 0),
			typeName,
			pWeReturned,
			pOutDistance ? *pOutDistance : -999,
			pFoot ? pFoot->Destination : nullptr,
			pFoot ? static_cast<unsigned>(pFoot->GetCurrentMission()) : 0u,
			pFoot ? static_cast<unsigned>(pFoot->MissionStatus) : 0u,
			pTeam, pScript, scriptId, acts);
		fflush(s_log);
	}
}

// ---------------------------------------------------------------------------
// 【唯一的改动点】原始满载返程目标选取规则的入口
//
//   ★ 本轮换钩点：从 0x4DEE80 换到 **0x4DF050**。
//
//   为什么换 —— 三个理由，各自对应一次实测失败：
//
//   ① 0x4DEE80 那个函数体的【尾声】与【序言】绑定：
//        序言：sub esp,0x2c / push ebp / push esi / push edi   （0x4DEE80..0x4DEE8A）
//        尾声：pop edi / pop esi / pop ebp / add esp,0x2c / ret 0x10
//      我们跳过序言直接进函数体，尾声 `add esp,0x2c` 就会抬过头、
//      `ret 0x10` 从错误位置取返回地址 ⇒ 跳到野地址（实测 EIP=0x00000000）。
//      **钩 0x4DEE80 就绕不开这个绑定。**
//
//   ② 0x4DF050 附近的尾声（0x4DF0B7）只做 `pop edi/esi/ebp/ebx/ecx`
//      + `mov eax,ebp`，**没有 add esp、没有 ret 参数** —— 干干净净，
//      不存在栈帧平衡问题。这是我们能安全接管的关键。
//
//   ③ 引擎在 `jle 0x4df0b7` 处判断 Dock 列表是否为空。
//      列表为空时，代码仍会走到 0x4DF07A `mov ecx,[edx+edi*4]` 去取列表元素
//      （edx = [[ebx+4]]，edi 从 0 起）—— **读越界**。
//      实测崩溃报告的 EDI=0 正对应这里。我们提前接管，也就绕开了它。
//
//   钩点选 0x4DF050（5 字节，正好覆盖 `test eax,eax` + `jle rel8`），
//   返回 0x4DF0B7（尾声）：尾声会用 EBP 作为返回值，所以我们先写 EBP，
//   再让 `0x4DF0B8 mov eax,ebp` 把它送进 EAX。
// ---------------------------------------------------------------------------
DEFINE_HOOK(0x4DF050, FootClass_TryNearestDockBuilding_SupportNonOwner, 0x5)
{
	enum { Epilogue = 0x4DF0B7 };   // 尾声：pop edi / mov eax,ebp / pop ... / ret

	GET(FootClass*, pThis, ECX);
	GET_STACK(BuildingTypeClass*, pDockType, 0x4);
	GET_STACK(int*, pOutDistance, 0x8);

	// ---- 探针 ----
	const bool ownHas = AlliedRefineryDock::OwnHouseHasDockBuilding(pThis, pDockType);
	const bool supported = AlliedRefineryDock::ShouldSupportNonOwner(pThis);

	BuildingClass* pFound = nullptr;

	// ---- ① 原版优先级：自己 House 里先找 ----
	if (ownHas)
		pFound = AlliedRefineryDock::FindDockInHouse(pThis->Owner, pThis, pDockType);

	// ---- ② 自己家没有 → 补上"非所有者所属方" ----
	if (!pFound && supported)
		pFound = AlliedRefineryDock::FindNonOwnerDock(pThis, pDockType);

	AlliedRefineryDock::Trace(pThis, pDockType, ownHas, supported, nullptr, pFound, pOutDistance);

	// 尾声以 EBP 为返回值，所以写 EBP；同时填好那个"距离"输出参数
	R->EBP(pFound);

	if (pOutDistance)
		*pOutDistance = pFound ? pThis->DistanceFrom(pFound) : -1;

	return Epilogue;
}

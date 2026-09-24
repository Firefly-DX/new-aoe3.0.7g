#include "UsrAI.h"
#include <set>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <list>
#include <cstdlib>
#include <cstring>   // memset：集结/待命点的整图位图（见 ensure_rally_ok_map）
//
using namespace std;

tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

#include <vector>
#include <initializer_list>

// ============================================================================
// 【本次改动】原先这些类型 / 数据 / 声明全部写在 UsrAI.h 的 class UsrAI 里面。
//   现在 UsrAI.h 被清空成"只保留 4 个必须 override 的虚函数声明"的空壳类，
//   其余内容整体搬到本文件：
//     · 类内嵌套类型   → 文件级类型（struct / enum / typedef）
//     · 类内成员变量   → 文件级静态全局量（名字、初值完全不变）
//     · 类内成员函数   → 文件级静态自由函数（名字、函数体完全不变）
//   所有函数体、判断条件、常量取值一个字都没动，只是"AI 的数据不再位于
//   UsrAI 对象内部"。
// ============================================================================

// 唯一的 UsrAI 实例（MainWidget 里 new 出来的那一个）。
// 原来成员函数靠继承可以直接调用 HumanMove / calDistance / DebugText 等基类方法；
// 现在那些调用点变成了自由函数，必须通过这个指针转一手。
static AI *g_ai = nullptr;

// ---- 基类方法转发（名字与原来完全一致，所以下面所有函数体一个字都不用改）----
static int HumanMove(int SN, double dr, double ur) { return g_ai->AI::HumanMove(SN, dr, ur); }
static int HumanAction(int SN, int obSN) { return g_ai->AI::HumanAction(SN, obSN); }
static int HumanBuild(int SN, int t, int d, int u) { return g_ai->AI::HumanBuild(SN, t, d, u); }
static int BuildingAction(int SN, int a) { return g_ai->AI::BuildingAction(SN, a); }
static double calDistance(double d1, double u1, double d2, double u2) { return g_ai->AI::calDistance(d1, u1, d2, u2); }
static void DebugText(const std::string &s) { g_ai->AI::DebugText(s); }

// ==================== 任务类型（原 UsrAI 类内嵌） ====================
enum TaskType  { TASK_GATHER, TASK_BUILD, TASK_PRODUCE, TASK_UPGRADE };
enum TaskState { TASK_WAITING, TASK_ASSIGNED, TASK_DONE, TASK_FAILED };

struct Task {
    int id = -1;
    int type = TASK_GATHER;   // TaskType
    int priority = 0;         // 数字越小越先执行
    int state = TASK_WAITING;

    int resourceType = -1;    // 采集：目标资源类型
    int targetSN = -1;        // 已锁定资源/敌人 SN
    int buildingType = -1;    // 建造/生产：建筑类型
    int blockDR = -1, blockUR = -1; // 建造位置
    int farmerSN = -1;        // 被分配的农民，-1 未分配
    int startFrame = 0;       // 分配帧号，用于超时
    int resendFrame = 0;      // 上次续建重发的帧号
    int resendCount = 0;      // 续建重发次数（超上限则判失败重排）
};

// 科技研发状态：同一个 Action 可用一次或两次（两级科技），用等级追踪
struct ResearchState {
    int buildingType = -1;   // 执行建筑类型
    int action = -1;         // BuildingAction 常量
    int maxLevel = 1;        // 1 = 单级；2 = 两级
    int level = 0;           // 已完成等级
    int pendingId = -1;      // 在研指令 id（-1 表示未在研）
    int pendingFrame = 0;    // 在研指令的下达帧
    int minPhase = 2;        // 这条科技最早可以在哪个阶段研发
    int food = 0, wood = 0, stone = 0, gold = 0;      // 一级资源门槛
    int food2 = 0, wood2 = 0, stone2 = 0, gold2 = 0;  // 二级资源门槛
    int deadlineFrame = 0;   // 必须升完的帧号
    int urgentFromFrame = 0; // 从这一帧开始插队冲刺
    const char *name = "";
};

// ==================== 行为树 ====================
enum class BTStatus { Success, Failure, Running };

// 黑板：节点共享的上下文
struct BTContext {
    tagInfo *info = nullptr;
};

struct BTNode {
    const char *btName = "";
    virtual ~BTNode() {}
    virtual BTStatus tick(BTContext &ctx) = 0;
};
typedef std::shared_ptr<BTNode> BTNodePtr;

// 组合节点：依次尝试，任一成功即成功（备选方案）
struct BTSelector : BTNode {
    std::vector<BTNodePtr> children;
    BTStatus tick(BTContext &ctx) override;
};
// 组合节点：依次执行，任一失败即失败（步骤链）
struct BTSequence : BTNode {
    std::vector<BTNodePtr> children;
    BTStatus tick(BTContext &ctx) override;
};
// 叶子节点：cond（条件）与 action（动作），至少提供一个
struct BTLeaf : BTNode {
    std::function<bool(BTContext&)> cond;     // 条件，可选
    std::function<bool(BTContext&)> action;   // 动作，可选
    BTStatus tick(BTContext &ctx) override;
};

// ==================== 全局数据（原 UsrAI 的成员变量，名字与初值完全不变） ====================
static std::vector<Task> taskQueue;
static int nextTaskId = 0;
static int phase = 0;                // 阶段状态机：1冲铜器 2发展军事 3反攻
static bool huntStarted = false;     // 打猎开关（人口/木头到位后锁存，开了一直开）
static bool berryPhase = true;       // 浆果阶段：城边那几丛采完就结束
static bool berrySeen  = false;      // 是否已见到过城边的浆果丛
static int farmTarget = 0;           // 目标农田数（= 打算派去种田的人数）
static std::unordered_map<int,int> farmHolder;      // 农田 SN → 采集它的村民 SN

static int convertTargetSN = -1;     // 待转化的敌方单位 SN
static int convertStuckFrame = 0;    // 上次检查"祭司是否卡在转化目标上"的帧
static double convertStuckDR = 0, convertStuckUR = 0;
static int towerFocusSN = -1;        // 箭塔集火目标 SN（仇恨标记，锁定后不切换）
static int scoutCheckFrame = 0;                  // 上次卡住检查的帧号（祭司）
static double scoutCheckDR = -1, scoutCheckUR = -1;
static int scoutUnitCheckFrame = 0;                  // 上次卡住检查的帧号（侦察兵）
static double scoutUnitCheckDR = -1, scoutUnitCheckUR = -1;
static int scoutUnitOrderFrame = 0;                  // 侦察兵移动指令上次下达帧（节流）
static bool scoutEverMade = false;   // 是否已经有过侦察兵（全局只造一个）
static double scoutLastDR = 0, scoutLastUR = 0;      // 侦察兵最后已知位置
static bool scoutSeenAlive = false;                  // 本阶段见过活着的侦察兵吗

static int homeSpotX = -1, homeSpotY = -1;   // 回村落脚点块坐标
static int homeSpotTry = 0;                  // 找落脚点的尝试次数（卡住时向外扩）
static int priestOrderFrame = 0;             // 祭司移动指令上次下达帧（节流用）
static int healTargetSN = -1;                // 正在治疗的伤兵 SN

// 祭司环形探路的"已选过路点"表
static unsigned char scoutSeen[505][505] = {{0}};

// ---- 祭司：环形广度优先 ----
static int ringRadius = 0;                   // 当前正在搜索的环半径（块）
static int ringIndex = 0;                    // 当前环上的路点下标

// ---- 侦察骑兵：DFS ----
static double scoutHeadDR = 0, scoutHeadUR = 0;  // 当前探索方向（单位向量）
static int scoutSideSign = 1;                    // 到目标角之后左右扫的方向（+1/-1），走不通就翻面
static int curTargetX = -1, curTargetY = -1;     // 上一次给出的 DFS 目标格
static std::unordered_map<long long,int> dfsBad;  // DFS 目标黑名单：格子 → 解禁帧号

static int arrowTowerTarget = 1;        // 目标箭塔数量
static int towerPeak = 0;               // 曾经拥有过的最多箭塔数
static bool arrowTowerResearched = false;   // 箭塔科技是否已研发
static int arrowTowerResearchId = -1;   // 箭塔科技研发指令 id（-1 = 未在研）
static int arrowTowerResearchFrame = 0; // 上面那条指令的下发帧（超时重试用，见 bt_sync）
static std::unordered_map<int,int> towerTargetSN;  // 箭塔 SN → 已下达的集火目标 SN
static int towerOrderFrame = 0;                    // 上次对箭塔下令的帧号
static int lastTowerFocusSN = -1;                  // 上次下达的集火目标 SN
static int towerAggroFrame = 0;                    // 当前集火目标"开始被箭塔打"的帧号
// ---- 修塔（前两波打完之后派 1 个村民去修最惨的那座塔）----
static int repairFarmerSN   = -1;        // 正在负责修理的村民（-1 = 还没派）
static int repairTargetSN   = -1;        // 正在修理的建筑 SN
static int repairOrderFrame = -1000000;  // 上次下修理指令的帧号（节流用）

// ==================== 第二阶段：军事（造兵 + 科技） ====================
static int armyTarget = 16;             // 目标军队规模（第三阶段自动提高）
static std::vector<ResearchState> researches;
static std::vector<int> researchBuildingUsed;   // 本帧已经下过研发单的建筑 SN

// ==================== 第三阶段：反攻 ====================
static int enemySiegeSN = -1;                        // 敌方武器工程厂 SN
static double enemySiegeDR = -1, enemySiegeUR = -1;  // 敌方武器工程厂细节坐标
static std::unordered_map<int,int> attackOrderSN;    // 单位 SN → 目标 SN
static int attackConvertSN = -1;                     // 祭司在反攻阶段正在转化的目标 SN

// ---- 第三阶段反攻状态机 ----
static int    assaultState = 0;
static int    assaultStageFrame = 0;      // 进入当前状态的帧
static bool   baitPushing  = false;       // 状态 2 拉锯：true = 正压上勾引线，false = 已退回诱杀线
static int    baitPhaseFrame = 0;         // 当前这一段（压上/退回）开始计时的那一帧
// “全队到位”的那一帧（0 = 本段还没到位）。拉锯计时从这一刻起算，见 ASSAULT_*_HOLD_MS。
static int    baitHoldFrame = 0;
// 勾引线缓存。**必须缓存 + 带滞回**：lineDR/lineUR 原本每帧重算，而它依赖
//   “**可见**的敌方塔”，迷雾里推进时会不断发现新塔、敌方还会重建塔
//   （enemyai.cpp 的 ifA 逻辑）⇒ 线会跳变 ⇒ noDeepen 跟着跳 ⇒ **全队落点抖**。
// 现在只在①状态换了段 ②敌营位置大变 ③当前线已经不安全 时才重算。
static double baitLineDR = -1, baitLineUR = -1, baitNoDeepen = 0;
static bool   baitLinePushing = false;
static double baitLineAnchorDR = 0, baitLineAnchorUR = 0;
static bool   enemyFarFound = false;      // 是否已记下"100 格外的敌方目标"
static double enemyFarDR = 0, enemyFarUR = 0;
static double stageDR = 0, stageUR = 0;   // 集结点/前线站位点

// ---- 前线集结区（7x7）的缓存：家 → 敌营 连线上那块空地 ----
static int    rallyBX = -1, rallyBY = -1;    // 集合点中心（块）；-1 = 还没找到
static int    rallyHalf = 0;                 // 铺开半径（rally_point 里按 RALLY_HALF 赋值）
static int    rallyFrame = -1000000;         // 上次重算的帧
static double rallyAnchorDR = 0, rallyAnchorUR = 0;  // 上次算法用的敌营位置
static double lastStageDR = -1, lastStageUR = -1;   // 上一次用的集结点（-1 = 还没用过）
static int    assaultLogFrame = 0;        // 反攻状态日志的上次输出帧
static int    siegePriestStuckFrame = 0;  // 转化阶段的祭司卡住检测
static double siegePriestStuckDR = 0, siegePriestStuckUR = 0;
static int    weakKillFrame = 0;          // 自裁弱兵的上次执行帧
// 最近一个被 demand_army 判了“自裁腾人口”的单位 SN。
// 【为什么必须有】BT 里 demand_army(第 5) 在 demand_attack(第 9) **之前**，
//   而内核 deduplicateInstructions 按**主体 SN** 去重、保留**最后**一条
//   ⇒ demand_attack 给同一单位下的任何指令（攻击/移动）都会把自裁顶掉，
//     而自裁本身还有 WEAK_KILL_INTERVAL_MS(3 秒) 节流 ⇒ **永远杀不掉**，
//     人口永远腾不出来、复合弓兵也就补不上。
//   demand_attack 看到这个 SN 就跳过它（那一个单位本帧不管）。
static int    weakKillSN = -1;
static std::unordered_map<int,int> unitStuckKey;    // 单位 SN → 上次采样的位置（打包）
static std::unordered_map<int,int> unitStuckFrame;  // 单位 SN → 上次采样的帧号
// 单位 SN → 上次“战斗位移”的帧（见 RANGED_STEP_GAP 的节流）。
// 【为什么需要】每帧重下移动指令会被内核 suspendRelation + 清路径，单位就在原地
//   拖动、永远走不到位（这个坑在集合点那儿已经踩过）。参考实现用的是
//   shooterJournal[JOURNAL_STEP_TICK_SLOT]，这里用一张表实现同一件事。
static std::unordered_map<int,int> unitStepFrame;
// 单位 SN → 上次下“攻击指令”的帧（控距开火里的重复下单节流，对应参考实现的
//   shooterJournal[JOURNAL_FIRE_TICK_SLOT]，间隔 25 帧）。
static std::unordered_map<int,int> unitFireFrame;
// 单位 SN 非 0 = 这个弓箭手正在“脱离接触”（拉扯的滞回状态，见 kite_archer_step）。
//   只按“距离 < 5 就退一步”判会来回抖：退一步后距离回到 9，下一帧立刻重下攻击指令，
//   内核又把人走回来 —— 净位移 0、还每次 suspendRelation 清路径，最后被追上。
static std::unordered_map<int,char> kiteRetreating;
// 单位 SN → 上次“家里迎战”下令帧（combat_tactic 第 3 节的节流）。
static std::unordered_map<int,int> defenseOrderFrame;
// 【2026-09-23】本帧已经"预订"出去的走位落点（(x<<12)|y → 单位 SN）。
//   同帧里十几个单位会各自选格，不记一下就会两个人都被派到同一格
//   （投石车溅射半径 0.5 格 → 同格的两个人一起挨）。demand_attack 开头清空。
static std::unordered_map<int,int> cellClaim;

static BTNodePtr btRoot;     // 行为树根节点
static BTContext btCtx;      // 行为树黑板

// ==================== 统计辅助缓存 ====================
static std::unordered_map<int,int> resSpots;   // 资源 SN → 可站格数（每帧缓存）
static int resSpotsFrame = -1;
static int woodCapCache = 0;                   // 全图伐木物理容量（每帧缓存）
static int woodCapFrame = -1;
static int gatherLogFrame = 0;                 // 每 5 秒报一次采集人力分配
static bool treeNearHome = false;              // "60 格内还有可砍的树吗"的每帧缓存
static int  treeNearFrame = -1;
static std::unordered_map<int,int> farmerOrderFrame;  // 村民 → 上次被派活的帧号
static std::unordered_map<int,int> escapeFrame;       // 村民危险撤离的上次下令帧

static int MAP[505][505] = {{0}};              // 建造占位图（>0 = 占用）
static std::unordered_map<int,int> badBuildSite;  // 被内核驳回过的建造位置（拉黑表）

// ==================== 函数前置声明（原 UsrAI 的类内声明区） ====================
static bool find_block(int x, int y, int dx, int dy);
static void bt_sync();
static void prune_farm_holders();
static bool build_site_ok(int x, int y);
static void mark_build_site_bad(int x, int y);
static int count_done(int type);
static int active_build(int btype);
static int active_gather(int rtype);
static int active_action(int btype, int action);
static int gatherers_on(int resSN);
static int res_stand_spots(int resSN);
static int wood_capacity();
static int wood_gather_limit();
static int pending_build_wood();
static bool has_resource(int rtype);
static bool res_too_far(int type, int blockDR, int blockUR);
static bool gather_spot_dangerous(double dr, double ur, int radiusBlocks = 0);   // <=0 = 用默认半径
static int  gold_demand();
static bool gold_needed();
static bool center_free();
static void demand_build();
static double nearest_dropoff_dist(int resType, double dr, double ur);
static bool hunt_dropoff_ready();
static void demand_produce();
static void demand_repair();
static void demand_gather();
static void demand_army();
static void init_researches();
static void request_research(ResearchState &r);
static bool compositeBowReady();
static bool compositeBowUrgent();
static bool rangeReservedForResearch();
static bool rushing_composite_bowman();
static void demand_research();
static bool home_center(double &dr, double &ur);
static double nearest_enemy_tower_dist(double dr, double ur);
static bool point_in_enemy_tower_range(double dr, double ur, double marginBlocks);
static int enemy_hunter_count();
static void record_enemy_positions();
static void demand_attack();
static void army_standby();
static bool block_is_water_side(int x, int y);
static bool find_free_spot_near(int cx, int cy, int r0, int r1, int &bx, int &by);
static bool block_is_standable(int i, int j);
static bool rally_ground_ok(int bx, int by);      // 集结/行军共用的“可走”判据（位图）
static bool next_ring_point(int &bx, int &by);
static bool next_dfs_point(tagArmy *walker, bool stuck, int &bx, int &by);
static bool get_defense_anchor(int &cx, int &cy);
static bool find_home_spot(int &bx, int &by, int attempt);
static void scout_retreat(tagArmy *priest);
static void recall_priest_home(tagArmy *priest);
static bool priest_heal(tagArmy *priest);
static void demand_scout();
static void bt_dispatch();
static bool build_margin_clear(int x, int y, int size);
static void sort_tasks();
static void assign_tasks();
static bool farmer_just_ordered(int sn);
static bool farmer_available(tagFarmer &f);
static void mark_farmer_order(int sn);
static bool on_build_task(int farmerSN);
static void recycle_tasks();
static void combat_tactic();
static bool enemy_near(double dr, double ur, double radius);
static bool bt_enemy_at_home();
static void build_behavior_tree();

tagInfo info;

// 建筑占地尺寸（块）。
// 【引擎真值】`MainWidget.cpp:2541 BuildingFundation[]` + `Building::setFundation()`：
//   · 房屋 / 箭塔 / 船坞 = FOUNDATION_SMALL(2) ⇒ **2x2**（碰撞盒 CRASHBOX_SMALL 也正好 2 格）；
//   · 其余（市中心/谷仓/仓库/农田/市场/兵营/马腈/靶场/武器工程厂）= FOUNDATION_MIDDLE ⇒ 3x3。
// 【用户 2026-09-24：“房屋大小设成 3*3，防止工人建好被卡死”】
//   引擎的**阻挡**确实是 2x2，但房屋的**贴图是 3x3**，于是原来按 2x2 规划会出事：
//   我们把农田/其它建筑排到房屋贴图的第 3 列/行上 ⇒ 村民站过去看起来就是“卡在房子里面”，
//   而且那栋新建筑的 3x3 与房屋贴图重叠、工人也挤不进去。
//   所以房屋按 **3x3 规划**（比引擎的阻挡多占 1 列/行）：预留得比内核更保守，
//   HumanBuild 永远不会因为“我们算得比内核宽”而被驳回；
//   代价是房屋需要更大一块空地（find_block 要 3x3、build_margin_clear 再要外圈 1 格）。
static int building_size(int type) {
    return (type == BUILDING_ARROWTOWER) ? 2 : 3;
}

// 各建筑消耗的木头（与 Development.cpp 里 buildCon 的数值对应）。
// 箭塔花石头，不占木头预算，返回 0。
// 注意：必须定义在 pending_build_wood() 之前，否则那边会报"未声明"。
static int build_wood_cost(int type)
{
    switch (type) {
    case BUILDING_HOME:     return BUILD_HOUSE_WOOD;
    case BUILDING_GRANARY:  return BUILD_GRANARY_WOOD;
    case BUILDING_STOCK:    return BUILD_STOCK_WOOD;
    case BUILDING_MARKET:   return BUILD_MARKET_WOOD;
    case BUILDING_ARMYCAMP: return BUILD_ARMYCAMP_WOOD;
    case BUILDING_RANGE:    return BUILD_RANGE_WOOD;
    case BUILDING_STABLE:   return BUILD_STABLE_WOOD;
    case BUILDING_COLLAGE:  return BUILD_COLLAGE_WOOD;
    case BUILDING_FARM:     return BUILD_FARM_WOOD;
    default:                return 0;
    }
}

// 【已删除 blocks_home_corridor / HOME_CORRIDOR_WIDTH】
//   原来这里是“给市中心正南固定预留一条 2 格宽通道”的规则，用来防止
//   “四面八方各落一块 3x3 建筑、四边邻格被填满、市中心被彻底围死”。
//   用户 2026-09 改成**建筑布局网格**（见下面 BUILD_GRID_PITCH）之后它就不需要了：
//   网格间距固定 4 格 = 3 格建筑 + 1 格缝，**每两块建筑之间天然有 1 格路**，
//   村民可以从任意一条缝绕到市中心旁边交货，不存在“被围死”这回事。
//   （房屋的“必须留缝”仍然由 build_margin_clear 保证。）

// 敌方第三波发动帧（约 14 分钟，默认 25fps → 21000 帧），之后转入反攻
static const int ATTACK_START_FRAME = 21000;

// 【用户 2026-09-24：“第二波祭司优先转化方阵兵”】
//   第二波的发动帧 = 9:00。出处：enemyai.cpp 的三波时刻
//   FAT=6000(4:00) / SAT=13500(9:00) / TAT=21000(14:00)，
//   而本文件上面的 ATTACK_START_FRAME 就是 TAT。
//   所以“第二波窗口” = [WAVE2_START_FRAME, ATTACK_START_FRAME)。
static const int WAVE2_START_FRAME = 13500;

// 采集任务的哨兵资源类型：表示"采集农田"（农田是建筑而非 tagResource，
// 且 BUILDING_FARM 与 RESOURCE_GAZELLE 数值都是 4，故用独立哨兵值区分）
static const int GATHER_FARM = 100;

// 防守触发半径（块）：只有可见敌军进入我方市镇中心这个半径内，才启用
// 「箭塔拉仇恨 + 祭司转化」的防守战术。这样祭司探图途中扫到远处零散兵群
// 或远处的来袭波次时，不会被误触发去防守，探图与防守不再互相打扰。
static const int HOME_DEFEND_RADIUS = 35;

// 探图时遇敌的撤离参数：
//   SCOUT_THREAT_RADIUS —— 敌人进入祭司这个半径内就撤离；
//   SCOUT_FLEE_STRIDE   —— 每次撤离目标取背离方向这么多格。
static const int SCOUT_THREAT_RADIUS = 12;
static const int SCOUT_FLEE_STRIDE = 15;

// ---------------- 祭司：前期"找家附近资源点" ----------------
// **祭司探图的唯一目的是把家附近的资源点探出来**（不是去画地图）。
// 为什么必须探：AI 只能看到"已探索"的资源（info.resources 里只有见过的），
// 而开局几件要紧事全指着这些：
//   · 石/金在哪 —— 决定村民去哪挖、离得远要不要在矿边补一座仓库；
//   · 城边浆果丛在哪 —— 前 6 个村民要采浆果；
//   · 瞪羚在哪 —— 人口 12 / 木 300 之后开打猎。
// 实测（任意图都一样，直到你换新图）：这三样通常都在离市中心 11~20 格内，所以：
//   · 环半径封顶 SCOUT_RING_MAX(40) 格 —— 再外面不是祭司的活；
//   · **不要搞"见着浆果/石/金就提前收工"**（试过，坑）：这三样往往在 20 格内，
//     祭司走到半径 ~18 就全看见了，1 分半就把自己判“探完”回村了，
//     而半径 20~40 那一圈（更多矿/树/瞪羚）全黑着——经济后面要找矿时抓瞎。
//     所以老老实实扫到 3.5 分钟（或撞上 SCOUT_RING_MAX），正好回家应付 4 点那波。
//   · **不再有“绝望探图”**（2026-09 用户要求）：侦察骑兵阵亡就说明那个方向有敌兵，
//     部队凭它最后的位置直接冲过去（见 record_enemy_positions），祭司只管家里的事。
static const int SCOUT_RING_MAX = 40;     // 祭司探图的环半径上限（格）

// 祭司环形探路参数：
//   从营地外 SCOUT_RING_START 格开始，每圈按弧长均匀布点
//   （间距 ≈SCOUT_ARC_SPACING），一圈扫完半径 +SCOUT_RING_STEP。
//   STEP 调小 → 扫得更细（更慢）；调大 → 更粗更快。
//   为什么祭司必须环形（BFS）：它速度只有 2.24，一路直线扎到地图另一头的话，
//   家里 4 分钟那一波敌袭它赶不回来防守/转化；环形扫得匀、始终在营地附近，
//   配 3.5 分钟回村正好。
static const int SCOUT_RING_START = 10;
static const int SCOUT_RING_STEP = 8;

// 每圈路点的目标间距（弧长，格）。点上个数按弧长算：n = 2πr / 间距。
// 必须按弧长算——营地几乎总在地图角落，"以营地为圆心的整圆"有大半在图外；
// 若每圈固定取 16 个点，图内那段弧上只剩 3~4 个点、间距二十多格，
// 看起来就成了"沿直线往外扩、只扫 1/4 圈"。
static const int SCOUT_ARC_SPACING = 8;

// ---------------- 侦察骑兵：后期"找敌军大本营" ----------------
// 【先搞清楚迷雾这件事，否则一定写错】
//   玩家 0 拿到的 theMap 是**带迷雾**的：Core::InitPlayerMap() 把所有格子设成
//   height=-1 / MAPPATTERN_UNKNOWN，之后只有"已探索"的格子会被 updateCommon()
//   刷成真实地形（Core.cpp: for(auto&p:explored) playerMap[p.x][p.y]=...）。
//   于是 block_is_standable() 对未探索格一律 false（type 不是草地/沙漠/浅滩，
//   而且 height<0 会命中 block_is_water_side）。
//   → **目标格绝不能是"未知格"**：那样每个方向都会因"不可站立"被否掉，
//     侦察兵被锁死在已探索区里原地打转（"侦察骑兵卡死、没记录敌方位置"的根因）。
//   正确做法：盯着**前沿格**——已知可站立、且邻域挨着 MAPPATTERN_UNKNOWN 的格子，
//   走到它就能把迷雾往前推一格。这就是这片地图上的"深度优先"：
//     · 候选打分 = 继续朝 scoutHeadDR/UR（dot × HEAD_W）+ 越远越好（dist × FAR_W）
//                  − 朝家降权 → 永远优先"朝当前方向一路扎到底"；
//     · 前方没有前沿时才转向（打分自然会落到侧/后方的前沿）；
//     · 卡住（目标走不到）→ **只把那个格子拉黑一段时间**，方向保留、同方向换一格
//       再试；只有前方真的一格可走的都没有了才允许回头。
//   **"朝家降权"是找到大本营的关键**（营地在地图角落、敌营在斜对面），别删。
static const int SCOUT_DFS_RANGE = 40;      // 找前沿的搜索半径（格）
static const int SCOUT_DFS_MIN   = 2;       // 比这还近的前沿不选（避免原地抖）。
                                            // **别调大**：侦察兵贴在前沿边上时，
                                            // 正前方的新前沿就在 3~6 格外，调大就把
                                            // "继续往前扎"否掉了，变成只能沿边走。
static const double SCOUT_DFS_HEAD_W = 2.0; // "继续朝当前方向"权重
static const double SCOUT_DFS_FAR_W  = 1.0; // "越远越好"权重
// 【身后重罚（用户 2026-09-21：“只有一条路走不通的时候才会走回头路，
//   其余沿着原方向继续探”）】dot < 0 = 目标在身后。取 4.0 保证：
//   只要正前方还有**任一**前沿格，它就一定赢过身后所有候选 ——
//   身后的候选 dot×2.0 ≤ 0、越远项 ≤ 1.0，再减 4.0 必为负；
//   而正前方的候选最差也有 0。所以“回头”只会在前方真的没路时发生。
// 【为什么必须单独列一项】原来“卡住”时会把 scoutHead 清零，于是下一轮所有
//   候选的 dot 都是 1.0，“朝前”这项变成常数，评分只剩“越远越好” ——
//   它会挑一个**身后**最远的格，看上去就是“一直在走回头路”。
static const double SCOUT_DFS_BACK_W = 4.0;
static const int SCOUT_BAD_MS = 30000;      // 走不到的目标拉黑多久（毫秒）
static const double SCOUT_BACK_HOME_PENALTY = 0.5;  // 朝家方向降权
// ---- 探图目标角（用户 2026-09-22 的新探图策略）----
// 【用户原话：“这一次的问题是没把敌方大本营探出来，现在改变探图策略，
//   朝我方大本营最远的角探路，然后再向两侧探”】
//   目标角 = **离我方市镇中心最远的地图角**。营地在地图角落、敌营在斜对面
//   （map/map1~3 实测都是），所以那个角就是敌营所在的那一带。
//   离目标角还超过 SCOUT_GOAL_NEAR 格 → 头方向 = 对准目标角；
//   进到 SCOUT_GOAL_NEAR 格以内 → 头方向 = 垂直于“家 → 角”的方向，
//   先扫一侧，那一侧走不通（目标被拉黑）就翻到另一侧（“向两侧探”）。
static const int SCOUT_GOAL_NEAR = 30;

// 复合弓科技的时间要求（用户硬性要求：20 分钟前升完）。
//   它在研发清单里排得靠后（前面一堆市场/兵营科技会先把食物木头花掉），
//   而且靶场还和"造弓箭手"抢（demand_army 排在 demand_research 前面，
//   靶场一空就先把订单下走了）。所以给它一个"到点插队"的机制：
//   过了 urgentFromFrame 还没升完，就第一个下单 + 让靶场停止造兵。
//   留 COMPOSITE_BOW_MARGIN_MIN 分钟余量给"资源不够要等"和"建筑忙要等"。
static const int COMPOSITE_BOW_DEADLINE_MIN = 20;   // 必须在第 20 分钟前升完
static const int COMPOSITE_BOW_MARGIN_MIN = 3;      // 提前 3 分钟开始冲刺

// 建造中场"续建重发"：
//   BUILD_RESUME_MAX    —— 最多催几次，超过就认输（释放占位、重排新任务）；
//   BUILD_LOST_GRACE_MS —— 下单后多久还没看到地基，就认为指令丢了（重排）。
static const int BUILD_RESUME_MAX = 6;
static const int BUILD_LOST_GRACE_MS = 3000;

// 被内核驳回的建造位置拉黑多久（毫秒）。别调太长：很多驳回是临时的
// （有单位站在那儿、锚点瞪羚刚走开），过十几秒那块地其实能建。
static const int BAD_BUILD_SITE_MS = 15000;

// 反攻：兵力到这个规模就**不再等家里安全**，直接全军压上（见 demand_attack）。
// 反攻是唯一取胜手段，硬上限 30:00（GAME_LOSE_SEC），攒着兵在家拼消耗最亏。
static const int ATTACK_FORCE = 16;

// 复合弓科技升完之前，靶场最多造几个**普通**弓兵（用户 2026-09 反馈：
// “第三波来之前，在没有升级复合弓之前就生产了过多的弓箭手”）。
// 为什么必须卡：普通弓兵 射程 5 / 攻击 3 / 35 血；复合弓兵 射程 7 / 攻击 5 / 45 血，
// 而且升完科技后普通弓兵**不会自动升级**（Army.cpp 里 AT_BOWMAN 的 upgradable=false），
// 而靶场 30 秒才出一个兵 —— 前期猛造普通弓兵就是既烧 40 食物 + 20 木，
// 又把靶场占住，后面就凑不出推图要的 10 个复合弓兵。
// 留几个只为前两波提供最基本的远程火力（觉得防守变弱就往上调这个数）。
static const int BOWMAN_PRE_TECH_MAX = 2;

// 复合弓科技（180 食物）没升完之前，家里至少留这么多食物。
// 为什幺：村民一个 50 食物，一旦有人在远处被狮子/敌军杀掉，市镇中心就会一直
// 造人来补，食物全被这个无底洞吃掉，科技就永远点不出来（用户 2026-09 实测：
// “村民跑太远被杀了 → 市中心一直在造村民 → 复合弓科技点不出来”）。
// 两处生效：demand_produce（不造村民）与 demand_army 的冲刺窗口（不造兵）。
// 【2026-09 修正】原来是 200：但 demand_produce / demand_army 都排在
// demand_research **前面**（行为树 build → produce → army → research），
// 食物一到 200 就被它们先花掉 50/40，research 再看就只剩 150 < 180 ——
// 科技永远点不上（用户反馈“食物被消耗了但不知道去哪”，答案就是造村民）。
// 现在按“**科技成本 + 一个村民**”来留：冲刺窗口内还会额外一律停造
// （见 demand_produce / demand_army 里的 compositeBowUrgent() 判断）。
static const int TECH_FOOD_RESERVE = BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_FOOD + 50;
// 【木头版的同类预留 —— 用户 2026-09 反馈“复合弓科技还是被拖了”】
//   复合弓科技要 180 食 + **100 木**。食物那边早就靠 TECH_FOOD_RESERVE 挡着
//   （食物 <200 就不造村民/不造兵），可木头这边一直漏着：行为树顺序是
//   build → produce → army → research，每帧 demand_build 先把木头花掉，
//   而**农田是持续消耗**（内核一块田只认一个采集者、采完就自动删除，
//   所以 demand_build 一直在补建），木头常年压在 “BUILD_FARM_WOOD + 50”
//   这条线附近，永远到不了 100 —— 科技就一直被拖。
//   冲刺期内 demand_build 必须先扣下这么多木给科技，剩下的才允许建造。
static const int TECH_WOOD_RESERVE = 100;

// ============ 第三阶段反攻：集结 → 诱杀野战军 → 齐射拆箭塔 → 祭司转化 ============
// （全部在 demand_attack 里用，注释里写的“格”都是欧氏距离的块数）
//   ASSAULT_BOWMAN_MIN   —— 攒够几个复合弓兵才推图（用户 2026-09-21：**18 个**）；
//   ASSAULT_STAGE_DIST   —— 诱杀线在敌营外多少格（见常量区的定义，出处是
//                           enemyai.cpp 的 DEFENSE_ALERT_RANGE=20）；
//   TOWER_SAFE_MARGIN    —— 认定“在箭塔射程外”还要再多留几格余量。
// 为什么死死咬着“不进箭塔射程”：箭塔**真实射程是 10 格**（DIS_ARROWTOWER=7 加上
// 敌方升满的 谷仓升级/木材加工/工艺 +3，算法见 Development.cpp:110-124），
// 而我们复合弓兵只有 7 格 —— 站在射程边缘对射就是把血换成箭塔的血
// （箭塔 125 血、我们 45 血）。先清野战军、再用近战当肉盾上去拆才划算。
// （敌方位置怎么记见 record_enemy_positions；射程怎么算见 ENEMY_DIS_ADD_*。）
static const int ASSAULT_BOWMAN_MIN = 18;
// 【用户 2026-09-21：“保守一点，18 个复合弓开始攻，时间是够的”】阈值 10 → 18。
//   够不够时间的账：靶场 30 秒/个，18 个 ≈ **9 分钟连续生产**；硬上限 30:00，
//   所以即便 21:00 才开始攒也来得及。（嫌慢就多建一座靶场，产量直接翻倍。）
// 等不到 18 个时的**底线档**：
// 【用户 2026-09-22：“等待两分钟就开始攻太离谱了，最起码等到 23 分钟作为底线，
//   如果 23 分钟凑不齐就开始强攻”】
//   · 主力档（`composite >= ASSAULT_BOWMAN_MIN`）**不设时间上限** —— 凑够 18 个就走；
//   · 底线档：第 ASSAULT_FORCE_MIN(23) 分钟一到，**不管手上几个兵都开打**。
//   （原来那条“在集结点等 ASSAULT_WAIT_TIMEOUT_MS(2 分钟) 凑不到 18 个就把门槛
//     降到 ASSAULT_BOWMAN_FALLBACK(6) 个”已经整体删除：2 分钟太短、6 个也太少。）
static const int ASSAULT_FORCE_MIN = 23;
// 前线集结点 = **诱杀线**：距敌营多少格。
// 【2026-09-23 由 20 改成 24】依据 enemyai.cpp 的真实数值 `DEFENSE_ALERT_RANGE 20`：
//   守军把“距武器工程厂 20 格内”的玩家单位当合法目标，超出就各自回原位。
//   ⇒ 集结/诱杀线必须 > 20，否则**集结期就在慢性地把守军全勾出来**。
static const int ASSAULT_STAGE_DIST = 24;
// 敌方守军的**真实**迎击半径。
// **出处：enemyai.cpp `#define DEFENSE_ALERT_RANGE 20`**，判定在 `AssignDefense()`，
//   基准点是武器工程厂（就是我们的 tx/ty）。
//   ⚠ 以前写成 15（来自 word 文档）—— 那是 enemyai.cpp 里一个**没被使用的** MODE1 宏。
//     这个错误数字让勾引线白深了 6~7 格，改了两轮才定位到。
static const int ENEMY_ALERT_DIST = 20;
// 勾引线：踩进警戒半径 1 格。守军必须**自己走出来**才能打到我们 —— 一走就离开塔的掩护。
static const int BAIT_TRIGGER_DIST = ENEMY_ALERT_DIST - 1;   // 19
// ---- 拉锯的两段时长（毫秒）----
// 【用户 2026-09-22：“拉锯还是要拉啊，否则一次性拉太多兵会损伤很重”】
//   压上（BAIT_TRIGGER_DIST=19）↔ 退回诱杀线（ASSAULT_STAGE_DIST=24）。
// 【2026-09-23】计时**从“全队到位”那一刻起算**（baitHoldFrame），
//   不再从翻转那一刻算 —— 否则 8 秒里先花 ~2 秒走路，实际驻留只剩 6 秒。
static const int ASSAULT_PUSH_HOLD_MS    = 8000;
static const int ASSAULT_RETREAT_HOLD_MS = 8000;
// 【待命点（兵还没凑够时部队在哪等）】优先用“前线集合点”（村庄朝敌营方向
//   RALLY_AWAY_DIST 格的一块空地，见 rally_point）；找不到才退回下面这个老逻辑。
// 【一级集结点】还没凑够推图兵力时，集结点离我方防御锚点多少格。
//   用户 2026-09：“怀疑是集结在敌人视野里，建议在不占用市中心附近的情况下
//   在家附近空旷的地方集结”。
//   14 格：离市中心/箭塔够远（不占市中心周边那些留给农田/科技建筑的地面），
//          又足够近 —— 是“家附近”，部队几步就能聚齐，而且**完全不在敌人视野里**。
static const int RALLY_DIST = 14;
// ---- 前线集合点（村庄外 RALLY_AWAY_DIST 格）----
//   RALLY_AWAY_DIST     ：集合点离村庄（市镇中心）多少格。用户 2026-09-21：
//                         “到村庄 40 格外空地集合就好”。只在敌营更近时才收小。
//   RALLY_CLEAR_HALF    ：**中心周围这块必须全可站立**（3 → 7x7 = 49 格）。
//                         用户 2026-09-23：“集合点选的有问题，集合点周围有水域，
//                         下指令被判定不可达，卡死” ⇒ 只验中心那一格不够：
//                         单位自己那格站不住 / 被水隔开时，HumanMove 出去要么被
//                         引擎判不可达、要么 findPath 返回空，单位就原地不动。
//   RALLY_HALF          ：兵的**铺开半径**（2 → 5x5 = 25 格，一个格子站一个兵），
//                         从最中心那格起一圈圈往外**螺旋填充**（见 rally_slot_offset）。
//                         它放在上面那块 7x7 的正中间 —— 外面留一圈余量，
//                         保证每个单位周围都是空地，不会互相挤住、也走得出去。
static const int RALLY_AWAY_DIST    = 40;
static const int RALLY_CLEAR_HALF   = 3;
static const int RALLY_HALF         = 2;
static const int RALLY_ENEMY_MARGIN = 24;
// ---- 集合点搜索的两个补充条件（用户 2026-09-23：“为什么你会选一个旁边有树有水的集结点”）----
//   RALLY_SIDE_MAX   ：允许垂直于“家 → 敌营”连线向**两侧**各走多少格。
//                      原来只在连线上前后挪、横向最多 ±3，第一个合格就收 ⇒
//                      线上恰好紧贴树林/水边时就认了，旁边 5 格外的空地看着不用。
//                      现在横向铺开 ±10 格做真正的 2D 搜索（越界的点会被
//                      “必须在市中心与敌营之间”那条矩形判定剔除）。
//   RALLY_OPEN_GOOD  ：`rally_openness()` 的满分是 24（8 个方向 × 4/5/6 格三圈）。
//                      这两个是档位门槛：≥GOOD = 很开阔，≥OK = 还算开阔，
//                      低于 OK 就只能算“勉强合格（周围被林/水包着）”。
//                      选点是**先比档位、同档取更靠前**，所以宁可退几格也要挑开阔地。
static const int RALLY_SIDE_MAX      = 10;
static const int RALLY_OPEN_GOOD     = 20;
static const int RALLY_OPEN_OK       = 12;
// ---- 第 1/2 阶段（第三波之前）军队的村外待命点（见 army_standby）----
// 【用户 2026-09-22：“兵种挡住农民的路了！”→ 澄清“第三波之前：军队就在村里/
//   生产建筑旁边不动，村民出不去”】
//   第三阶段有“前线集合点”把部队拉到村外 40 格；但 14:00 之前 demand_attack
//   直接 return，**没有任何人指挥部队**，它们就停在生产建筑旁边
//   （靶场/马厩/兵营都在中心 8~12 格那一圈建筑带里，农田在 4~8 格）——
//   村民必须从那儿挤出去，于是整个村子被堵死。
//   ARMY_STANDBY_DIST 为什么是 20：已经完全出了建筑带；而**圆周越远越长**，
//   同样 7x7 的一块人占地，在 20 格的环上只占一小段弧，村民从两边都能绕过去。
//   不再往外选是为了“家里挨打能及时跑回来”（combat_tactic 一喊就跑）。
static const int ARMY_STANDBY_DIST = 20;
// 待命区的铺开半径（7x7 → 3），一个格子一个兵，由 rally_slot_offset 从中心往外排。
static const int ARMY_STANDBY_HALF = 3;
// 在几何猜测点周围多大半径内找“整块 7x7 都干净”的待命中心（块）。
// 【为什么要“找”】原来只验了中心那一格（block_is_standable），
//   7x7 里可以有树/水/矿 —— 用户 2026-09-23 看到的就是这个。
static const int ARMY_STANDBY_SEARCH_R = 12;
static const int ASSAULT_STAGE_RADIUS = 8;      // 单位离集结点多近算“就位”
// 前线集合点在几何落点周围多大半径内找一块“整块 7x7 都干净”的地（块）。
// 原来这条路**一格都没验**，所以部队会站在树里/水边。
static const int ASSAULT_STAGE_SEARCH_R = 10;
// 【2026-09 用户反馈“复合弓还没集结就冲上去了 / 骑兵跑得快、其他兵种没到就总攻”】
//   集结点原来只有 16 格，**比 ASSAULT_ENGAGE_DIST(20) 还近**：移速快的骑兵
//   一到集结点就直接锁定了 20 格内的敌方野战军冲上去；它一走，下面的 staged
//   （全员到位）就永远不成立，硬等到 ASSAULT_STAGE_TIMEOUT_MS 超时强推，
//   而移速慢的复合弓兵还在半路上 —— 这就是“没集结完就总攻”。
//   现在集结期靠下面这个**很紧的还手半径**卡住。
//   （另：诱杀线 ASSAULT_STAGE_DIST=24 现在 > ASSAULT_ENGAGE_DIST=20，
//     “把集结点挪到交战圈外”这条又成立了。）
static const int ASSAULT_STAGE_GUARD_DIST = 7;  // 集结期唯一允许还手的距离（格）
// 集结超时强推时，圈内至少要有这么多兵（否则推上去也是送；落后的大多是刚出生、
// 正在赶路的新兵，为它们无限等待不值得，但也不能一个没到就冲）。
// 【2026-09-22 用户反馈“兵力没集结到位就开打”】只有这一条远远不够：
//   18 个复合弓兵里到 6 个就全军压上，剩下 12 个还在半路 —— 等于拿 1/3 的兵
//   去打 5 座箭塔 + 30 个守军。所以下面还要再加一条“最多只允许
//   ASSAULT_STAGE_STUCK_MAX 个人掉队”，两条同时满足才允许超时强推。
static const int ASSAULT_STAGE_MIN_READY = 6;
// 超时强推时**容许掉队的上限**（比正常路径的 ASSAULT_STAGE_LAG_MAX(2) 松一点）：
//   超时强推本来就是给“有个别单位卡在墙角永远走不到”准备的，
//   不是给“大部队还没到”准备的。
static const int ASSAULT_STAGE_STUCK_MAX = 4;
// “就位”容许的掉队人数：第三阶段兵营一直在造复合弓，新兵会不断从城里往外走，
// 路上总要有人。兵营 30 秒出一个，路上最多一两个，所以容许 2 个。
static const int ASSAULT_STAGE_LAG_MAX = 2;
static const int ASSAULT_BACKSTEP = 4;          // 集结点落在塔射程里时每次后退几格
static const double TOWER_SAFE_MARGIN = 3.0;
// ---- 敌人科技的射程加成（2026-09 对照 word 文档 + Development.cpp:91-125 核实）----
// 【为什么不能直接用 DIS_***】word 文档（个人项目说明2026）明确：
//   “敌方的各种技术已经全部升完”。所以真实射程 = DIS_* + 科技加成，
//   Development::get_addition_DisAttack 的算法：
//     · 箭塔     ：谷仓升级(+1) + 木材加工(+1) + 工艺(+1) = **+3**（三个 if 都会命中）
//     · 弓箭类（armyClass == ARMY_ARCHER）：木材加工(+1) + 工艺(+1) = **+2**
//     · 投石兵   ：石矿开采(+1) = **+1**
//     · 投石车等攻城武器：**不吃任何射程加成**（源码注释：“攻城武器不在此列”）
//   所以箭塔真实射程是 **7 + 3 = 10 格**，不是 7 —— 拿 7 当射程会在 7~10 格之间
//   站着挨打。下面一律按“敌方满科技”算。
static const int ENEMY_DIS_ADD_TOWER   = 3;
static const int ENEMY_DIS_ADD_ARCHER  = 2;
static const int ENEMY_DIS_ADD_SLINGER = 1;
static const int ASSAULT_TOWER_RADIUS = 24;     // 敌营这个范围内的箭塔要拆掉
static const int ASSAULT_ENGAGE_DIST = 20;      // 单位主动交战的半径
// 【用户 2026-09-24】状态 3（拆塔）回退到状态 2（拉锯）的门槛：
//   · 至少要这么多敌方军队贴上来，才值得放弃拆塔、回头打人；
//   · 刚进状态 3 的头 15 秒一律不回退（先把眼前这座塔打掉）。
//   以前是 `fieldUnits > 0`（一个敌人就触发），
//   全军会莫名其妙从塔下走回勾引线“再集结一次”。
static const int ASSAULT_ROLLBACK_MIN     = 3;
static const int ASSAULT_TOWER_COMMIT_MS  = 15000;
// 投石车（祭司转化来的）站位要在**弓兵线之后**多少格
//   （用户 2026-09-24：“投石车太靠前，必须在弓箭手身后”）。
static const int SIEGE_BACK_DIST          = 6;
// 走位时的“阵位铺开半径”（2 → 5x5 = 25 格，一个格子一个兵）。
// 【2026-09-23】推进 / 勾引 / 后撤三个落点原来都是**一个坐标下给所有人**，
//   二十个兵往一个格子挤 ⇒ 两个人同格、被投石车一发溅射（半径 0.5 格）一起打掉。
//   改成用 spread_slot 按 SN 分格位后，这里就是那个方块的半径。
static const int ASSAULT_SPREAD_HALF = 2;
// 【2026-09-23 用户：“大概 10 个复合弓开始就不去了”】
//   spread_slot 原来写 `if (slot >= cells) slot = cells - 1;` —— 把第 25 个以后的人
//   全指向同一格；而第三阶段手里常有十几个前期造的方阵兵/骑兵（SN 更小，
//   先把 slot 0..13 占了），复合弓兵只剩 slot 14..24
//   ⇒ **第 11 个起就分不到格位、永远收不到移动指令**。故：不够就往外扩圈。
static const int SPREAD_EXTRA_RINGS = 3;        // 最多往外多扩 3 圈（2+3=5 → 11x11）
static const int ASSAULT_PROTECT_DIST = 8;      // 转化阶段部队停在敌营外几格（护祭司但不挡路）
// 塔拆完但**敌方祭司猎手**还没清掉时，等这么久就放弃等待（用户 2026-09：
//   “祭司不参与反攻，解决敌方祭司猎手后再到反攻区”）。
//   猎手可能一直缩在厂区里不出来，无限等下去就等于放弃转化 = 输，所以必须有这个兜底。
static const int ASSAULT_HUNTER_WAIT_MS = 60000;
static const int ASSAULT_STAGE_TIMEOUT_MS = 60000;  // 集结超时：有人到不了就别等了
// 压到勾引线之后等这么久还没人追出来 → 认为厂区守军已经打光，转入拆塔阶段。
//   要留出“全军从诱杀线（ASSAULT_STAGE_DIST=24）走到勾引线（BAIT_TRIGGER_DIST=19）
//   再观察一轮”的时间（约 5 格路程）。
static const int ASSAULT_BAIT_TIMEOUT_MS = 40000;
static const int WEAK_KILL_INTERVAL_MS = 3000;  // 自裁弱兵的节流（毫秒）

// ---- 远程兵的行动节流参数（旧的“控距开火” 已整体删除）----
//   威胁是敌方弓兵 → 撤到 它射程+5+2 格；威胁是箭塔 → 撤到 17 格外。



static const int    RANGED_STEP_GAP       = 20;   // 同一单位两次战斗位移的最小帧间隔
static const int    RANGED_FIRE_GAP       = 25;   // 同一单位重复下攻击指令的最小帧间隔

// ---- 弓箭手“拉扯”（用户 2026-09-24 规则，实现见 kite_archer_step）----
static const int KITE_FLEE_DIST    = 5;   // 离最近敌人小于这个距离就进入“脱离”（格）
static const int KITE_RETREAT_STEP = 4;   // 每次脱离朝家退几格

// 让箭塔先拉仇恨、再让祭司转化：塔开火后等这么久（毫秒）祭司才动手。
static const int CONVERT_AGGRO_DELAY = 1500;

// 祭司"在位"判定半径（块）：以防御锚点（箭塔，无塔则市中心）为圆心。
// 不能用市中心判定——祭司是被 recall_priest_home 派到**箭塔**下待命的，
// 箭塔建得离市中心远时，用市中心算距离会把已到位的祭司误判成"没到位"，
// 结果它站在塔下一动不动、永远不转化。
// 必须与 SCOUT_HOME_CALL_RADIUS 保持同一个值（两边职责相反，半径要一致）。
static const int PRIEST_ENGAGE_RADIUS = 30;
// 家里士兵迎战的下令节流（毫秒）。防“每帧重下”把攻击蓄力打断，见 combat_tactic 第 3 节。
static const int DEFENSE_ORDER_MS = 1000;

// 祭司优先转化的距离（块）：这个范围内的敌人能立刻上手，
// 超出后要给大额惩罚，免得祭司丢下脚边的敌人去追远处的（路上还会被反杀）。
static const int PRIEST_CONVERT_RADIUS = 12;

// 祭司转化目标打分时的“投石车优先”加成（单位：等效格数，越大越优先）。
// 为什么单独给这一档：敌方投石车射程 10 格 > 我方箭塔射程 7 格，
// 它站在塔打不到的地方拆我们的箭塔（一发 50 伤害，箭塔总共 125 血），
// 所以必须优先抢下来（转化成功就变成我们的战力，一箭双雕）。
// 取值要**小于**“正在咬祭司”那一档（-200），保证祭司的自卫优先级不受影响：
// 投石车在 12 格内时得分 ≈ 12-150 = -138，会赢过任何没在咬祭司的目标，
// 但一旦有敌人咬上祭司（≤ -200），还是先把它拉下来。
static const double PRIEST_CONVERT_SIEGE_BONUS = 150.0;
// 【用户 2026-09-24：“第二波祭司优先转化方阵兵”】
//   方阵兵 = **AT_HOPLITE**（学院造的 60 食 + 40 金的重步兵，
//   config.h 的 ACT_COLLAGE_CREATE_HOPLITE_NAME = "训练方阵兵(花费:60食物,40黄金)"）。
//   分值取 180：在 200（正在打祭司 = 自卫最优先）之下、150（投石车）之上 ——
//   只要不是“有人正咬着祭司”，第二波里祭司就先抢方阵兵。
//   只在第二波窗口（WAVE2_START_FRAME ≤ frame < ATTACK_START_FRAME）生效。
static const double PRIEST_CONVERT_PHALANX_BONUS = 180.0;

// 祭司"空余时间治疗"：这个时间点之前（分钟），只要家里没敌袭，
// 祭司回村待命时就顺便给伤兵回血（内核里祭司对友军执行 HumanAction 就是治疗）。
// 之后的战事密集，祭司交给 combat_tactic / demand_attack，不再单独治疗。
// 【用户 2026-09】12 → 13 分钟：治到 13 分钟就收手，回塔下待命。
static const int PRIEST_HEAL_UNTIL_MIN = 13;   // 分钟

// 治疗的追击上限：只治离祭司这个距离以内的伤兵，别为了回血跑遍全图。
static const int HEAL_MAX_DIST = 25;           // 块

// 敌方打到家时，祭司离防御锚点（箭塔/市中心）超过这个距离才把它叫回来
// （块）。已经在附近就交给 combat_tactic 专心转化，不再移动。
// 注意：必须与 combat_tactic 里的 PRIEST_ENGAGE_RADIUS 一致——树里 defense 在
// scout 之前跑，若这里半径更小，16~30 格之间的祭司会被回村指令覆盖掉同一帧
// 刚下的转化指令，看起来就是"祭司站着不转化"。
static const int SCOUT_HOME_CALL_RADIUS = 30;

// 祭司靠到防御锚点（箭塔/市中心）这个距离以内就算"已到位"，不再下移动指令。
// 不能用"距落脚点 <4 格"判定：塔边常挤满村民，祭司到不了那个精确格子，
// 会反复换点、在塔边来回徘徊。
static const int HOME_STAY_RADIUS = 6;

// ============ 开局经济：6 人采浆果 + 其余全砍树 + 先砍树后打猎 ============
// 食物固定人数：只采浆果，不派人打猎（浆果就在城边，打猎要跑远、来回搬肉）。
// 这里用"绝对人数"而不是百分比：开局只有 8 个村民，任何百分比算出来都是
// 2~3 人，永远攒不起冲铜器建筑链要的 545 木材。
static const int FOOD_GATHERERS = 6;

// 打猎（瞪羚）开闸条件：人口约 HUNT_START_POP、木头约 HUNT_START_WOOD 时开始。
// 开到闸之后锁存常开，不随木头存量上下波动来回切（否则打猎人数会反复增减、村民来回跑）。
// 【2026-09 用户要求：“减少伐木人口，给前期增加捕猎人口”】所以两个门槛都调低了：
// 8 人（开局初始人口）就能开猎；木头 150 是“拍完打猎仓库（120 木）还剩点”的量。
static const int HUNT_START_POP  = 8;
static const int HUNT_START_WOOD = 150;

// 开闸后打猎人数占村民总数的百分比（农田优先，剩下的名额才给打猎）。
static const int HUNT_PERCENT = 30;

// 木头富余 → 多派人打猎（用户要求："前期木头偏多，分一点去给打瞪羚的食物采集"）。
// 判据不用"木头存量"本身，而是**可自由支配的木头** = 存量 − 队列里还没建成的建筑
// 要花的木头（pending_build_wood）。这样冲铜器建筑链、补农田/房屋期间不会误判成
// "木头富余"把伐木的人抽走；等这些需求都满足了，多出来的人就转去打猎。
// （还受下面的"木材保底 WOOD_MIN_GATHERERS"兜着，伐木人数不会掉到保底以下。）
static const int HUNT_WOOD_SURPLUS   = 150;   // 自由木头超过这个数就算富余
static const int HUNT_SURPLUS_EXTRA  = 2;     // 富余时额外多派几个人去打猎
// 打猎人数上限与"仓库没好时的先遣队"：
//   HUNT_EARLY —— 打猎仓库还没到位时先派几个人去打（食物收入提前起来；
//                 代价是来回交肉要走远一点，所以人少一点把浪费控住）；
//   HUNT_MAX_GATHERERS —— 打猎人数硬上限。
static const int HUNT_EARLY = 2;
static const int HUNT_MAX_GATHERERS  = 6;     // 打猎人数上限
// 【用户 2026-09-24：“第一波多出的村民目前看来都去打猎了，请稍微分一部分给木头”】
//   第三波之前把打猎名额让出这么多人给伐木（打猎仍然至少保留 1 人）。
//   为什么前期木头更值钱：冲铜器链 545 木 + 靶场 150 + 农田 75/块 + 房屋 30/座，
//   而食物这边还有城边浆果（最多 6 人）+ 农田顶着；后期反过来（phase>=3 不让）。
static const int HUNT_EARLY_TRIM    = 2;

// 伐木人数软上限（用户要求："现在减少伐木人口"）。
// 人堆在树上收益递减：一棵树站不下几个人，挤在一起还砍不到
// （res_stand_spots 已经在控站位），而且伐木科技一到位
// （BUILDING_MARKET_WOOD_UPGRADE：采集速度 +50%、背包 +2）同样人数产木更快。
// 所以开猎之后把伐木人数软封顶在这里，多出来的人转去打猎；
// 下面的"木材保底"只往下压打猎人数，压不住时伐木会自然涨回来。
static const int WOOD_MAX_GATHERERS = 6;
// 【用户 2026-09-24：“请稍微分一部分给木头”】第三波之前的上限（比后期宽 2 人）。
//   不把它抬高，“分给木头”就落不了地：主流程的 wantWood 会被上面那个 6 封死，
//   多出来的人仍然全进打猎（或被兜底 2 抢走）。
static const int WOOD_MAX_GATHERERS_EARLY = 8;

// ============ 农田：后期食物主力 ============
// 内核里一块农田是"一次性资源建筑"（CNT_BUILD_FARM = 250 食物，成本 75 木），
// 而且只允许**一个**采集者（Building_Resource::isGathererAsLandlord 的地主判定），
// 采完会被内核自动删除（非 surplus 的资源建筑直接移除）。
// 所以规则是"一个村民对应一格农田"：想派 N 个人种田就得有 N 块农田，
// 由 demand_build 按人口持续补建。
static const int FARM_PER_POP = 3;   // 每多少个村民配 1 块农田
static const int FARM_MAX     = 8;   // 八宫格一共 8 个格位（硬上限）
// 【用户 2026-09-24：“第三波后农场减到 7 个，防止金矿卡死搬不进去”】
//   八宫格全铺满（8 块）时，最外那一圈容易把**去金矿的路**堵住 ——
//   金矿（StaticRes，2x2）常常就在村外不远处，村民绕不过去就“卡死搬不进去”。
//   所以第三波后只铺 7 块，给路留一格（前两阶段本来只有 2/4 块）。
static const int FARM_TARGET_LATE = 7;
// 【用户 2026-09-24：“前期农场建的太多”】前期的**阶段封顶**。
//   一块田 = BUILD_FARM_WOOD(75 木) + 30 秒工时，而它只有 CNT_BUILD_FARM(250) 食、
//   采完就被内核删掉，**我们还要再花 75 木补建** —— 而前 14 分钟这点木头要和
//   冲铜器建筑链(545 木)、复合弓科技(100 木)、第二座靶场(150 木)抢。
//   而且每块田常驻一个村民（地主制），等于同时吃木头和人力。
//   所以：工具时代 2 块够吃（还有城边浆果+打猎），进铜器加到 4 块，
//   扛过第三波（phase>=3）才开到 FARM_TARGET_LATE(7) —— 那时食物才是真瓶颈
//   （复合弓科技 180 食、复合弓兵 40 食/个），而且远处的采集点容易被清。
static const int FARM_CAP_TOOL   = 2;   // 工具时代上限
static const int FARM_CAP_BRONZE = 4;   // 铜器时代~14:00 上限
// 【2026-09 用户要求】后期农田不够：原来 20 个村民时 20/5 = 4 块，现在 20/3 = 6 块
// （正好顶到 FARM_MAX）。之所以要加：后期食物是瓶颈（复合弓科技 180、
// 复合弓兵 40/个、方阵兵 60/个），而远处的采集点又容易被狮子/敌军清掉，
// 靠家的农田才是稳定来源。

// ============ 后期分工 ============
// 木材保底人数：后期食物为主，但房屋 / 补仓库 / 农田本身 / 科技都还要木头。
// 名额不够时按"打猎 → 农田"的顺序往回缩。
static const int WOOD_MIN_GATHERERS = 4;

// 以市中心为锚点的建筑（市场/兵营/靶场/马厩/学院/箭塔…）的环形兜底起始环半径（块）。
// 【2026-09 用户要求】原来 6，现在 8：其它建筑要落在“外圈网格”上（见下面
//   BUILD_GRID_PITCH 的说明），环形兜底也得从外圈起，否则会跟八宫格农田抢位置。
static const int CENTER_BUILD_START_R = 8;

// ---- 建筑布局网格（用户 2026-09 要求）----
//   以市中心（3x3）为**中央格**，按“3 格建筑 + 1 格缝 = 4 格”的间距排成 5x5 网格
//   （偏移量 -2..+2，单位 = 本常量）：
//     偏移 (0,0)         → 市中心自己
//     偏移 (±1, ±1) 即八宫格，共 8 个 → **农田**（FARM_MAX = 8，但第三波后只用 7 个，
//       留一格给去金矿的路，见 FARM_TARGET_LATE）
//     偏移 (±2, ...) 即外圈，共 25-9 = 16 个 → 市场/兵营/靶场/马厩/学院/箭塔
//   好处：
//     ① 建筑之间**永远留 1 格缝**，村民能从缝里绕行，不会出现“被围死”；
//     ② 农田环绕市中心，采完走一条缝就能上交；
//     ③ 位置固定，不会东一块西一块；农田被内核“采完自动删除”后，补建**回到原来那一格**
//        （见 recycle_tasks 第 4 步：建筑一建成就把 MAP 占位释放掉，
//         所以那一格在农田被删掉之后又能重新被选中）。
//   不在网格上的：房屋（往地图边缘排居住带）、资源旁的谷仓/仓库（挨资源建）——
//   它们各有各的锚点，不受这套网格约束。
static const int BUILD_GRID_PITCH = 4;

// 打猎“划不划算”的半径：看得见的瞪羚里至少有一只离可用存放建筑（仓库/谷仓/市中心）
// 不超过这个距离（块），才按 HUNT_PERCENT 放开派猎 —— 否则村民大半时间都花在搬肉的路上。
// **它现在只是个“值不值得派”的门槛**：仓库不再由 AI 补建（用户要求“防止每天造仓库”），
// 太远的瞪羚不采就是了（见 hunt_dropoff_ready）。
static const int HUNT_DROP_RADIUS = 10;

// "食物断供"的例外：浆果吃光、又没有农田时只能靠打猎续命，
// 此时不必等人口/木头门槛；但开局这段帧内不启用，保证"开局不杀瞪羚"。
static const int HUNT_STARVE_MIN_FRAME = 750;   // ≈30 秒（默认 25fps）

// 浆果只在前期采：只采城边 BUSH_NEAR_RADIUS 格内那几丛（开局那几丛总在城边），
// 采完就结束浆果阶段——不跑远去追别的浆果丛。
// （半径是策略值、不依赖具体地图：哪怕新图的浆果更远，也交给农田/打猎，
//   反正 GATHER_MAX_DIST 也只允许 60 格。）
// 空出来的名额转给农田 / 打猎 / 伐木（具体由下面的配额逻辑决定）。
static const int BUSH_NEAR_RADIUS = 22;

// 箭塔数量目标（含开局自带的那座）：
//   工具时代 1 座 —— 只有开局自带的那座。**用户 2026-09 明确要求“第二座塔在铜器
//     以后建”**：工具时代的人力全留给冲铜器的木材链，不要为了 80 秒建塔工时
//     （TIME_BUILD_ARROWTOWER）和 1 个采石村民拖慢升级。
//   进铜器 2 座 —— 一进铜器就用**开局那 150 石**立刻起第二座（不用等采石）。
//   二三波之间 3 座 —— 第二座起来后继续备料 150 石（见 demand_gather 里的 needStone），
//     8:00 目标升到 3 就马上开工；就算进铜器晚于 8:00，也是“进铜器 → 建第二座
//     → 采石 ~1.5 分钟 → 建第三座”，第三波（14:00）前稳稳 3 座。
//   TOWER_TARGET_LATE 同时是“最多建几座”的硬顶：采石逻辑拿它判断“还要不要备料”。
// 多一座塔的作用：祭司在塔下转化时会被弓手白嫖，多一座塔能把仇恨拉走。
// 想再多多一座就把 LATE 改成 4（代价：150 石 + 村民 80 秒工时）。
static const int TOWER_TARGET_EARLY  = 1;
static const int TOWER_TARGET_BRONZE = 2;
static const int TOWER_TARGET_LATE   = 3;
static const int TOWER_LATE_MIN      = 8;   // 第几分钟开始补最后那座（二三波之间）
// 石头保持量上限（用户 2026-09 反馈“后期石头太多”）。
//   石头有三个用途：① 建塔 150/座（最多 TOWER_TARGET_LATE=3 座，而且塔被打坏了
//   还要补建 towerPeak）② **修塔**（REPAIR_COST_RATIO=0.5 × 损失血量比例 × 150，
//   半血修满 ~37 石）③ 谷仓“箭塔升级”科技 50 石。
//   三项加起来最坏也就 450+150+50，所以上限 400 已经足够宽裕；超过它就没必要再挖了。
//   （真正的“该不该继续挖”看 stone_demand_left()，这里只是防充过头。）
static const int STONE_KEEP_MAX      = 400;
// 【用户 2026-09-23：“直接这样吧，15分钟后禁止采石，我看你也控不下来石头量”】
//   15:00 之后**一律禁止采石**，不看账上还欠多少。理由：15:00 之后石头确实没用 ——
//   建塔/补塔、修塔都只在 phase<3（`demand_build` 的箭塔段、`demand_repair`、
//   `stone_demand_left()` 的 ①② 全都 gate 在 phase<3），谷仓“箭塔升级”也在前期。
//   这条硬闸与“按欠账算”那套（stone_demand_left）**同时生效，谁先到就按谁关**。
static const int STONE_STOP_MIN = 15;     // 游戏分钟：到这之后禁止采石
static const int STONE_SWEEP_MS = 2000;   // 已经在矿上的人，多久扫一遍把他们调走（毫秒）
// 黄金保持量上限（用户 2026-09-23：“金子到后期也太多了”）。
//   金子后期的唯一用途是**复合弓兵 20 金/个**（BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_GOLD），
//   一堆科技也花掉一些（车轮 0 金、金矿开采 0 金）。按 24 个弓兵算也就 480 金，
//   再留一点周转，300 已经很宽裕。
//   【为什么不只是“不再派人”】与石头同一个坑：一块金矿点自带 CNT_GOLDORE = 400，
//   而且采集关系一旦建立，村民会**一直挖到矿枯竭**；recycle_tasks 必须
//   **主动把人调走**才是真的停（见那里 RESOURCE_GOLD 那一支）。
static const int GOLD_KEEP_MAX       = 300;

// ---- 箭塔的三波时间线（enemyai.cpp：FAT=6000(4:00) / SAT=13500(9:00) / TAT=21000(14:00)）----
// 【用户 2026-09-21】“第三波之前一定要场上有 3 个箭塔，派一个村民在前两波结束以后去修一修”
//   ① 硬兜底：离第三波还有 TOWER_DEADLINE_GUARD_MIN 分钟时，**不管当前是什么时代**
//      都把箭塔目标顶到 3 座 —— 上面那条阶梯（1/2/3）全部依赖“进铜器”，
//      一旦进铜器晚了（800 食物 + 545 木的链子），到第三波就只有 1~2 座塔。
//   ② 修塔：第二波（9:00）打完、家里没有敌人之后，派 1 个村民去修血最少的那座塔。
//      修塔比补建便宜得多：REPAIR_COST_RATIO=0.5 → 塔半血修满只要 ~37 石，
//      而重建一座要 150 石 + 80 秒工时。
static const int TOWER_DEADLINE_MIN       = 14;   // 第三波到达时刻（= ATTACK_START_FRAME）
static const int TOWER_DEADLINE_GUARD_MIN = 3;    // 提前几分钟开始强行催塔（14-3 = 11:00）
static const int REPAIR_FROM_MIN          = 11;   // 第几分钟起允许修塔（第二波 9:00 + 余量）
static const int REPAIR_ORDER_INTERVAL_MS = 2000;  // 修塔指令节流（重下会 suspendRelation、修理从头开始）

// 靶场上限（用户 2026-09-21：“学院换靶场”）：第 1 座在冲铜器链里建，
//   第 2 座要等第三波打完（phase>=3）才拍 —— 两座靶场把 18 个复合弓兵的生产
//   时间从 9 分钟压到 4.5 分钟（一座靶场 30 秒/个）。
static const int RANGE_MAX = 2;

// 【用户 2026-09-21】第三阶段人口目标（房屋盖到这个上限）。
//   背景：“金子太多了”——黄金堆着花不掉，是因为**人口上限把靶场卡住了**
//   （Human_MaxNum 不够 → 造不了复合弓兵），而不是黄金不够。
//   第三阶段的人口需求：20 个村民 + 18~24 个复合弓兵 + 祭司 + 侦察兵 ≈ 45，
//   再留点余量 → 50（房屋 4 人/座，一共 13 座）。
//   注意第三阶段只剩 16 分钟，房屋 30 木/座、20 秒工时，不会抢掉靶场那 150 木
//   （demand_build 里仍然走 woodBudget 那道预留线）。
static const int PHASE3_POP_TARGET = 50;

// 【用户 2026-09-23：“开局房子造太多导致木头不够，开局房子只需要满足 
//   28 人口即可，然后第三波以后补充房子，这样节省木头”】
//   第三波之前房屋只补到这个上限。
//   【为什么能省很多】一座房 30 木 / 4 人 ⇒ 原来那套 `20 + armyTarget(16) + 4 = 40`
//   比 28 多出 12 人 = **3 座房 = 90 木**；而冲铜器建筑链 + 复合弓科技（100 木）
//   全挤在前 14 分钟，90 木就是实的。
//   28 人怎么够：前期村民 12~14 + 防守用的兵，打完第三波房门再补到
//   PHASE3_POP_TARGET(50)，正好接上“18 个复合弓兵”那个真正的瓶颈。
//   （房屋在 demand_build 里是 priority 1，但它只走 woodBudget、
//     不能动第二座靶场那 150 木的预留 —— 所以“省木头”省的是它自己的钱。）
static const int POP_TARGET_EARLY = 28;

// 侦察骑兵数量：专门用来探路的快速单位（SPEED_SCOUT = 4.07，祭司只有 2.24）。
// **全局只造一个**（用户 2026-09 要求）：它占人口但不计入战斗兵，一旦阵亡不再补造
// （反复补造就是反复花 60 食物 + 一个人口名额）。
static const int SCOUT_UNITS = 1;

// 侦察骑兵最快什么时候造（分钟）+ 必须在进铜器之后。
// 【用户 2026-09 反馈“侦察骑兵造的太早，影响我升级时代了”】它只花 60 食物，
// 但铜器升级要 800 食物，工具时代每 60 食物都是实打实的拖后腿；
// 而且它反正要第三波（14:00）才出门（见 demand_scout），所以 12 分钟造正好：
// 出来就能赶上第三波后的探图，前期又完全不占食物/人口。
static const int SCOUT_BUILD_MIN = 12;

// 采集点分散：同一个资源点最多同时挂 GATHER_PER_RESOURCE_MAX 个村民。
// 树 / 矿石都只占一格，周围站不下太多人（碰撞会把后到的人挤开），
// 全挤在离卸货点最近的那棵树上，结果就是谁也采不踏实。
// 资源点够多时按上限分散；一个符合条件的都没有时（资源太少）
// 会自动放开限制选最近的，不让村民干等。
static const int GATHER_PER_RESOURCE_MAX = 3;

// 采集半径硬上限（格，以市镇中心为圆心）：**超过这个距离的资源一概不采**。
// 【通用规则，不针对任何具体地图】2026-09 用户要求从 100 收到 60：
//   · 村民速度 2.44 格/秒 → 单程 60 格就要走 25 秒，来回 50 秒全耗在路上；
//   · 本 AI 不给采集队派兵护送，走远了遇上敌军/狮子就是白送一个劳动力 + 50 食物
//     （人被杀市中心还会补人，把食物吃掉、拖垮科技，见 TECH_FOOD_RESERVE）。
// 远处的食物/木头缺口由"靠家的农田 + 打猎"补（农田就围着市中心建）。
// 它是以市中心为圆心的欧氏距离，跟第几张图无关；判定用块坐标的平方距离做整数比较，
// 不经 BLOCKSIDELENGTH（避开 Fixed 与 double 混算的重载歧义）。
static const int GATHER_MAX_DIST = 60;

// 石/金的采集半径上限（格）——比食物/木头放宽。
// 理由（仍然是与具体地图无关的通用考虑）：石/金是**战略资源**，
// 复合弓兵 20 金/个、方阵兵 40 金/个，新图上万一石/金只在远处才有，
// 一律不许采就等于自断科技、反攻根本打不起来。
// 实际不会因此多跑路：选点始终挑**最近的可采点**，地图上门口有矿时
// 永远不会去远处的；只有“近处真的一粒矿都没有”才会用上这条例外。
// 觉得不该放宽就把这个值改成跟 GATHER_MAX_DIST 一样（60）即可。
static const int GATHER_MAX_DIST_ORE = 100;

// 采集点的"危险半径"（格）：资源点附近有**可见**敌方单位、或者有活狮子，
// 就不派村民去了。为什幺（用户 2026-09：“后期村民跑太远被杀了”）：
//   狮子攻击距离 10 格（ANIMAL_ATTACKRANGE_LION）、村民 25 血，遇上基本必死；
//   敌方的零散单位/残兵同理。迷雾下只能看到视野内的敌人，但这已经足够挡住
//   “村民自己往敌军/狮群边上凑”。资源点太远/太危险时宁可让村民闲着，
//   也不要白送 —— 死一个就是 50 食物 + 一个劳动力。
static const int GATHER_DANGER_RADIUS = 14;
// 【石/金专用：更小的危险半径】石/金是**没有替代品**的资源 —— 塔有硬死线
//   （第三波 14:00 前必须 3 座），修塔也要石头；食物/木头近处没得采还能去别处。
//   用 14 格的话：第二波（9:00）之后敌人一直在我们家附近转，而我们的 6 块石矿
//   全在市中心 11~16 格 ⇒ 全被判“危险” ⇒ `has_resource(RESOURCE_STONE)` 恒假 ⇒
//   建任务的入口（demand_gather 的 `if (has_resource(...))`）直接不进 ⇒
//   **一个采石工都没有**，第三座塔那 150 石永远攒不到（用户 2026-09-24 反馈）。
//   6 格：敌人真的踩到矿边才不派人 —— 死一个 50 食物的村民，换 150 石的塔。
static const int GATHER_DANGER_RADIUS_ORE = 6;

void UsrAI::processData()
{
    g_ai = this;   // 唯一实例：供下面那些自由函数转发基类调用（HumanMove / calDistance / DebugText ...）

    info = getInfo();

    // 首次进入时构建行为树
    if (!btRoot) build_behavior_tree();

    btCtx.info = &info;

    btRoot->tick(btCtx);
}

// 建筑建造模块

bool find_block(int x,int y,int dx,int dy){
    if (info.theMap == nullptr) return 0;
    int w = (int)info.theMap->size();
    if (w == 0) return 0;
    int ht = (int)(*info.theMap)[0].size();
    if (x < 0 || y < 0 || x + dx > w || y + dy > ht) return 0;
    // 【2026-09 加固】高度缓存改成定长数组 + 正确索引。
    //   原来是变长数组 `int h[dx*dy]` 配索引 `h[dx*i + j]` —— 只有 dx == dy 时
    //   才恰好不越界（3x3 最大下标 3*2+2 = 8 < 9 ✓）；dx=3, dy=2 时最大下标
    //   3*2+1 = 7 ≥ 6，就越界写栈了。本工程只传方阵所以没炸，但公式本身是错的。
    const int n = dx * dy;
    if (n <= 0 || n > 16) return 0;      // 防御：尺寸超出预期（本工程最大 3x3 = 9）
    int h[16] = {0};
    for (int i = 0;i < dx;i ++){
        for (int j = 0;j < dy;j ++){
            tagTerrain field = (*info.theMap)[x + i][y + j];
            if (field.type != MAPPATTERN_GRASS){
                return 0;
            }
            if (MAP[x + i][y + j] != 0){
                return 0;
            }
            h[i * dy + j] = field.height;
        }
    }
    for (int i = 1;i < n;i ++){
        if (h[i] != h[i - 1]){
            return 0;
        }
    }

    // 已有建筑占用（我方 + 敌方）
    for (tagBuilding &b : info.buildings){
        int bs = building_size(b.Type);
        if (b.BlockDR < x + dx && b.BlockDR + bs > x &&
            b.BlockUR < y + dy && b.BlockUR + bs > y) return 0;
    }
    for (tagBuilding &b : info.enemy_buildings){
        int bs = building_size(b.Type);
        if (b.BlockDR < x + dx && b.BlockDR + bs > x &&
            b.BlockUR < y + dy && b.BlockUR + bs > y) return 0;
    }

    // 资源/树木/动物占用（单格）
    for (tagResource &r : info.resources){
        if (r.BlockDR >= x && r.BlockDR < x + dx &&
            r.BlockUR >= y && r.BlockUR < y + dy) return 0;
    }

    // 移动单位占用（村民/军队，单格）
    for (tagFarmer &f : info.farmers){
        if (f.BlockDR >= x && f.BlockDR < x + dx &&
            f.BlockUR >= y && f.BlockUR < y + dy) return 0;
    }
    for (tagArmy &a : info.armies){
        if (a.BlockDR >= x && a.BlockDR < x + dx &&
            a.BlockUR >= y && a.BlockUR < y + dy) return 0;
    }

    return 1;
}

// ==================== 战斗模块 ====================

// ==================== 任务系统 ====================
// 字段复用约定：PRODUCE/UPGRADE 任务中
//   buildingType = 执行动作的建筑类型；targetSN = 要执行的 Action 编号。

// ---------- 同步：阶段推进 + 回收任务 ----------
void bt_sync()
{
    if (info.civilizationStage < CIVILIZATION_BRONZEAGE) {
        phase = 1;   // 开局即工具时代，直接冲铜器
    } else if (info.GameFrame >= ATTACK_START_FRAME) {
        phase = 3;   // 第三波之后：组织反攻
    } else {
        phase = 2;   // 铜器时代：发展军事、防守三波
    }

    // 敌方位置记录：每帧调一次，但**只有第三阶段（侦察骑兵出门探图后）才真正生效**
    //（见 record_enemy_positions 开头）。放在 phase 算完之后调，避免阶段切换那一帧
    // 读到上一帧的 phase；后面的 demand_attack / demand_scout 都能用上同一帧的最新位置。
    record_enemy_positions();

    // 箭塔数量目标：前期 1 座（只有开局自带的那座），进铜器后再谈建造。
    // **"补到 3 座"这条也要求 phase>=2**：用户要求"第二座塔在铜器以后建"，
    // 所以哪怕进铜器很晚（已经过了 8:00），也不在工具时代为了塔去占人力，
    // 而是"进铜器 → 用开局 150 石建第二座 → 采石 → 建第三座"。
    // （唯一的例外是下面那条 11:00 的硬兜底：那种情况下人命比木材重要。）
    const int towerLateFrame = (int)(TOWER_LATE_MIN * 60 * 1000.0 / TimePerFrame);
    if (phase >= 2) arrowTowerTarget = TOWER_TARGET_BRONZE;
    else            arrowTowerTarget = TOWER_TARGET_EARLY;
    if (phase >= 2 && info.GameFrame >= towerLateFrame)
        arrowTowerTarget = TOWER_TARGET_LATE;
    // 【用户 2026-09-21：“第三次进攻结束以后不需要建箭塔了”】
    //   第三阶段资源全让给复合弓兵 + 第二座靶场，塔被打坏也不再补。
    //   demand_build 那边建塔那一段也按 phase<3 关了（否则 towerPeak 会把它顶回来）；
    //   采石的需求也按 phase<3 关了（见 stone_demand_left —— 第三阶段只剩
    //   “箭塔升级”科技那 50 石，塔本身不再建也不再修）。
    if (phase >= 3) arrowTowerTarget = 0;

    // 【用户 2026-09-21】“第三波之前一定要场上有 3 个箭塔”：
    //   上面那条阶梯（1 → 2 → 3）全部挂在“进铜器”上，进铜器一慢就凑不齐。
    //   这里补一条**按时间**的硬兜底 —— 离第三波（14:00）还有 GUARD 分钟时，
    //   不管当前是什么时代都先把目标顶到 3 座：demand_build 会立刻排建造任务，
    //   demand_gather 的 needStone（用的是 TOWER_TARGET_LATE，不受本变量影响）
    //   也会马上派村民去备那 150 石。
    //   工具时代也认：真到 11:00 还在工具时代，塔的防守价值已经高于那点木材人力。
    if (phase < 3
        && info.GameFrame >= (int)((TOWER_DEADLINE_MIN - TOWER_DEADLINE_GUARD_MIN)
                                   * 60 * 1000.0 / TimePerFrame))
        arrowTowerTarget = TOWER_TARGET_LATE;

    // ---- 目标农田数 = 想派去种田的村民数（一个村民对应一格农田）----
    // 内核一块农田只认一个采集者，而且采完会自动消失，所以要按人口持续补建。
    // 前置：市场（Development 里农田的 buildCon 挂了市场的 precondition）。
    farmTarget = 0;
    if (count_done(BUILDING_MARKET) > 0) {
        int farmerNum = 0;
        for (tagFarmer &f : info.farmers)
            if (f.FarmerSort == FARMERTYPE_FARMER) farmerNum++;
        farmTarget = farmerNum / FARM_PER_POP;
        // 【用户 2026-09-24：“前期农场建的太多”】先按“村民数/3”算，再按阶段封顶：
        //   工具时代 ≤ FARM_CAP_TOOL(2)、铜器 ≤ FARM_CAP_BRONZE(4)、
        //   第三波后直接 FARM_TARGET_LATE(7)（见那两个常量的说明）。
        //   注意 farmTarget 同时是“派几个村民去种田”的名额，所以封顶不只省木头，
        //   还会把人手还给伐木/建造。
        if (phase >= 3) {
            farmTarget = FARM_TARGET_LATE;        // 7 = 八宫格留一格，别把去金矿的路堵死
        } else {
            const int cap = (phase >= 2) ? FARM_CAP_BRONZE : FARM_CAP_TOOL;
            if (farmTarget > cap) farmTarget = cap;
        }
        if (farmTarget > FARM_MAX) farmTarget = FARM_MAX;   // 兜底：八宫格一共只有 8 个格位
    }

    // 农田绑定表：清掉"田没了（采完被内核删）"或"人没了"的条目
    prune_farm_holders();

    // 【给新建的任务补创建帧号】—— 必须排在 recycle_tasks() 之前
    //   `Task.startFrame` 默认 0，而 recycle_tasks 有两处依赖它：
    //     ① TASK_GATHER 分支靠它区分"还没轮到 assign_tasks 派发"与"真的派不出去"
    //        （行为树把 gather 排在 dispatch 之后，采集任务要等下一帧才拿到 targetSN）；
    //     ② 函数末尾那条"WAITING 超过 60*120 帧就判 FAILED"也用它 ——
    //        startFrame == 0 会让任务在帧 7200（约 4.8 分钟）之后"新建即删"。
    //   集中在这里补，采集 / 建造 / 生产任务全部覆盖。
    for (Task &t : taskQueue)
        if (t.startFrame == 0) t.startFrame = info.GameFrame;

    recycle_tasks();
}

// 清理 farmHolder：田不存在/已采完，或农民已阵亡，就解除绑定。
// 必须做——否则被删掉的田会永远占着一条绑定，后续永远匹配不上。
void prune_farm_holders()
{
    if (farmHolder.empty()) return;

    // 【2026-09 加固：先把“还活着的”收成两张表，再单向扫一遍 farmHolder】
    //   原来对每条绑定都重新遍历一遍 info.buildings 和 info.farmers，
    //   复杂度 O(绑定数 × (建筑数 + 农民数))。规模小时无所谓，但**遍历 info 的
    //   次数越少越安全**：引擎侧 AI 线程与内核并非完全互斥，
    //   少读一次就少一分踩到坏内存的机会。
    //   （用 unordered_map 当集合：本头文件只能 include <unordered_map>，
    //     加不了 <set>/<unordered_set>。）
    std::unordered_map<int,int> liveFarms;    // 还活着、且还有剩余资源的农田 SN
    for (tagBuilding &b : info.buildings)
        if (b.Type == BUILDING_FARM && b.Cnt > 0) liveFarms[b.SN] = 1;

    std::unordered_map<int,int> liveFarmers;  // 还在的村民 SN
    for (tagFarmer &f : info.farmers) liveFarmers[f.SN] = 1;

    for (auto it = farmHolder.begin(); it != farmHolder.end(); ) {
        if (liveFarms.count(it->first) && liveFarmers.count(it->second)) ++it;
        else it = farmHolder.erase(it);
    }
}

// ---------- 统计辅助 ----------
// 建造位置拉黑表（见 UsrAI.h 里的说明：防止"每几秒重下一单、每次都被内核驳回"）
bool build_site_ok(int x, int y)
{
    int key = (x << 12) | y;
    std::unordered_map<int,int>::iterator it = badBuildSite.find(key);
    if (it == badBuildSite.end()) return true;
    return it->second <= info.GameFrame;      // 过期即视为可用
}

void mark_build_site_bad(int x, int y)
{
    // 顺手清过期项（表一直很小）
    for (std::unordered_map<int,int>::iterator it = badBuildSite.begin();
         it != badBuildSite.end(); ) {
        if (it->second <= info.GameFrame) it = badBuildSite.erase(it);
        else ++it;
    }
    int key = (x << 12) | y;
    badBuildSite[key] = info.GameFrame + BAD_BUILD_SITE_MS / TimePerFrame;
}

int count_done(int type)
{
    int c = 0;
    for (tagBuilding &b : info.buildings)
        if (b.Type == type && b.Percent >= 100) c++;
    return c;
}

int active_build(int btype)
{
    int c = 0;
    for (Task &t : taskQueue)
        if (t.type == TASK_BUILD && t.buildingType == btype
            && t.state != TASK_DONE && t.state != TASK_FAILED) c++;
    return c;
}

int active_gather(int rtype)
{
    int c = 0;
    for (Task &t : taskQueue)
        if (t.type == TASK_GATHER && t.resourceType == rtype
            && t.state != TASK_DONE && t.state != TASK_FAILED) c++;
    return c;
}

int active_action(int btype, int action)
{
    int c = 0;
    for (Task &t : taskQueue)
        if ((t.type == TASK_PRODUCE || t.type == TASK_UPGRADE)
            && t.buildingType == btype && t.targetSN == action
            && t.state != TASK_DONE && t.state != TASK_FAILED) c++;
    return c;
}

// 该资源点上已经派了几个采集村民（还没做完的任务）
int gatherers_on(int resSN)
{
    int c = 0;
    for (Task &t : taskQueue)
        if (t.type == TASK_GATHER && t.targetSN == resSN
            && t.state != TASK_DONE && t.state != TASK_FAILED) c++;
    return c;
}

// 资源点 resSN "最多能同时站几个人"：它周围 8 格里可站立格的数量，
// 上限 GATHER_PER_RESOURCE_MAX(3)。**0 = 根本够不到**。
//
// 为什么要看这个（用户反馈"某些情况下砍树的还是砍不到"的根因）：
//   1) 内核要求采集者贴到目标 ~0.5 格以内才能开工
//      （Core_CondiFunc.h: distance_AllowWork = 目标半宽 + 2*CRASHBOX_SINGLEOB，
//       树是 CRASHBOX_SINGLEBLOCK 级别，算下来只有十几个细节单位 = 不到一格），
//      而树/矿/建筑在图上都是障碍格。周围一个可站立格都没有的点
//      （密林深处、水里/水边、被建筑围住），村民根本走不到跟前，
//      内核很快判"行动无用"（forcedInterruptCondition 的 UselessAction）
//      强制中断关系，村民变回 IDLE —— 表现就是"砍不到"，然后又被重派过去，
//      来回白跑。
//   2) 就算站得到，能站的也就那么几个位置。人比位置多的时候，
//      多出来的会被碰撞挤开、够不到采集距离，同样是"砍不到"。
//
// 实现：**按需惰性计算 + 每帧缓存**（key = GameFrame，SN → 站位数的表）。
// 只有这一帧真的被问到过的资源点才算一次，算过就存下来；下一帧清空重来。
// 不要写成"每帧把所有资源点都算一遍"：全图资源上百个、每个要查 9 个格子的
// block_is_standable（内部还要遍历建筑表和资源表），那是百万级的开销。
int res_stand_spots(int resSN)
{
    if (resSpotsFrame != info.GameFrame) {   // 新的一帧：缓存作废
        resSpots.clear();
        resSpotsFrame = info.GameFrame;
    }
    std::unordered_map<int,int>::iterator it = resSpots.find(resSN);
    if (it != resSpots.end()) return it->second;

    int n = 0;
    for (tagResource &r : info.resources) {
        if (r.SN != resSN) continue;
        for (int dx = -1; dx <= 1 && n < GATHER_PER_RESOURCE_MAX; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                if (dx == 0 && dy == 0) continue;
                if (!block_is_standable(r.BlockDR + dx, r.BlockUR + dy)) continue;
                if (++n >= GATHER_PER_RESOURCE_MAX) break;
            }
        }
        break;
    }
    resSpots[resSN] = n;
    return n;
}

// 全图"伐木工能同时站几个人"：把所有可砍的树周围的可站立格**去重**后数一遍。
//
// 【为什么需要它：用户 2026-09 反馈"砍树的人太多导致卡死"】
//   上面 res_stand_spots() 是**按单棵树**数周围 8 格的，而密林里相邻几棵树的
//   站位格是**互相重叠**的：五棵挨在一起的树，每棵都报"能站 3 个"，加起来 15，
//   而整片林子实际只有 3~4 个落脚点。按这个虚高的容量派人，多出来的人全挤在
//   林子边缘、互相碰撞、够不到采集距离，内核判"行动无用"强制中断关系 →
//   村民变 IDLE → 下一帧又被重派过去 → 死循环。表现就是"一大群伐木工堵在
//   树林里谁也不动"。
//
// 所以伐木总人数必须按"**去重后的全图站位格数**"来卡（见 wood_gather_limit()），
// 而不是按"单点上限 × 树的棵数"。
//
// 实现上沿用 res_stand_spots 的"按需惰性 + 每帧缓存"：一帧只算一次，key = GameFrame。
// 用 unordered_map 当集合（不能引入 <set>，见 UsrAI.h 头部的说明）。
int wood_capacity()
{
    if (woodCapFrame == info.GameFrame) return woodCapCache;

    std::unordered_map<int,int> cells;   // 站位格集合（value 恒为 1，只当 set 用）
    for (tagResource &r : info.resources) {
        if (r.Type != RESOURCE_TREE) continue;
        if (r.Cnt <= 0) continue;              // 已经砍完的树不算
        if (res_too_far(r.Type, r.BlockDR, r.BlockUR)) continue;   // 太远：本来就不派人去
        if (gather_spot_dangerous(r.DR, r.UR)) continue;           // 危险：本来就不派人去
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                if (dx == 0 && dy == 0) continue;
                if (!block_is_standable(r.BlockDR + dx, r.BlockUR + dy)) continue;
                // 打包成 (x << 12) ^ y：同一格被多棵树看到只算一次
                cells[((r.BlockDR + dx) << 12) ^ (r.BlockUR + dy)] = 1;
            }
        }
    }

    woodCapCache = (int)cells.size();
    woodCapFrame = info.GameFrame;
    return woodCapCache;
}

// 伐木人数上限（推导见 UsrAI.h 的声明处注释）。
//
// 【注意这里的下限处理】以前想着"至少留 WOOD_MIN_GATHERERS 个人砍树，保证木头不断供"，
// 但如果树林本身就只剩两个落脚点（cap = 2），硬留 4 个人只会让 2 个人白跑、
// 把林子挤死 —— 那正是本次要修的病。所以**物理容量优先**：
//   cap > 0 时上限就是 min(WOOD_MAX_GATHERERS, cap)，不往上抬；
//   cap = 0（视野内没树 / 树都砍完了）时返回保底值，反正 has_resource() 会拦住不派。
int wood_gather_limit()
{
    int cap = wood_capacity();
    if (cap <= 0) return WOOD_MIN_GATHERERS;

    // 【用户 2026-09-24：“第一波多出的村民目前看来都去打猎了，请稍微分一部分给木头”】
    //   第三波之前木头是硬瓶颈（冲铜器链 545 木 + 靶场 150 + 农田 75/块 + 房屋 30/座），
    //   所以前期把上限从 6 提到 WOOD_MAX_GATHERERS_EARLY(8)；
    //   第三阶段反过来（食物才是瓶颈、木头只剩房屋/补建设）仍用 6。
    int lim = (phase >= 3) ? WOOD_MAX_GATHERERS : WOOD_MAX_GATHERERS_EARLY;
    if (cap < lim) lim = cap;          // 物理上限：全图树林站不下这么多人
    return lim;
}

// 每帧缓存：树 SN → 正在伐这棵树的村民数（**内核真值**，不是我们的任务表）。
// 【为什么要它】Core.cpp:459 把“当前关系的目标对象 SN”直接写进 WorkObjectSN
//   （Core_List::getObjectSN：没有关系时返回 -1），所以“正走过去砍”也算在内。
//   而 gatherers_on() / active_gather() 数的是**任务**：下面 demand_gather 末尾的
//   “兜底 2”是直接用 HumanAction 给村民下指令、**不进任务队列**的。
//   只按任务数判“这个点站满没有”，会永远以为还有空位，一帧一帧往里塞人
//   （既可能把林子挤死，也可能让人空转）。
// 只算 FARMERTYPE_FARMER：渔船的渔夫不算。
static std::unordered_map<int,int> cutterAtTree;   // 树 SN → 村民数（每帧重建）
static int cutterAtTreeFrame = -1;

static void ensure_cutter_at_tree()
{
    if (cutterAtTreeFrame == info.GameFrame) return;
    cutterAtTreeFrame = info.GameFrame;
    cutterAtTree.clear();

    std::unordered_map<int,int> isTree;   // 活着的树的 SN 集合
    for (tagResource &r : info.resources)
        if (r.Type == RESOURCE_TREE && r.Cnt > 0) isTree[r.SN] = 1;
    if (isTree.empty()) return;

    for (tagFarmer &f : info.farmers) {
        if (f.FarmerSort != FARMERTYPE_FARMER) continue;
        if (f.WorkObjectSN == -1) continue;
        if (!isTree.count(f.WorkObjectSN)) continue;
        cutterAtTree[f.WorkObjectSN]++;
    }
}

// 队列里所有"还没建成/还没失败"的建造任务，总共要花多少木头。
// 为什么需要它：内核是在**执行建造那一刻**才检查并扣资源的
// （Core_List：ACTION_INVALID_RESOURCE "当前资源不足"），
// 而我们是在**排任务那一刻**用 info.Wood 判断的。
// 同一帧排出去的多个建筑（建筑链和农田现在都是 priority 2）加起来就可能超支，
// 后执行的那个就会直接报失败。所以排队时要按"已排出的花费"预留。
int pending_build_wood()
{
    int sum = 0;
    for (Task &t : taskQueue) {
        if (t.type != TASK_BUILD) continue;
        if (t.state == TASK_DONE || t.state == TASK_FAILED) continue;
        sum += build_wood_cost(t.buildingType);
    }
    return sum;
}

// 采集点的“危险半径”：石/金没有替代品（塔有时间死线），只用很小的半径；
//   食物/木头近处没得采还能去别处，命更重要。
static int gather_danger_radius(int rtype)
{
    return (rtype == RESOURCE_STONE || rtype == RESOURCE_GOLD)
           ? GATHER_DANGER_RADIUS_ORE : GATHER_DANGER_RADIUS;
}

bool has_resource(int rtype)
{
    // 普通资源看 Cnt；活动物 Cnt=0 但 Blood>0，也算"有资源"（可打猎）
    bool isAnimal = (rtype == RESOURCE_GAZELLE || rtype == RESOURCE_ELEPHANT
                     || rtype == RESOURCE_LION);
    for (tagResource &r : info.resources) {
        if (r.Type != rtype) continue;
        if (res_too_far(rtype, r.BlockDR, r.BlockUR)) continue;   // 离市中心太远：当它不存在
        // 附近有敌人/狮子：别派村民去送死。**石/金用更小的半径**（见 GATHER_DANGER_RADIUS_ORE）
        if (gather_spot_dangerous(r.DR, r.UR, gather_danger_radius(r.Type))) continue;
        if (r.Cnt > 0) return true;
        if (isAnimal && r.Blood > 0) return true;
    }
    return false;
}

// 以我方市中心为圆心、半径 limit 格：这个块是不是在圈外。
// 找不到已建成的市中心（异常）时返回 false：宁可照常采集，也不要让 AI 停摆。
// 用**块坐标的平方距离**做整数比较：不用开方，也避开了 BLOCKSIDELENGTH
// （Fixed 定点类型）和 double 混算的重载歧义。
// 该块到**最近的地图边**的距离（块）—— 越小说明越贴着地图边缘。
// 房屋摆放用它：房子要沿着离我方基地最近的那条边排，而不是往地图中心扩。
static int edge_distance(int blockDR, int blockUR)
{
    // 【越界一律当成“最贴边”】环形搜索时 ax+i 可能超出地图，交给随后的 find_block
    //   拒掉；这里绝不能返回负数（否则比较会乱）。
    if (blockDR < 0 || blockUR < 0 || blockDR >= MAP_L || blockUR >= MAP_U) return 0;
    // 【注意用 MAP_L-1】合法块坐标是 0..MAP_L-1（Map::isOverBorder 判 >=MAP_L），
    //   所以“紧贴另一侧边”（blockDR == MAP_L-1）的真实距离是 0、不是 1。
    //   以前用 MAP_L - blockDR 会让**远侧那条边永远到不了 0**，房屋就会提前撞上
    //   “找不到比锚点更贴边的点”而掉进放宽的那一遍。
    int d = blockDR;                                          // 到 DR=0 那条边
    if (MAP_L - 1 - blockDR < d) d = MAP_L - 1 - blockDR;
    if (blockUR < d) d = blockUR;                              // 到 UR=0 那条边
    if (MAP_U - 1 - blockUR < d) d = MAP_U - 1 - blockUR;
    return d;
}

// 【已删除】unit_on_site(bx,by,size)：它检查“建造位上是否正站着单位”，
//   意图是防止把村民关在建筑里（“房屋把农民卡住了”）。但 **find_block() 里
//   已经有逐字等价的两段检查**（info.farmers / info.armies 的 BlockDR/BlockUR
//   落在 footprint 内就返回 0），而调用点排在 find_block 之后 ——
//   所以它**永远不会触发**，是纯粹的冗余。防“单位被关死”这条由 find_block 负责。

static bool block_beyond_home(int blockDR, int blockUR, int limitBlocks)
{
    for (tagBuilding &b : info.buildings) {
        if (b.Type != BUILDING_CENTER || b.Percent < 100) continue;
        int dx = blockDR - b.BlockDR;
        int dy = blockUR - b.BlockUR;
        return dx * dx + dy * dy > limitBlocks * limitBlocks;
    }
    return false;
}

// 该资源点是否离市镇中心太远 → 一律不派人去采。
// 普通资源（食物/木头）用 GATHER_MAX_DIST(60)；石/金用 GATHER_MAX_DIST_ORE（策略例外）。
// 【树的第二个例外：近处没树了就放宽（用户 2026-09 反馈“木头预留没生效”）】
//   城边的树总有砍完的一天。近处的树砍完之后如果还死守 60 格，木头收入直接归零 ——
//   此时“给科技预留 100 木”根本没意义：预留只是**不花**，收入是 0 就永远攒不到 100。
//   所以近处确实没树时放宽到 GATHER_MAX_DIST_ORE(100)，与石/金那条例外同理：
//   选点主判据是“到最近仓库的距离”（见 assign_tasks），近处有树时绝不会去远处。
bool res_too_far(int type, int blockDR, int blockUR)
{
    int limit = (type == RESOURCE_STONE || type == RESOURCE_GOLD)
                ? GATHER_MAX_DIST_ORE : GATHER_MAX_DIST;
    if (type == RESOURCE_TREE && limit < GATHER_MAX_DIST_ORE) {
        if (treeNearFrame != info.GameFrame) {   // 每帧算一次就够（它在选点循环里被反复调用）
            treeNearFrame = info.GameFrame;
            treeNearHome = false;
            for (tagResource &r : info.resources) {
                if (r.Type != RESOURCE_TREE || r.Cnt <= 0) continue;
                if (!block_beyond_home(r.BlockDR, r.BlockUR, GATHER_MAX_DIST)) {
                    treeNearHome = true;
                    break;
                }
            }
        }
        if (!treeNearHome) limit = GATHER_MAX_DIST_ORE;
    }
    return block_beyond_home(blockDR, blockUR, limit);
}

// 该点附近是否“很危险”：有可见的敌方军队，或者有活狮子。
// 用途：选采集点时过滤（`assign_tasks` / 兜底 2 / `has_resource`），
// 以及让已经站在危险区里的闲置村民撤回家（见 demand_gather 的兜底 2）。
// 动物活着的判据用的是 Blood>0（跟采集选点那两处一致：死物看 Cnt、活动物看 Blood）。
bool gather_spot_dangerous(double dr, double ur, int radiusBlocks)
{
    const double r = ((radiusBlocks > 0) ? radiusBlocks : GATHER_DANGER_RADIUS)
                     * BLOCKSIDELENGTH;
    for (tagArmy &e : info.enemy_armies)
        if (calDistance(dr, ur, e.DR, e.UR) < r) return true;
    for (tagResource &res : info.resources) {
        if (res.Type != RESOURCE_LION) continue;
        if (res.Blood <= 0) continue;                 // 已经死掉的狮子不危险
        if (calDistance(dr, ur, res.DR, res.UR) < r) return true;
    }
    return false;
}

bool center_free()
{
    for (tagBuilding &b : info.buildings)
        if (b.Type == BUILDING_CENTER && b.Percent >= 100 && b.Project == 0)
            return true;
    return false;
}

// ---------- 建造需求 ----------
void demand_build()
{
    // 【给复合弓科技留木头（详见 TECH_WOOD_RESERVE 的说明）】
    //   冲刺期（compositeBowUrgent()）内先把科技要的 100 木扣下来，
    //   剩下的才算"可建木头"。这样木头会在预留线之上波动，科技一有
    //   100 木就能点上；农田/房屋则是间歇性补建（不会饿死）。
    //   箭塔只花石头，不受影响。
    int woodBudget = info.Wood;
    if (compositeBowUrgent()) {
        woodBudget -= TECH_WOOD_RESERVE;
        if (woodBudget < 0) woodBudget = 0;
    }

    // 【用户 2026-09-22：“这一次少靶场。能否 18 分钟前建成 2 个靶场”】
    //   第二座靶场的条件原来只是“phase>=3 且木头够”，但它和房屋（priority 1）、
    //   农田（priority 2）抢同一池木头，而**房屋每帧都先被排进队列**
    //   （房屋那一段在本函数最前面），`pending_build_wood()` 一加上房屋那 30 木，
    //   靶场那 150 木就常常一直凑不齐 —— 于是到 18 分钟场上还是只有一座靶场。
    //   这里给它**预留**一份木料：第二座还没建成之前，房屋/农田/马厩这些
    //   “可选建造”只能花扣掉预留之后剩下的钱（optionalWood）；
    //   靶场自己的判定仍然用完整的 woodBudget，所以木头一到位就轮到它。
    //   （预留只在 phase>=3 生效 —— 冲铜器/科技冲刺期不能动它们的木头。）
    const int rangeReserve =
        ((phase >= 3
          && count_done(BUILDING_RANGE) + active_build(BUILDING_RANGE) < RANGE_MAX)
         ? BUILD_RANGE_WOOD : 0);
    const int optionalWood = (woodBudget > rangeReserve) ? (woodBudget - rangeReserve) : 0;
    // ---- 房屋：按"目标人口"提前补，别让人口上限卡住村民生产与造兵 ----
    // 【用户 2026-09-23：“开局房子造太多导致木头不够，开局房子只需要满足 28 人口
    //   即可，然后第三波以后补充房子，这样节省木头”】
    //   所以分两档：
    //     · 第三波之前 → POP_TARGET_EARLY(28)。房屋 30 木/座，而前 14 分钟木头要同时
    //       供冲铜器建筑链（545 木）和复合弓科技（100 木）—— 原来按
    //       `20 + armyTarget(16) + 4 = 40` 盖，比 28 多 3 座房 = 白白 90 木；
    //       而前两波防守根本用不到 40 人（村民优先占人口，兵自然被卡在 28）。
    //     · 第三波之后（phase>=3）→ PHASE3_POP_TARGET(50)：那才是真瓶颈
    //       （18~24 个复合弓兵 + 20 个村民全部要装下）。
    {
        const int targetPop = (phase >= 3) ? PHASE3_POP_TARGET : POP_TARGET_EARLY;
        int homeNeed = (targetPop - info.Human_MaxNum + HOUSE_HUMAN_NUM - 1)
                       / HOUSE_HUMAN_NUM;      // 还差几座房
        // 注意用 optionalWood（扣掉第二座靶场的预留）：房屋是 priority 1、又排在本
        //   函数最前面，让它先花的话第二座靶场那 150 木永远凑不齐（见 rangeReserve）。
        if (homeNeed > 0
            && optionalWood >= pending_build_wood() + BUILD_HOUSE_WOOD
            && active_build(BUILDING_HOME) < 2) {
            Task t;
            t.id = nextTaskId++;
            t.type = TASK_BUILD;
            t.priority = 1;
            t.buildingType = BUILDING_HOME;
            taskQueue.push_back(t);
        }
    }

    // ---- 冲铜器建筑链：谷仓 → 市场 → 兵营 → 靶场 → 马厩 ----
    // 靶场/马厩既能让"工具时代建筑数 ≥ 2"满足升级条件，又提供远程与机动兵种
    if (phase < 2) {
        struct BuildNeed { int type; int wood; };
        const BuildNeed chain[] = {
            { BUILDING_GRANARY,  BUILD_GRANARY_WOOD  },
            { BUILDING_MARKET,   BUILD_MARKET_WOOD   },
            { BUILDING_ARMYCAMP, BUILD_ARMYCAMP_WOOD },
            { BUILDING_RANGE,    BUILD_RANGE_WOOD    },
            { BUILDING_STABLE,   BUILD_STABLE_WOOD   },
        };
        for (const BuildNeed &n : chain) {
            // 要按"已排队但还没建成的花费"预留，否则同一帧排出的几个建筑
            // （建筑链是 priority 1、农田是 2）加起来会超支，
            // 后执行的那个会被内核以 ACTION_INVALID_RESOURCE"当前资源不足"拒绝。
            if (count_done(n.type) == 0 && active_build(n.type) == 0
                && woodBudget >= pending_build_wood() + n.wood) {
                Task t;
                t.id = nextTaskId++;
                t.type = TASK_BUILD;
                // 【用户 2026-09-23：“把第一个靶场优先级提到农场前”】
                //   整条冲铜器链提到 priority 1（与房屋/箭塔同级，**高于农田的 2**）。
                //   为什么必须提：农田是**持续补建**的（内核采完就把田删掉），
                //   上一帧排出的农田任务若一时没闲人，会一直停在 WAITING，
                //   而它的 id 比**更晚**才排进队列的靶场小 —— 同级按 id 排序时
                //   农田永远赢，靶场（唯一能出弓兵/复合弓兵的来源）就一直等不到工人。
                //   【链内顺序靠 id 保住了】本循环一帧只排一个建筑、从前往后走，
                //   所以后一位的 id 一定更大；而**靶场的前置条件是兵营**
                //   （Development.cpp:727
                //     BUILDING_RANGE.buildCon->addPreCondition(BUILDING_ARMYCAMP.buildCon)）
                //   —— 兵营 id 更小 ⇒ 一定排在靶场前面，不会出现“靶场先被派去建、
                //   却因前置未完成被内核驳回”的反复重排。
                //   （第二座靶场在下面 phase>=3 的分支里早就是 priority 1，口径一致。）
                t.priority = 1;
                t.buildingType = n.type;
                taskQueue.push_back(t);
                break;
            }
        }
    } else {
        // 铜器时代：补齐马厩（骑兵前置，也是升铜器三选一之一）
        //   用 optionalWood：第二座靶场比马厩重要（第三阶段不造骑兵了）。
        if (count_done(BUILDING_STABLE) == 0 && active_build(BUILDING_STABLE) == 0
            && optionalWood >= pending_build_wood() + BUILD_STABLE_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 2;
            t.buildingType = BUILDING_STABLE;
            taskQueue.push_back(t);
        }
        // 【用户 2026-09-21：“学院换靶场。在第三次进攻结束以后再拍一个靶场”】
        //   · 不再建学院：它只能出方阵兵（60 食 + 40 金），而那些黄金正好是
        //     复合弓兵要的（20 金/个）；demand_army 里那段方阵兵生产也一并删了。
        //   · 换成**第二座靶场**，而且等第三波打完（phase>=3）才拍：
        //     冲铜器和复合弓科技冲刺期都缺木头，不能提前跟它们抢。
        //     2 座靶场 → 18 个复合弓兵从 9 分钟压到 4.5 分钟。
        // 【用户 2026-09-22：“能否 18 分钟前建成 2 个靶场”】
        //   条件没变（仍然是 phase>=3，即“进铜器 + 过 14:00”），变的是：
        //   ① rangeReserve 已经把 150 木扣下来（不再被房屋/农田抢走）；
        //   ② 优先级从 2 提到 **1**（与房屋同级），免得排到农田后面排队。
        else if (phase >= 3
            && count_done(BUILDING_RANGE) + active_build(BUILDING_RANGE) < RANGE_MAX
            && woodBudget >= pending_build_wood() + BUILD_RANGE_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 1;
            t.buildingType = BUILDING_RANGE;
            taskQueue.push_back(t);
        }
    }

    // ---- 农田：环绕市中心建（采完走一格就能上交），数量 = 目标农田数 ----
    // "一个村民对应一格农田"：内核一块农田只允许一个采集者，而且采完会被
    // 自动删除，所以这里要持续补建，直到达到 farmTarget（见 bt_sync）。
    // 优先级用 2：低于冲铜器建筑链（1）—— 农田是**持续补建**的，
    //   如果和建筑链同级，先排出来的田会靠 id 一直压在靶场前面
    //   （用户 2026-09-23：“把第一个靶场优先级提到农场前”）。
    //   仍低于采集的 3/4（派不派采集靠采集任务，不靠建筑任务）。
    // 用 optionalWood：第二座靶场那 150 木的预留要优先保证（见 rangeReserve）。
    if (farmTarget > 0
        && count_done(BUILDING_FARM) + active_build(BUILDING_FARM) < farmTarget
        && optionalWood >= pending_build_wood() + BUILD_FARM_WOOD + 50) {
        Task t;
        t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 2;
        t.buildingType = BUILDING_FARM;
        taskQueue.push_back(t);
    }

    // ---- 升级铜器时代 ----
    if (phase < 2) {
        int tool = count_done(BUILDING_MARKET)
                 + count_done(BUILDING_STABLE)
                 + count_done(BUILDING_RANGE);
        if (tool >= 2 && info.Meat >= BUILDING_CENTER_UPGRADE_BRONZEAGE_FOOD
            && center_free()
            && active_action(BUILDING_CENTER, BUILDING_CENTER_UPGRADE) == 0) {
            Task t;
            t.id = nextTaskId++;
            t.type = TASK_UPGRADE;
            t.priority = 0;
            t.buildingType = BUILDING_CENTER;
            t.targetSN = BUILDING_CENTER_UPGRADE;
            taskQueue.push_back(t);
        }
    }

    // ---- 箭塔：谷仓研发科技 → 建箭塔（防御核心）----
    {
        // 研发箭塔科技（通过 ins_ret 的 LOCK 判定是否已研发）
        if (!arrowTowerResearched && count_done(BUILDING_GRANARY) > 0) {
            if (arrowTowerResearchId >= 0 && info.ins_ret.count(arrowTowerResearchId)) {
                if (info.ins_ret[arrowTowerResearchId] == ACTION_INVALID_BUILDACT_LOCK)
                    arrowTowerResearched = true;   // 已研发过
                arrowTowerResearchId = -1;
            }
            // 【2026-09-23 超时重试】与 request_research 同一个坑：内核
            //   `deduplicateInstructions` 按主体 SN 去重，而**同一座谷仓**上还有
            //   demand_research 的“箭塔升级”订单（BUILDING_GRANARY_ARROWTOWE_UPGRADE，
            //   与这里的 BUILDING_GRANARY_ARROWTOWER 是**两个不同的动作**）——
            //   同一帧两条都指向这座谷仓时，后者（demand_research 更晚跑）胜出，
            //   这条指令就被丢掉，`ins_ret` 里永远不会出现这个 id。
            //   没有超时的话 `arrowTowerResearchId` 永远不为 -1 ⇒ **永不再下单** ⇒
            //   `arrowTowerResearched` 永远为假 ⇒ **一座箭塔都建不出来**。
            if (arrowTowerResearchId >= 0
                && info.GameFrame - arrowTowerResearchFrame > 2000 / TimePerFrame)
                arrowTowerResearchId = -1;
            if (!arrowTowerResearched && arrowTowerResearchId == -1
                && info.Meat >= BUILDING_GRANARY_ARROWTOWER_FOOD) {
                for (tagBuilding &b : info.buildings) {
                    if (b.Type == BUILDING_GRANARY && b.Percent >= 100 && b.Project == 0) {
                        arrowTowerResearchId = BuildingAction(b.SN, BUILDING_GRANARY_ARROWTOWER);
                        arrowTowerResearchFrame = info.GameFrame;
                        break;
                    }
                }
            }
        }

        // 建箭塔
        int towerNum = 0;
        for (tagBuilding &b : info.buildings)
            if (b.Type == BUILDING_ARROWTOWER) towerNum++;
        // 【用户 2026-09 要求】箭塔被打坏了要补一个回来：
        //   记住“曾经拥有过的最多塔数”（towerPeak），当前数量一旦掉到它以下，
        //   就把建造目标顶回去 —— 即“打坏几个补几个”。
        // 【为什么光靠箭头 arrowTowerTarget 不行】那条阶梯是 1/2/3，数量少一个
        //   固然会让条件成立，但“目标只涨不补”的语义在掉塔时会和 active_build、
        //   石头库存纠缠（比如建完第二座后石为 0，而目标刚好等于当前塔数），
        //   用峰值判断才真正稳定。
        if (towerNum > towerPeak) towerPeak = towerNum;
        // 【用户 2026-09-21：“第三次进攻结束以后不需要建箭塔了”】
        //   必须连这段一起关：`towerGoal = max(arrowTowerTarget, towerPeak)`，
        //   光把 arrowTowerTarget 设成 0 还是会被 towerPeak（“打坏几个补几个”）顶回来。
        if (phase < 3) {
            int towerGoal = arrowTowerTarget;
            if (towerPeak > towerGoal) towerGoal = towerPeak;
            if (arrowTowerResearched && towerNum + active_build(BUILDING_ARROWTOWER) < towerGoal
                && info.Stone >= BUILD_ARROWTOWER_STONE) {
                Task t;
                t.id = nextTaskId++;
                t.type = TASK_BUILD;
                t.priority = 1;
                t.buildingType = BUILDING_ARROWTOWER;
                taskQueue.push_back(t);
            }
        }
    }

    // 【2026-09 用户要求】原来这里会调 demand_dropoff()：某种资源离最近的存放建筑
    // 超过 10 格就在它旁边补建一座谷仓/仓库。后果就是“一天到晚在建仓库”（每种资源
    // 一座，开局只剩几个名额，动不动就花 120 木），而村民还是跑得一样远。
    // 现在改成“**先认准已有的存放建筑，再去它边上找资源**”（选资源的主判据换成
    // nearest_dropoff_dist，见 demand_gather / assign_tasks），自然就不需要补仓库了。
}
// 资源点 (dr,ur) 到"最近的可用存放建筑"的距离。
double nearest_dropoff_dist(int resType, double dr, double ur)
{
    bool berryFood = (resType == RESOURCE_BUSH);
    double best = 1e18;
    for (tagBuilding &b : info.buildings) {
        if (b.Percent < 100) continue;
        bool can = (b.Type == BUILDING_CENTER)
                || (berryFood && b.Type == BUILDING_GRANARY)
                || (!berryFood && b.Type == BUILDING_STOCK);
        if (!can) continue;
        double d = calDistance(dr, ur,
                               b.BlockDR * BLOCKSIDELENGTH,
                               b.BlockUR * BLOCKSIDELENGTH);
        if (d < best) best = d;
    }
    return best;
}

// 打猎前置条件：看得见的瞪羚里，至少有一只离"可用存放建筑"
// （仓库/谷仓/市中心）不超过 HUNT_DROP_RADIUS 格。
// 为假 = 猎物太远、来回搬肉太亏，此时先补建仓库（见 demand_dropoff）再打猎。
bool hunt_dropoff_ready()
{
    double best = 1e18;
    for (tagResource &r : info.resources) {
        if (r.Type != RESOURCE_GAZELLE) continue;
        if (r.Cnt <= 0 && r.Blood <= 0) continue;
        if (block_beyond_home(r.BlockDR, r.BlockUR, GATHER_MAX_DIST)) continue;  // 太远的猎物不算数
        double d = nearest_dropoff_dist(RESOURCE_GAZELLE, r.DR, r.UR);
        if (d < best) best = d;
    }
    return best <= HUNT_DROP_RADIUS * BLOCKSIDELENGTH;
}

// ---------- 生产需求：村民 ----------
void demand_produce()
{
    int farmerNum = 0;
    for (tagFarmer &f : info.farmers)
        if (f.FarmerSort == FARMERTYPE_FARMER) farmerNum++;

    // ---- 造村民（全程持续，直到 20 人）----
    // 【食物保底】复合弓科技（180 食物）没升完之前，家里先留够 TECH_FOOD_RESERVE：
    // 村民一个 50 食物，远处被杀掉一个市中心就补一个，食物被这个无底洞吃光，
    // 科技就永远点不出来（用户 2026-09 实测）。
    // 只在铜器之后（phase>=2）启用：开局食物刚好 200，那时卡住会把开局毁掉。
    // 【用户 2026-09 反馈“食物被消耗了但不知道去哪”—— 就消耗在这一句的后面】
    //   行为树顺序是 build → produce → army → research，即**造村民排在研发前面**。
    //   食物刚涨到 TECH_FOOD_RESERVE 时，上面那句条件为假（不 return）→ 先花 50
    //   造一个村民 → 食物掉回 150 → 后面的 demand_research 看到 150 < 180，
    //   科技升不了；食物再涨回 200 → 又造一个…… 反复循环，食物全被这个吃掉。
    //   所以：① 冲刺窗口（compositeBowUrgent()）内**一个村民都不造**，食物全给科技；
    //        ② 平时也按“科技成本 + 一个村民”留（见 TECH_FOOD_RESERVE）。
    if (phase >= 2 && !compositeBowReady()
        && (compositeBowUrgent() || info.Meat < TECH_FOOD_RESERVE)) return;
    if (farmerNum < 20
        && info.Human_Num + 1 <= info.Human_MaxNum
        && info.Meat >= BUILDING_CENTER_CREATEFARMER_FOOD
        && center_free()
        && active_action(BUILDING_CENTER, BUILDING_CENTER_CREATEFARMER) == 0) {
        Task t;
        t.id = nextTaskId++;
        t.type = TASK_PRODUCE;
        t.priority = 0;
        t.buildingType = BUILDING_CENTER;
        t.targetSN = BUILDING_CENTER_CREATEFARMER;
        taskQueue.push_back(t);
    }
}

// ---------- 修塔（“第三波之前场上一定要有 3 个箭塔”）----------
// 【内核事实】把己方**已建成**的建筑点给村民 = CoreEven_FixBuilding 修理：
//   Core.cpp:1016 → interactionList->addRelation(self, obj, CoreEven_FixBuilding)；
//   Core_List.cpp:174 只在**满血**时驳回（ACTION_INVALID_HUMANACTION_BUILDNOTNEEDFIX），
//   而半成品（Percent<100）走的是同一条关系 = 续建。
//   修理代价 = REPAIR_COST_RATIO(0.5) × 修好的血量比例 × 原造价
//   （Building::tryDeductRepairHpCost）→ 塔半血修满只要 ~37 石。
// 【为什么不靠 towerPeak 补建】补建要 150 石 + 80 秒工时，石头不够就一直排不上；
//   修一座没倒的塔又快又便宜，正好卡在“第二波打完 → 第三波（14:00）”这段空档里。
void demand_repair()
{
    // 只在“前两波打完、第三波还没到”这段窗口里修
    if (phase >= 3) return;   // 第三波之后敌方不再上门，塔也不用修了
    if (info.GameFrame < (int)(REPAIR_FROM_MIN * 60 * 1000.0 / TimePerFrame)) return;
    if (bt_enemy_at_home()) return;   // 还在打 → 别把村民派出去送

    // 目标 = 血**最少**的那座已建成的箭塔
    //   （不用浮点百分比：直接交叉相乘比 Blood/MaxBlood，避免除法/精度问题）
    tagBuilding *worst = nullptr;
    for (tagBuilding &b : info.buildings) {
        if (b.Type != BUILDING_ARROWTOWER) continue;
        if (b.Percent < 100) continue;                            // 半成品交给建造任务续建
        if (b.MaxBlood <= 0 || b.Blood >= b.MaxBlood) continue;   // 满血：内核会驳回
        if (worst == nullptr
            || b.Blood * worst->MaxBlood < worst->Blood * b.MaxBlood) worst = &b;
    }
    if (worst == nullptr) {           // 没有要修的塔
        repairTargetSN = -1;
        repairFarmerSN = -1;
        return;
    }
    if (worst->SN != repairTargetSN) {   // 换目标 → 重新挑人
        repairTargetSN = worst->SN;
        repairFarmerSN = -1;
    }

    // 已经派出去的村民还在忙（走去 / 正在修）→ 什么都别做：
    //   重下一遍会先 suspendRelation 再重建，修理进度从头开始。
    if (repairFarmerSN != -1) {
        for (tagFarmer &f : info.farmers) {
            if (f.SN != repairFarmerSN) continue;
            if (f.NowState != HUMAN_STATE_IDLE) return;
            break;
        }
    }
    // 节流：修理关系被内核因“行动无用”中断时，别每帧重下（参考建造任务的 resend 节流）
    if (info.GameFrame - repairOrderFrame < REPAIR_ORDER_INTERVAL_MS / TimePerFrame) return;

    for (tagFarmer &f : info.farmers) {
        if (f.FarmerSort != FARMERTYPE_FARMER) continue;
        if (on_build_task(f.SN)) continue;   // 手上还有没建完的工地，拉走就烂尾了
        if (!farmer_available(f)) continue;  // 内核说他忙 / 我刚派过他
        HumanAction(f.SN, worst->SN);
        repairFarmerSN = f.SN;
        repairOrderFrame = info.GameFrame;
        // 保护他：同一帧稍后的 assign_tasks / demand_gather 兜底 2 不许抢
        mark_farmer_order(f.SN);
        return;   // **只派一个村民**（用户要求）
    }
}

// ==================== 石头的总需求 ====================
// 【现在还需不需要采石？】
// 【为什么抽成函数】demand_gather（决定派几个人）和 recycle_tasks（决定回收哪个任务）
//   必须用**同一个判据**，否则就会出现“不再派新人、但已经在挖的任务永远不被回收”
//   那道裂缝 —— 村民会一直挖到矿空（用户 2026-09-23：“石头挖到 1020 个”）。
//
// 【★ 2026-09-24 重写：判据从“还要建几座塔”改成“**还差多少石头**”】
//   用户：“问题是石头现在根本不够啊”。查出来的账：
//     石头一共有三个用途，而旧判据 `towerCnt < 3 && Stone < 150` 只认第一个 ——
//       ① 建塔 150 石/座；
//       ② **修塔**：`Building.cpp:460 tryDeductRepairHpCost` 用的是
//          `REPAIR_COST_RATIO(0.5) × 本次修好的血量比例 × 原造价`，
//          而箭塔的原造价是 **150 石** ⇒ 半血塔修满要 ~37 石，快死了要 ~75 石；
//       ③ 谷仓“箭塔升级”科技 50 石。
//   旧判据的后果（第三波前必然发生）：开局 150 石 → 第二座塔花光 → 采到 150 →
//   第三座塔花光 → **towerCnt 变成 3，`stone_needed()` 立刻永远为假** ⇒
//   11:00 派去修塔的那个村民面对“0 石”，`tryDeductRepairHpCost` **直接 return false、
//   一点血都修不回来** ⇒ 塔修不好、村民站在塔下干等（看起来就是“定住不工作”），
//   “箭塔升级”也永远点不上。
//
//   现在的口径：`Stone < stone_demand_left()`，即“手里的石头够不够付掉所有欠账”。
//   `recycle_tasks` 用的是同一个函数，所以采石工会一直挖到欠账付清才被调走，
//   也不会像旧版那样停在“刚好 150”上。
static int stone_demand_left()
{
    int need = 0;

    // ① 还要建的箭塔（在造的也算：demand_build 的判据是
    //    `towerNum + active_build(ARROWTOWER) < towerGoal`，这里必须对齐）。
    //    goal 取 “目标 与 历史峰值(打坏几个补几个)” 的较大者。
    if (phase < 3) {
        int towerNum = 0;
        for (tagBuilding &b : info.buildings)
            if (b.Type == BUILDING_ARROWTOWER) towerNum++;
        int goal = arrowTowerTarget;
        if (towerPeak > goal) goal = towerPeak;
        const int left = goal - towerNum - active_build(BUILDING_ARROWTOWER);
        if (left > 0) need += left * BUILD_ARROWTOWER_STONE;
    }

    // ② 修塔的欠账（**这是原来漏掉的那一项**）。只算已建成、且没满血的箭塔。
    //    系数直接读 config.json 的 REPAIR_COST_RATIO（与内核同一份配置），
    //    **不能在文件作用域做 static const 初始化** —— 那会在 RuntimeConfig
    //    读 config.json 之前就取值，只能拿到默认值。
    if (phase < 3) {
        const double rr = REPAIR_COST_RATIO;
        for (tagBuilding &b : info.buildings) {
            if (b.Type != BUILDING_ARROWTOWER) continue;
            if (b.Percent < 100 || b.MaxBlood <= 0 || b.Blood >= b.MaxBlood) continue;
            const double lost = 1.0 - (double)b.Blood / (double)b.MaxBlood;
            need += (int)(rr * lost * (double)BUILD_ARROWTOWER_STONE);
        }
    }

    // ③ 谷仓“箭塔升级”科技（+1 攻 +1 射程）的 50 石。
    //    【用户 2026-09-24 定了口径：“所需石头量包括修塔的石头和建造新的塔的石头”】
    //    —— 它不属于“必须采”的那两笔账，所以**只在反正要开矿（need > 0）时顺手多留**。
    //    否则它会**单独把采石任务常年挂起来**：塔建满、塔也修好之后，账上就只剩
    //    这 50 + 周转 20，于是一直有个人在那挖 —— 用户看到的“后期石头又多了”
    //    就是它（而那时石头根本没有真用途）。
    if (need > 0) {
        for (ResearchState &rs : researches)
            if (rs.action == BUILDING_GRANARY_ARROWTOWE_UPGRADE && rs.level < rs.maxLevel) {
                need += BUILDING_GRANARY_UPGRADE_ARROWTOWER_STONE;
                break;
            }
    }

    // ④ 周转余量：上面的欠账都是按血量比例取整算的，而内核是**逐次扣整数**的，
    //    一点余量都不留时“第一刀”可能就扣不动（扣不动就一点血都修不回）。
    //    20 足够（半座塔的 1/7），也远不会撞上 STONE_KEEP_MAX(400)。
    if (need > 0) need += 20;

    return need;
}

// 【硬闸】15:00 之后一律禁止采石（用户 2026-09-23）。
//   为什么必须有这条：下面那套“按欠账算”（stone_demand_left）逻辑上是对的，
//   但**现场控不住** —— 兜底 2 直接下指令派去采矿的村民手上没有任务，
//   recycle_tasks 按任务回收管不到他们，一座矿（CNT_STONE=250）会被一个人挖空。
//   用户的口径：与其调参，不如到点全停。
static bool stone_forbidden_now()
{
    const int tpf = (TimePerFrame > 0) ? TimePerFrame : 40;
    return (long long)info.GameFrame * tpf >= (long long)STONE_STOP_MIN * 60000;
}

bool stone_needed()
{
    // 硬闸放在这里，是为了让**所有**路径一次到位：demand_gather 的派人数（wantStone）、
    //   兜底 1/兜底 2 的石料候选、recycle_tasks 的“石头够了就回收”读的都是它。
    if (stone_forbidden_now()) return false;
    return info.Stone < stone_demand_left();
}

// ---------- 采集需求 ----------
void demand_gather()
{
    int farmerNum = 0;
    for (tagFarmer &f : info.farmers)
        if (f.FarmerSort == FARMERTYPE_FARMER) farmerNum++;

    // ---- 采集需求（食物 / 木头 / 石头 / 黄金按需分配）----
    int total = farmerNum > 0 ? farmerNum : 1;

    // 【2026-09-23】采石需求抽成 stone_needed()，与 recycle_tasks 的回收
    //   **共用同一个判据** —— 否则会出现“不再派新人、但老任务永远不被回收”
    //   那道裂缝（石头会一直涨到矿空，见 recycle_tasks 里石头那一段）。
    bool needStone = stone_needed();

    int wantStone = 0;
    if (needStone) {
        // 【用户 2026-09-24：“派一人去采不够”】
        //   算一下一个人的产能：一趟只背 FARMER_CARRYLIMIT_STONE(10) 石，
        //   采满背包要 10 / FARMER_GATHERSPEED_STONE(0.02) = **500 帧 ≈ 20 秒**
        //   （config.json，TimePerFrame=40），再加来回交货的路 —— 150 石（一座塔）
        //   一个人要 **6~9 分钟**。而塔的时间线是“进铜器就建第二座、14:00 前第三座”，
        //   中间还要留出修塔那 ~37 石，一个人根本等不起。
        //   所以按 stone_demand_left() **精确**派人数：欠账越大派的人越多。
        //   【为什么不会重演“挖到 1020 石”】那次是“只派 1 个人”在兜底 ——
        //   真正的闸在 recycle_tasks：它用**同一个判据**
        //   （`Stone >= stone_demand_left()` 或 `Stone >= STONE_KEEP_MAX(400)`）
        //   一到线就把所有人调走（send_gatherer_to_wood），跟派几个人无关。
        //   同时按总人口封一下（前期只有 8~12 个村民时派 3 个人采石太奢侈）。
        const int need = stone_demand_left();
        wantStone = (need > BUILD_ARROWTOWER_STONE)             ? 3
                  : (need > BUILD_ARROWTOWER_STONE / 2)         ? 2 : 1;
        const int stoneCap = total / 3;
        if (stoneCap >= 1 && wantStone > stoneCap) wantStone = stoneCap;
        if (wantStone < 1) wantStone = 1;
    }

    // 黄金：**按欠账动态算**（用户 2026-09-24：“后期金不够，请对资源数量做出动态调整”）
    //   判据见 gold_demand()/gold_needed()。要点：
    //   · 还在补复合弓兵 → 至少 2 人常驻（两座靶场 ≈80 金/分的固定支出，
    //     一个人 ≈25~30 金/分，必然断供 ⇒ 靶场干等金）；
    //   · 金离需求还差 100 以上 → 3 人冲；
    //   · 已经攒够（gold_needed() 假）→ **一个都不派**（不能只留 1 人：那样任务
    //     会被 recycle_tasks 每帧收掉又重建，村民在矿和树之间来回跑）。
    int wantGold = 0;
    if (gold_needed() && has_resource(RESOURCE_GOLD)) {
        wantGold = rushing_composite_bowman() ? 2 : 1;
        if (info.Gold + 100 < gold_demand()) wantGold = 3;
        const int goldCap = total / 3;
        if (goldCap >= 1 && wantGold > goldCap) wantGold = goldCap;
    }

    int farmCnt = 0;
    for (tagBuilding &b : info.buildings)
        if (b.Type == BUILDING_FARM && b.Percent >= 100 && b.Cnt > 0) farmCnt++;

    // ---- 食物：固定 FOOD_GATHERERS 人采浆果（只采前期城边那几丛）----
    // 不用百分比：开局 8 个村民里按比例分，食物只剩 2~3 人，
    // 木头永远攒不起来（冲铜器建筑链要 545 木）。
    // 只算"城边 BUSH_NEAR_RADIUS 格内"的浆果丛：这几丛采完（或压根没探到）
    // 就结束浆果阶段，不再跑远去采别的浆果丛，名额转给农田/打猎/伐木。
    double homeDR = -1, homeUR = -1;
    for (tagBuilding &b : info.buildings) {
        if (b.Type == BUILDING_CENTER) {
            homeDR = b.BlockDR * BLOCKSIDELENGTH;
            homeUR = b.BlockUR * BLOCKSIDELENGTH;
            break;
        }
    }
    int nearBush = 0;
    for (tagResource &r : info.resources) {
        if (r.Type != RESOURCE_BUSH || r.Cnt <= 0) continue;
        if (homeDR < 0) { nearBush++; continue; }   // 异常（没市中心）：不设距离限制
        if (calDistance(homeDR, homeUR, r.DR, r.UR)
            <= BUSH_NEAR_RADIUS * BLOCKSIDELENGTH) nearBush++;
    }
    if (nearBush > 0) berrySeen = true;
    if (berryPhase && berrySeen && nearBush == 0) berryPhase = false;   // 锁存：不再回头采浆果

    int wantBush = 0;
    if (berryPhase)
        wantBush = nearBush < FOOD_GATHERERS ? nearBush : FOOD_GATHERERS;

    // ---- 打猎（瞪羚）开闸 ----
    // 【2026-09 用户反馈"现在瞪羚优先级是不是很低，前期采瞪羚的还是少"】原因有两个：
    //   ① 开闸要求 hunt_dropoff_ready()（瞪羚附近得先有仓库），而仓库要等市场建好
    //      才拍（见 demand_dropoff），于是前期一直没人去打猎；
    //   ② 人数按 total×HUNT_PERCENT，人口少时比例算出来就一两个。
    // 现在的策略：
    //   · 开闸只看"人口/木头到位"（或食物断供的例外），**不再等仓库**；
    //   · 仓库没到位时先派 HUNT_EARLY 个人去打（来回搬肉远一点，但人少浪费有限，
    //     食物收入能提前起来）；仓库到位后按 HUNT_PERCENT 放开派。
    // 开闸后锁存常开，不再随木头存量波动来回切。
    bool foodCut = (wantBush == 0 && berrySeen
                    && farmCnt == 0 && farmTarget == 0
                    && info.GameFrame >= HUNT_STARVE_MIN_FRAME);
    if (!huntStarted
        && (foodCut || (farmerNum >= HUNT_START_POP
                        && info.Wood >= HUNT_START_WOOD)))
        huntStarted = true;

    // 打猎是"额外增加"的采集位，不从 FOOD_GATHERERS 里挤（浆果那 6 人不动）。
    int wantHunt = 0;
    if (huntStarted && has_resource(RESOURCE_GAZELLE)) {
        if (hunt_dropoff_ready()) {
            wantHunt = total * HUNT_PERCENT / 100;
            if (wantHunt < 1) wantHunt = 1;
            // 【用户 2026-09-24】第三波之前让出 HUNT_EARLY_TRIM 个名额给伐木
            //   （打猎至少保留 1 人）。后期（phase>=3）不让：那时食物才是瓶颈。
            if (phase < 3) {
                wantHunt -= HUNT_EARLY_TRIM;
                if (wantHunt < 1) wantHunt = 1;
            }
        } else {
            wantHunt = HUNT_EARLY;         // 仓库还没好：先派少量人，别让来回搬肉拖垮
            if (wantHunt > total) wantHunt = total;
        }
    }

    // ---- 木头富余 → 多派人打猎 ----
    // 自由木头 = 存量 − 队列里还没建成的建筑要花的木头（pending_build_wood）。
    // 有富余还在堆人砍树就是浪费：食物永远不嫌多（造村民/升铜器/造兵全靠食物）。
    // 只在"已经开闸打猎"之后生效，免得绕过"先拍仓库再打猎"那个前置。
    if (huntStarted && has_resource(RESOURCE_GAZELLE)) {
        int freeWood = info.Wood - pending_build_wood();
        if (freeWood >= HUNT_WOOD_SURPLUS) {
            wantHunt += HUNT_SURPLUS_EXTRA;
        }
    }

    // ---- 农田：一个村民对应一格农田 ----
    // 想派 farmTarget 个人种田（数量在 bt_sync 里按人口算），
    // 但实际只能派到"已经建好的农田数"为止（内核一块农田只认一个采集者）。
    // 农田采完会被自动删除，demand_build 会补建，人数就跟着农田数一起长。
    int wantFarm = farmTarget;
    if (wantFarm > farmCnt) wantFarm = farmCnt;

    // ---- 伐木人数软上限：多出来的人转去打猎（用户要求"减少伐木人口"）----
    // 放在"农田"算完之后、木材保底之前，这样 restNow 才是完整的伐木人数。
    // 上限本身是"软"的：下面的木材保底会把打猎人数压回去，伐木自然涨回来。
    if (huntStarted && has_resource(RESOURCE_GAZELLE)) {
        int restNow = total - wantStone - wantGold - wantBush - wantHunt - wantFarm;
        // 上限用 wood_gather_limit()（已含“第三波前 8 / 之后 6”与物理容量），
        // 而不是写死的 WOOD_MAX_GATHERERS —— 否则“分给木头”会被那个旧值封掉。
        int move = restNow - wood_gather_limit();
        if (move > 0) wantHunt += move;
        if (wantHunt > HUNT_MAX_GATHERERS) wantHunt = HUNT_MAX_GATHERERS;
    }

    // ---- 木材保底 ----
    // 后期虽然食物为主，但房屋 / 补仓库 / 农田本身 / 科技都还要木头。
    // 名额不够时按"打猎 → 农田"的顺序往回缩（农田是后期食物主力，最后动）。
    while (total - wantStone - wantGold - wantBush - wantHunt - wantFarm
           < WOOD_MIN_GATHERERS) {
        if (wantHunt > 0) wantHunt--;
        else if (wantFarm > 0) wantFarm--;
        else break;
    }

    // ---- 其余劳动力全部伐木 ----
    // "造出来的村民，除了被派去拍建筑的，全都去砍树"：
    // 建筑任务优先级更高（sort_tasks 里 1~2 < 采集 3~4），会先把空闲村民领走，
    // 所以这里把剩余名额全给木头就不会漏掉建造。
    int rest = total - wantStone - wantGold - wantBush - wantHunt - wantFarm;
    if (rest < 1) rest = 1;
    // 【2026-09 用户反馈"砍树的人太多导致卡死"，这里必须封顶】
    //   以前是无条件 `wantWood = rest`：所有没被食物/金/石分走的人全去砍树，
    //   而上面那条 WOOD_MAX_GATHERERS 只是"**能打猎时才生效的软上限**"
    //   （没有瞪羚 / 打猎还没开闸时完全不起作用），人口一多就能有十几人涌进同一片林子。
    //   树林里的空地是有限的（而且密林里相邻几棵树的站位还互相重叠，见 wood_capacity()），
    //   人比落脚点多的时候整片林子会互相挤死：谁都贴不到树上、内核判"行动无用"
    //   中断关系、村民变 IDLE、又被重派过去 —— 看起来就是"一群伐木工卡在树林里"。
    //   现在按 wood_gather_limit() 硬封顶，多出来的人**不再派任务**（留 IDLE 待命）：
    //   派过去也是白跑，反而把林子堵死。
    //   ★ 光封这里还不够：下面的"兜底 1 / 兜底 2"也会把剩余空闲村民倒给木头，
    //     那两处也必须按同一个配额来（否则这里的封顶会被一笔勾销）。
    int woodLimit = wood_gather_limit();
    int wantWood = rest;
    if (wantWood > woodLimit) wantWood = woodLimit;
    const int woodCap = wood_capacity();   // 只用于下面的日志

    if (has_resource(RESOURCE_BUSH)) {
        while (active_gather(RESOURCE_BUSH) < wantBush) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_GATHER; t.priority = 3;
            t.resourceType = RESOURCE_BUSH;
            taskQueue.push_back(t);
        }
    }
    if (has_resource(RESOURCE_GAZELLE)) {
        while (active_gather(RESOURCE_GAZELLE) < wantHunt) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_GATHER; t.priority = 3;
            t.resourceType = RESOURCE_GAZELLE;
            taskQueue.push_back(t);
        }
    }
    if (farmCnt > 0) {
        while (active_gather(GATHER_FARM) < wantFarm) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_GATHER; t.priority = 3;
            t.resourceType = GATHER_FARM;
            taskQueue.push_back(t);
        }
    }
    if (has_resource(RESOURCE_TREE)) {
        while (active_gather(RESOURCE_TREE) < wantWood) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_GATHER; t.priority = 3;
            t.resourceType = RESOURCE_TREE;
            taskQueue.push_back(t);
        }
    }
    if (has_resource(RESOURCE_GOLD)) {
        while (active_gather(RESOURCE_GOLD) < wantGold) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_GATHER; t.priority = 3;
            t.resourceType = RESOURCE_GOLD;
            taskQueue.push_back(t);
        }
    }
    if (has_resource(RESOURCE_STONE)) {
        while (active_gather(RESOURCE_STONE) < wantStone) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_GATHER; t.priority = 4;
            t.resourceType = RESOURCE_STONE;
            taskQueue.push_back(t);
        }
    }

    // ---- 兜底 1：按"真实空闲村民数"补采集任务 ----
    // 旧实现用 taskQueue 里的任务数反推空闲人数，一旦有任务卡在 WAITING
    // （找不到目标）就会少算，导致村民站着不动。
    int idleNow = 0;
    for (tagFarmer &f : info.farmers)
        if (f.FarmerSort == FARMERTYPE_FARMER && f.NowState == HUMAN_STATE_IDLE
            && !on_build_task(f.SN))   // 建造中的村民不算"空闲可派"
            idleNow++;

    int waitingGather = 0;
    for (Task &t : taskQueue)
        if (t.type == TASK_GATHER && t.state == TASK_WAITING) waitingGather++;

    int spare = idleNow - waitingGather;
    if (spare > 0) {
        // 依次尝试"还有存货"的资源类型，把空闲村民都安排上
        const int fallbackTypes[] = { RESOURCE_TREE, RESOURCE_STONE, RESOURCE_GOLD,
                                      RESOURCE_BUSH, RESOURCE_GAZELLE };
        int nTypes = (int)(sizeof(fallbackTypes) / sizeof(fallbackTypes[0]));
        for (int k = 0; k < nTypes && spare > 0; k++) {
            if (fallbackTypes[k] == RESOURCE_GAZELLE && !huntStarted)
                continue;   // 开局不杀瞪羚：兜底也不能把村民拉去打猎
            if (fallbackTypes[k] == RESOURCE_BUSH && !berryPhase)
                continue;   // 浆果阶段已结束：不再回头采浆果
            // 【需求为零的资源不当候选 —— 用户 2026-09 反馈“后期石头太多”】
            //   这一条兜底和下面的“兜底 2”都只看“地图上还有没有这种资源”，
            //   不看**要不要**：箭塔建满、石头堆成山了还派人去挖，
            //   等于白占一个劳动力（不如去伐木/种田）。
            if (fallbackTypes[k] == RESOURCE_STONE && !needStone) continue;
            if (fallbackTypes[k] == RESOURCE_GOLD  && wantGold == 0) continue;
            if (!has_resource(fallbackTypes[k])) continue;
            int allow = spare;
            if (fallbackTypes[k] == RESOURCE_TREE) {
                // 【伐木配额（用户 2026-09“砍树的人太多导致卡死”）】
                //   这条兜底原来是把 spare **全部**倒给第一个有存货的类型（= 木头），
                //   于是上面 wantWood 的封顶被它一笔勾销 —— 空闲村民还是全去砍树。
                //   现在只补到 wood_gather_limit() 为止，而且配额满了就**不再往下流转**：
                //   主流程刚才是按“该有几个伐木工”定过名额的，兜底的职责只是补漏，
                //   不该绕过这个决定（多出来的人留 IDLE 待命）。
                int left = wood_gather_limit() - active_gather(RESOURCE_TREE);
                if (left < 0) left = 0;
                if (left == 0) break;
                if (allow > left) allow = left;
            }
            for (int i = 0; i < allow; i++) {
                Task t;
                t.id = nextTaskId++; t.type = TASK_GATHER; t.priority = 5;
                t.resourceType = fallbackTypes[k];
                taskQueue.push_back(t);
            }
            spare -= allow;
        }
    }

    // ---- 兜底 2：assign_tasks 之后**仍然**空闲的村民 → 就近派一份采集活 ----
    // 排在 dispatch 之后（详见 build_behavior_tree）；树优先，其次石/金/猎物。
    // 【必须按“站位”分配，不能各挑各的最近】否则一群人同时空下来时会**全被派到
    //   同一棵树**，只有一两个能挤进去，其余原地挤着“砍不到”。所以按
    //   res_stand_spots()（该点周围能站几人）分配，并把本帧已派的人数（sentNow）
    //   也算上；实在所有点都站满了，才退回“就近硬挤”（总比站着不动强）。
    {
        std::unordered_map<int,int> sentNow;   // 本帧兜底已经派到每个资源点的人数
        // 【伐木配额（用户 2026-09“砍树的人太多导致卡死”）】
        //   这段兜底原来是**没有总数上限**的第二处：只有单点站位限制
        //   （res_stand_spots），主流程的 wantWood 封顶会被它绕过。
        //   现在先算出还剩几个伐木名额（woodLeft），配额用完就不把树当候选，
        //   多出来的村民宁可去采石/金/打猎，也不去把林子堵死。
        // 【为什么这段兜底必须排在 dispatch 之后】详见 build_behavior_tree 里的说明：
        //   · 排在前面 → 它用 HumanAction 直接抢人，把该去建造的村民抢走了（"拍了建筑不建"）；
        //   · 改成"给建造留 N 个人" → 建造任务一旦选址失败，被留下的村民会**每帧被跳过**、
        //     永远站着不动。
        //   排在 dispatch 之后，assign_tasks 先把正经任务派完，这里拿到的才是真正剩下的人。
        int woodLeft = wood_gather_limit() - active_gather(RESOURCE_TREE);
        if (woodLeft < 0) woodLeft = 0;
        // 【2026-09-24 用户：“为什么还是有村民定住不工作”】
        //   上面那个 woodLeft 是**政策**名额（WOOD_MAX_GATHERERS = 6）。用完就把树
        //   整个排除出候选 —— 于是“石头建满（needStone 假）/ 金子堆够（wantGold==0）/
        //   浆果打完 / 猎物打完”之后，空闲村民就只剩“站着不动”这一条路了
        //   （原来上面那段注释里写的“多出来的人不再派任务，留 IDLE 待命”就是它）。
        //   这里准备下面第 3 趟要用的**内核真值**：每座矿旁边现在有几个自己人。
        ensure_cutter_at_tree();
        // 村民自己就站在危险区里（敌人/狮子靠近）→ 先撤回家，别在野外继续干活。
        // 用户反馈“后期村民跑太远被杀了”：死一个就是 50 食物 + 一个劳动力，
        // 而市中心会不停造人来补，把食物吃掉（见 TECH_FOOD_RESERVE）。
        double homeDR = 0, homeUR = 0;
        const bool haveHome = home_center(homeDR, homeUR);
        for (tagFarmer &f : info.farmers) {
            if (f.FarmerSort != FARMERTYPE_FARMER) continue;
            // 正负责建造的村民即使这一帧看着空闲（建造关系被内核断了）也不能拉走：
            // 拉走就烂尾了，recycle_tasks 会把它们叫回工地。
            if (on_build_task(f.SN)) continue;
            // 【统一判据】内核说他忙 / 我刚派过他 → 一律不碰（见 farmer_available）。
            // 位置必须在“危险撤离”之前：内核 deduplicateInstructions 保留靠后的一条，
            // 撤离先写会把同一帧刚下发的建造指令顶掉。
            if (!farmer_available(f)) continue;

            if (haveHome && gather_spot_dangerous(f.DR, f.UR)) {
                // 【不能直接朝市中心跑】home_center() 返回的是市中心**建筑本体**
                //   那一格的块中心，单位必然站不上去（walk 不过去）——
                //   recall_priest_home 的注释里已经踩过这个坑（“回村目标不能取
                //   市镇中心自己占的块，会让单位卡住”）。这里用 find_home_spot()
                //   找一个真正的可站立落脚点。
                // 【必须节流】否则村民只要保持 IDLE 且身处危险半径内，就会**每帧**
                //   重下 HumanMove，内核每次 addRelation 都 suspendRelation
                //   （initAction + 清空路径）→ 村民原地拖动、永远走不出去。
                const double bsl = BLOCKSIDELENGTH;
                int bx = -1, by = -1;
                if (find_home_spot(bx, by, 0)) {
                    int gap = 2000 / TimePerFrame;
                    if (gap < 1) gap = 1;
                    std::unordered_map<int,int>::iterator itE = escapeFrame.find(f.SN);
                    if (itE == escapeFrame.end()
                        || info.GameFrame - itE->second >= gap) {
                        HumanMove(f.SN, bx * bsl, by * bsl);
                        escapeFrame[f.SN] = info.GameFrame;
                    }
                }
                continue;
            }

            int pick = -1;
            bool pickIsTree = false;
            // 三趟：
            //   0 = 优选（树要政策名额；每个点不许超过它的站位）
            //   1 = 硬挤（无视站位上限，先把人派出去；树仍要政策名额）
            //   2 = 【最后一档】放宽伐木政策名额，但守树的物理站位
            //       —— 走到这一趟说明石/金/浆果/猎物一格都没有了，
            //          此时唯一的替代就是“站着不动”（用户 2026-09-24 的反馈）。
            for (int pass = 0; pass < 3 && pick == -1; pass++) {
                int treeSN = -1, anySN = -1;
                int treeUsed = 0x7fffffff, anyUsed = 0x7fffffff;   // 主判据：点上已派几人
                double treeD = 1e18, anyD = 1e18;          // 平手用：村民到资源的距离
                double treeHaul = 1e18, anyHaul = 1e18;    // 次判据：资源到最近存放建筑的距离
                for (tagResource &r : info.resources) {
                    if (r.Cnt <= 0 && r.Blood <= 0) continue;
                    // 【只打瞪羚，不打大象（用户 2026-09：得不偿失）】
                    //   config.json：瞪羚 BLOOD 8 / CNT 150；象 BLOOD 45 / CNT 300。
                    //   象还会还手（Animal.cpp：isRangeAttack = true、Elephant_Attack 音效），
                    //   而村民只有 25 血 —— 费半天劲才多拿 150 食物，人却可能搭进去。
                    //   所以象一律不进候选（原来它是候选之一，村民会跑过去打象）。
                    if (r.Type != RESOURCE_TREE && r.Type != RESOURCE_STONE
                        && r.Type != RESOURCE_GOLD && r.Type != RESOURCE_BUSH
                        && r.Type != RESOURCE_GAZELLE)
                        continue;
                    // 伐木名额用完了：把树整个排除出候选（剩下的名额让给石/金/猎物）
                    // 【第 3 趟例外】见下面 pass==2 的说明：那一趟是“再没活干就只能
                    //   站着不动”的最后一档，政策名额不再是理由。
                    if (r.Type == RESOURCE_TREE && pass < 2 && woodLeft <= 0) continue;
                    if (r.Type == RESOURCE_GAZELLE && !huntStarted)
                        continue;   // 开局不杀瞪羚
                    if (r.Type == RESOURCE_BUSH && !berryPhase)
                        continue;   // 浆果阶段结束：兜底也不再去采浆果
                    // 需求为零的资源不当候选（“后期石头太多”：塔建满、石堆成山还去挖）
                    if (r.Type == RESOURCE_STONE && !needStone) continue;
                    if (r.Type == RESOURCE_GOLD  && wantGold == 0) continue;
                    if (res_too_far(r.Type, r.BlockDR, r.BlockUR)) continue;   // 太远：不采
                    if (gather_spot_dangerous(r.DR, r.UR, gather_danger_radius(r.Type))) continue;   // 危险：不派
                    int spots = res_stand_spots(r.SN);
                    if (spots <= 0) continue;   // 走不到跟前，别白跑
                    // 这个点上"已经有人"的数量：队列里的任务 + 本帧兜底已经派过去的
                    const int usedHere = gatherers_on(r.SN) + sentNow[r.SN];
                    if (pass == 0 && usedHere >= spots) continue;   // 这个点已经站满了
                    // 【第 3 趟】最后一档：只守**物理站位**，判据用内核真值
                    //   （cutterAtTree：WorkObjectSN 指向这棵树的村民数，含“正走过去”的）。
                    //   为什么不能再用任务数：兜底自己下的指令不进任务队列。
                    //   一个站位格一个人 ⇒ 既不会把林子挤死（原来那个 bug），
                    //   也不会再有人站着不动。
                    if (pass == 2) {
                        if (r.Type != RESOURCE_TREE) continue;   // 石/金/浆果/猎物前两趟已试过
                        if (cutterAtTree[r.SN] + sentNow[r.SN] >= spots) continue;
                    }
                    double haul = nearest_dropoff_dist(r.Type, r.DR, r.UR);
                    double d = calDistance(f.DR, f.UR, r.DR, r.UR);
                    // 【主判据 = 这个点上的人】少的优先（与 assign_tasks 同一套道理，
                    //   见那里的说明：不然 6 丛浆果会被前 3 个人挤在同一丛上）。
                    //   第一遍按"人少"优先，第二遍（硬挤兜底）才按距离挑。
                    if (pass == 0) {
                        if (usedHere < anyUsed
                            || (usedHere == anyUsed && haul < anyHaul)
                            || (usedHere == anyUsed && haul == anyHaul && d < anyD))
                        { anyUsed = usedHere; anyHaul = haul; anyD = d; anySN = r.SN; }
                        if (r.Type == RESOURCE_TREE
                            && (usedHere < treeUsed
                                || (usedHere == treeUsed && haul < treeHaul)
                                || (usedHere == treeUsed && haul == treeHaul && d < treeD)))
                        { treeUsed = usedHere; treeHaul = haul; treeD = d; treeSN = r.SN; }
                    } else {
                        if (haul < anyHaul || (haul == anyHaul && d < anyD))
                        { anyHaul = haul; anyD = d; anySN = r.SN; }
                        if (r.Type == RESOURCE_TREE
                            && (haul < treeHaul || (haul == treeHaul && d < treeD)))
                        { treeHaul = haul; treeD = d; treeSN = r.SN; }
                    }
                }
                pick = (treeSN != -1) ? treeSN : anySN;
                pickIsTree = (treeSN != -1);
            }

            if (pick != -1) {
                HumanAction(f.SN, pick);
                // 与 assign_tasks / 兜底 1 抢人：派活是异步的，内核此刻还说他是 IDLE，
                // 不记一下的话下一帧 assign_tasks 会再给他派一个采集任务、把这条顶掉。
                mark_farmer_order(f.SN);
                sentNow[pick]++;
                if (pickIsTree) --woodLeft;   // 占掉一个伐木名额
            }
        }
    }

    // ---- 每 5 秒报一次采集人力分配（手动跑图调参用；嫌吵把这一段删掉即可）----
    //   看“伐木=?/?”这一项：分子是当前挂着的伐木任务数，分母是配额
    //   （括号里是“全图去重站位格数”= 树林真正能站几个人）。
    //   如果分子一直等于分母而村民还是挤着不动，说明堵的不是人数而是路，
    //   那就得看“林容量”是不是明显大于实际能用的落脚点。
    const int toFrames = (TimePerFrame > 0) ? TimePerFrame : 40;   // 帧 ↔ 毫秒换算
    if (info.GameFrame - gatherLogFrame >= 5000 / toFrames) {
        gatherLogFrame = info.GameFrame;
        // 待派建造任务数：如果这个数一直 > 0 而建筑又不动工，就是“没人/没地方建”
        int nBuildWait = 0;
        for (Task &t : taskQueue)
            if (t.type == TASK_BUILD && t.state == TASK_WAITING) nBuildWait++;
        // 这一帧结束时仍然空闲、且没被建造任务领走的村民数。
        // 【怎么用这两个数定位】
        //   待建>0 且 闲=0  → 没人可用（村民都被采集/建造占了）
        //   待建>0 且 闲>0  → 有人但 assignment/选址没成：多半是**找不到合法建造位**
        //                     （被 find_block / build_margin_clear / badBuildSite /
        //                       未探索区域 挡住），要看具体是哪种建筑
        int nIdleFarmer = 0;
        for (tagFarmer &f : info.farmers)
            if (f.FarmerSort == FARMERTYPE_FARMER
                && f.NowState == HUMAN_STATE_IDLE
                && !on_build_task(f.SN)) nIdleFarmer++;
        DebugText(std::string("采集: 村民=") + std::to_string(farmerNum)
                  + " 待建=" + std::to_string(nBuildWait)
                  + " 闲=" + std::to_string(nIdleFarmer)
                  + " 人口=" + std::to_string(info.Human_Num)
                  + "/" + std::to_string(info.Human_MaxNum)
                  + " 食=" + std::to_string((int)info.Meat)
                  + " 木=" + std::to_string((int)info.Wood)
                  + " 伐木=" + std::to_string(active_gather(RESOURCE_TREE))
                  + "/" + std::to_string(woodLimit)
                  + "(林容量=" + std::to_string(woodCap) + ")"
                  + " 浆果=" + std::to_string(active_gather(RESOURCE_BUSH))
                  + " 打猎=" + std::to_string(active_gather(RESOURCE_GAZELLE))
                  + " 农田=" + std::to_string(active_gather(GATHER_FARM))
                  + " 金=" + std::to_string(active_gather(RESOURCE_GOLD))
                  + " 石=" + std::to_string(active_gather(RESOURCE_STONE))
                  + " 复合弓科技=" + (compositeBowReady() ? "OK" : "未完成"));
    }
}

// ---------- 第二阶段：造兵需求 ----------
// 优先级：学院方阵兵 > 马厩骑兵 > 靶场弓箭手（复合弓科技升完后改出复合弓兵）。
// **兵营不造棍棒兵**（见函数末尾注释）。
// 每帧最多给一座空闲军事建筑下一条命令（建筑随后进入忙碌状态，自然不会重复下达）。
void demand_army()
{
    // 统计现有兵力（祭司不计入战斗兵；侦察兵单独算，也不计入战斗兵）
    // 注意：不再统计 AT_CLUBMAN（棍棒兵已不允许生产）、也不再统计 AT_HOPLITE
    // （学院已经不再建造，见 demand_build）—— 留着计数只会变成
    // "赋值但从不读取"的变量（GCC -Wall 会警告）。
    int bowman = 0, composite = 0, cavalry = 0, scout = 0, totalArmy = 0;
    bool weakKillAlive = false;            // weakKillSN 还在吗（见它的说明）
    for (tagArmy &a : info.armies) {
        if (a.SN == weakKillSN) weakKillAlive = true;
        if (a.Sort == AT_PRIEST) continue;
        if (a.Sort == AT_SCOUT) { scout++; continue; }
        totalArmy++;
        if (a.Sort == AT_COMPOSITE_BOWMAN) composite++;
        if (a.Sort == AT_BOWMAN || a.Sort == AT_COMPOSITE_BOWMAN
            || a.Sort == AT_SLINGER) bowman++;
        else if (a.Sort == AT_CAVALRY || a.Sort == AT_CHARIOT) cavalry++;
    }

    // **侦察骑兵“全局只造一个”的闩**（用户 2026-09 要求）：只要见过侦察兵
    // （自己造的或地图自带的）就置位，之后永远不再补造。
    // 【位置很关键】必须放在下面“人口已满 → return”之前：否则人口满时函数提前返回，
    // 这个闩永远置不上，侦察兵阵亡后还是会被补造。
    if (scout > 0) scoutEverMade = true;

    // 自裁标记的清理：那个兵已经死了（自裁成功/阵亡）就把标记清掉，
    // 否则 demand_attack 会永远跳过这个 SN。
    if (weakKillSN != -1 && !weakKillAlive) weakKillSN = -1;

    auto free_building = [&](int type) -> tagBuilding* {
        for (tagBuilding &b : info.buildings)
            if (b.Type == type && b.Percent >= 100 && b.Project == 0)
                return &b;
        return nullptr;
    };

    if (info.Human_Num + 1 > info.Human_MaxNum) {
        // ---- 自裁清理弱兵（用户要求：“造兵的时候可通过自裁清理较弱兵种”）----
        // 内核支持“自己删自己”：HumanAction(SN, SN)（目标 = 自己）→
        //   Core::handleMilitaryAction 里 self == obj 且 SORT_ARMY → deleteSelf()，
        //   把血量拉满即视为死亡，人口立刻腾出来。
        // 只处理“确定被淘汰”的：普通弓兵（复合弓科技升完后完全被复合弓兵替代，
        // 射程 5 对 7、攻击 3 对 5）、投石兵（25 血 / 射程 4）；棍棒兵已禁止生产，
        // 万一有残留也一并清掉。
        // 只在两件事同时成立时才动手：① 人口快满（不删就造不了新兵）；
        //   ② 不是在守家（守家时自减战力等于自杀）。
        if (phase >= 2 && !bt_enemy_at_home()
            && compositeBowReady()
            && info.Meat >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_FOOD
            && info.Gold >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_GOLD) {
            int interval = WEAK_KILL_INTERVAL_MS / TimePerFrame;
            if (interval < 1) interval = 1;
            if (info.GameFrame - weakKillFrame >= interval) {
                int weakSN = -1;
                for (tagArmy &a : info.armies) {
                    if (a.Sort == AT_CLUBMAN || a.Sort == AT_SLINGER) {
                        weakSN = a.SN; break;
                    }
                    if (a.Sort == AT_BOWMAN) { weakSN = a.SN; break; }
                }
                if (weakSN != -1) {
                    HumanAction(weakSN, weakSN);   // 自裁（目标 = 自己）
                    weakKillFrame = info.GameFrame;
                    weakKillSN = weakSN;           // 告诉 demand_attack 别顶掉它
                }
            }
        }
        return;   // 人口已满：下面那些造兵都排不进去
    }

    // ---- 侦察兵：专职探路。速度 4.07，是祭司（2.24）的 1.8 倍；
    //      马厩在**工具时代**就解锁它、而且只花食物（Development.cpp:700）。----
    // 【用户 2026-09 要求】**全局只造一个**（闩在函数开头已更新）。
    //   它阵亡了也不补（前期那一匹已经够探路，后期反攻靠复合弓兵 + 投石车 + 祭司）。
    // 【造得太早会拖慢铜器升级】所以要满足两个条件才造：
    //   ① phase >= 2（铜器已升完，不再跟 800 食物抢）；
    //   ② 已经过了 SCOUT_BUILD_MIN 分钟（正好赶上第三波后的探图）。
    if (!scoutEverMade && scout < SCOUT_UNITS
        && phase >= 2
        && info.GameFrame >= (int)(SCOUT_BUILD_MIN * 60 * 1000.0 / TimePerFrame)) {
        tagBuilding *st0 = free_building(BUILDING_STABLE);
        if (st0 && info.Meat >= BUILDING_STABLE_CREATE_SCOUT_FOOD) {
            BuildingAction(st0->SN, BUILDING_STABLE_CREATE_SCOUT);
            return;
        }
    }

    if (phase < 2) return;   // 铜器时代前不打仗，只留上面那个侦察兵

    // 【食物保底】复合弓科技冲刺窗口内，把食物留给科技：方阵兵 60 食、骑兵 40 食，
    // 而 demand_army 在行为树里排在 demand_research 前面 —— 不挡一下食物永远攒不到 180
    // （用户反馈“复合弓点不出来”）。科技一升完 compositeBowUrgent() 立刻变假，恢复正常造兵。
    // 【2026-09 加强】原来只在“食物 < TECH_FOOD_RESERVE”时才挡住，
    // 于是食物一涨到门槛线就会被造兵吃掉（与上面 demand_produce 造村民是同一个坑：
    // demand_army 也排在 demand_research 前面）。冲刺窗口内**一律不造兵**，
    // 食物/黄金全留给科技；科技一升完 compositeBowUrgent() 变假，立刻恢复正常。
    if (compositeBowUrgent()) return;

    // 【第三阶段：一切为复合弓服务】不能再按“总兵力”卡上限。
    //   用户 2026-09 反馈“复合弓没造出来就开推了”的实际链路：
    //   第三波防守打完手里往往已经有十几个兵（方阵兵/骑兵/早期弓手），
    //   而第三阶段 target = armyTarget + 8 = 24，只够再造 8 个复合弓 ——
    //   推图要 ASSAULT_BOWMAN_MIN(18) 个，于是永远凑不齐，集结一直不成立，
    //   最后只能拖到 23:00 的底线档强推上去。
    //   所以“复合弓还没凑够推图数量”时**忽略总兵力上限**，一直造到够为止
    //   （人口是硬限制，满了会由上面的自裁腾位置）。
    const bool rushComposite = (phase >= 3) && compositeBowReady()
                               && composite < ASSAULT_BOWMAN_MIN;
    int target = (phase >= 3) ? armyTarget + 8 : armyTarget;
    if (!rushComposite && totalArmy >= target) return;

    // 【用户要求 2026-09】扛过第三波（phase>=3）之后**全部为复合弓兵服务**：
    // 马厩（骑兵 40 食 + 黄金）停产（学院/方阵兵已按用户要求删除），
    // 食物/黄金全留给复合弓兵（40 食 + 20 金/个）与复合弓科技（180 食）。
    // 推图的输出主力本来就是复合弓兵（后面还有投石车拆塔、祭司转化）。
    // 想改回"第三阶段也补近战"，把这段 if 的 phase<3 条件去掉即可。
    if (phase < 3) {
        // 马厩：骑兵（需食物 + 黄金）
        tagBuilding *st = free_building(BUILDING_STABLE);
        if (st && info.civilizationStage >= CIVILIZATION_BRONZEAGE
            && cavalry < (target + 2) / 3
            && info.Meat >= BUILDING_STABLE_CREATE_CAVALRY_FOOD
            && info.Gold >= BUILDING_STABLE_CREATE_CAVALRY_GOLD) {
            BuildingAction(st->SN, BUILDING_STABLE_CREATE_CAVALRY);
            return;
        }
    }

    // 靶场：弓箭手（远程，主力）。复合弓科技升完后改出复合弓兵。
    // 【让位】复合弓科技冲刺窗口内（rangeReservedForResearch），靶场先别造兵：
    //   demand_army 在行为树里排在 demand_research 前面，靶场一空就会被造兵订单
    //   抢走，科技永远排不上队——这正是复合弓拖到 20 分钟以后还没升完的原因。
    // 弓兵名额（用户 2026-09-23 定的口径：“除了开局的两个弓箭手不允许造其他弓箭手”）：
    //   · 科技升完之前 → **只造 BOWMAN_PRE_TECH_MAX(2) 个普通弓兵**（就是开局那两个），
    //     之后一律不再造；
    //   · 科技升完之后 → 名额只数复合弓兵（早期那两个普通弓兵不占名额，靠上面的
    //     自裁清弱兵回收），**任何情况下都不再出普通弓兵**。
    int bowmanForTarget = compositeBowReady() ? composite : bowman;
    int bowmanCap = compositeBowReady() ? (target + 1) / 2 : BOWMAN_PRE_TECH_MAX;
    // 推图至少要 ASSAULT_BOWMAN_MIN 个复合弓兵：别被兵力公式算小了卡住名额
    if (compositeBowReady() && bowmanCap < ASSAULT_BOWMAN_MIN)
        bowmanCap = ASSAULT_BOWMAN_MIN;
    tagBuilding *rg = free_building(BUILDING_RANGE);
    if (rg && !rangeReservedForResearch() && bowmanForTarget < bowmanCap) {
        // 科技已升完 → **只**出复合弓兵（40 食物 + 20 黄金）；不够就空转等，绝不退回普通弓兵。
        //   【用户 2026-09-23：“现在除了开局的两个弓箭手不允许造其他弓箭手”】
        //   这里原来有条“金不够就落下去造普通弓兵”的回落：普通弓兵射程 5/攻 3（复合弓 7/5）、
        //   而且**不占 bowmanCap 名额**（名额数的是 composite）⇒ 金一低于 20 靶场每 30 秒
        //   就冒一个，白花 40 食 + 20 木还占人口；更糟的是“自裁清弱兵”要求 Gold>=20 才敢删人
        //   ⇒ 金一断就既造不出复合弓兵、也清不掉旧弓兵，人口被占死。现在**整条取消**。
        if (compositeBowReady()) {
            if (info.Meat >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_FOOD
                && info.Gold >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_GOLD) {
                BuildingAction(rg->SN, BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN);
            }
            return;      // 够不够都到此为止：不许再出普通弓兵
        }
        // 科技没升完：只补到 BOWMAN_PRE_TECH_MAX(2) 个普通弓兵 ＝“开局那两个”
        if (info.Meat >= BUILDING_RANGE_CREATE_BOWMAN_FOOD
            && info.Wood >= BUILDING_RANGE_CREATE_BOWMAN_WOOD) {
            BuildingAction(rg->SN, BUILDING_RANGE_CREATE_BOWMAN);
            return;
        }
    }

    // 兵营：**不造棍棒兵**（用户明确要求禁掉）。
    // 理由：棍棒兵（AT_CLUBMAN，atk 最低、无护甲科技）在铜器时代之后毫无价值，
    // 造出来不但占人口（Human_MaxNum 被房屋卡着），还会被拉去防守白送。
    // 【2026-09】兵营的两条科技（战斧升级 / 阔剑科技）也一并从 init_researches 删了，
    // 所以兵营现在**既无生产也无研发** —— 它只剩建筑链里的一个占位。
    // 注意：demand_army 下面那条"camp 造兵"分支已经删掉了，这里刻意不再补。
}

// ---------- 第二阶段：科技研发 ----------
// 研发清单（两级科技的同一 Action 调用两次即可，用 level 追踪进度）
void init_researches()
{
    researches.clear();
    auto add = [&](const char *name, int btype, int action, int maxLevel,
                   int food, int wood, int stone, int gold,
                   int food2, int wood2, int stone2, int gold2) {
        ResearchState r;
        r.name = name;
        r.buildingType = btype;
        r.action = action;
        r.maxLevel = maxLevel;
        r.food = food; r.wood = wood; r.stone = stone; r.gold = gold;
        r.food2 = food2; r.wood2 = wood2; r.stone2 = stone2; r.gold2 = gold2;
        researches.push_back(r);
    };

    // 经济类（市场）—— 越靠前优先级越高
    add("伐木加工", BUILDING_MARKET, BUILDING_MARKET_WOOD_UPGRADE, 1,
        BUILDING_MARKET_WOOD_UPGRADE_FOOD, BUILDING_MARKET_WOOD_UPGRADE_WOOD, 0, 0,
        0, 0, 0, 0);
    // 伐木加工是**工具时代**就解锁的科技（Development.cpp：
    //   conditionDevelop(CIVILIZATION_TOOLAGE, BUILDING_MARKET, ..._CUTTING, ...)），
    // 效果又特别猛：采集速度 +50%、背包 +2、远程防御 +1
    // （BUILDING_MARKET_WOOD_UPGRADE_ADDITION_GATHERRATE / _CARRY / _DISSHOOT）。
    // 所以它不受"进铜器后才研发"的限制：minPhase=1 → 市场一建好就点，
    // 越早点，后面每个人砍木头的效率越高（用户要求"先点出伐木科技"）。
    for (ResearchState &r : researches)
        if (r.action == BUILDING_MARKET_WOOD_UPGRADE) { r.minPhase = 1; break; }
    add("驯养动物", BUILDING_MARKET, BUILDING_MARKET_FARM_UPGRADE, 1,
        BUILDING_MARKET_FARM_UPGRADE_FOOD, BUILDING_MARKET_FARM_UPGRADE_WOOD, 0, 0,
        0, 0, 0, 0);
    add("金矿开采", BUILDING_MARKET, BUILDING_MARKET_GOLD_UPGRADE, 1,
        BUILDING_MARKET_GOLD_UPGRADE_FOOD, BUILDING_MARKET_GOLD_UPGRADE_WOOD, 0, 0,
        0, 0, 0, 0);
    // 【用户 2026-09-23：“你车轮科技有什么用？我不造战车弓兵！”】
    //   査源码后的事实（Development.cpp）：
    //     · `finishAction` 里 `rate_FarmerMove = 0.3`，而 `get_rate_Move` 对
    //       `SORT_FARMER` 返回 **1 + 0.3 = 1.3** ⇒ **村民移速 +30%**
    //       —— 这是它对我们**唯一**的价值；
    //     · 另一项是**解锁战车 / 战车弓箭手**的前置（Development.cpp:709 / 741），
    //       而本 AI 从来不造这两种兵 ⇒ 这部分对我们完全无用。
    //   造价 150 食 + 100 木 + 40 秒。对 20+ 村民的经济体，+30% 移速确实值，
    //   但它**不紧急** —— 不像复合弓科技那样卡着整个进攻时间线。
    //   ⇒ 按用户的意思把它**降到最后**（原来排在经济类第 3，常出现在复合弓
    //     科技之前就把食物/木头花掉，抢了冲 800 食升铜器的资源）。
    add("车轮",     BUILDING_MARKET, BUILDING_MARKET_WHEEL_UPGRADE, 1,
        BUILDING_MARKET_WHEEL_UPGRADE_FOOD, BUILDING_MARKET_WHEEL_UPGRADE_WOOD, 0, 0,
        0, 0, 0, 0);

    // 军事类（兵营 / 靶场）
    // 【2026-09 删除】兵营那两条科技现在**全部移除**了，兵营只剩下建筑链里的占位：
    //   · 战斧升级（BUILDING_ARMYCAMP_UPGRADE_CLUBMAN）：只强化棍棒兵，
    //     而棍棒兵已经禁止生产（见 demand_army），研发它纯属白花食物。
    //   · 阔剑科技（BUILDING_ARMYCAMP_UPGRADE_BROADSWORD）：查过 Development.cpp:610-621，
    //     它的**唯一**前置用户是“训练阔剑兵（BUILDING_ARMYCAMP_CREATE_BROADSWORD）”，
    //     而本 AI 从来不造阔剑兵（demand_army 只出方阵兵/骑兵/弓箭手/复合弓兵）。
    //     也就是说 140 食 + 50 金 + 40 秒研发换不到任何东西，还占着兵营的研发位
    //     跟仓库/靶场那几条抢食物 —— 所以按用户要求整条删除。
    //   ⚠ 兵营（BUILDING_ARMYCAMP）**必须保留，绝对不能省**：
    //     Development.cpp:697/727 里 **马廐和靶场都以兵营为建造前置**
    //     （buildCon->addPreCondition(developLab[BUILDING_ARMYCAMP].buildCon)），
    //     而升铜器要求 {市场, 靶场, 马廐} >= 2 —— 没兵营就建不出靶场/马廐，
    //     永远进不了铜器，复合弓科技也无从谈起。造价 125 木 / 30 秒。
    add("复合弓科技", BUILDING_RANGE, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW, 1,
        BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_FOOD,
        BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_WOOD, 0, 0, 0, 0, 0, 0);
    // 复合弓科技是**硬截止科技**：用户要求 20 分钟前必须升完。
    // 它在上面这份清单里排第 8，食物/木头常被前面的科技先花掉，
    // 又和"造弓箭手"抢同一座靶场（demand_army 排在 demand_research 前面，
    // 靶场一空就先被造兵订单抢走），很容易拖到 20 分钟以后。
    // 所以给它一个"到点插队"的窗口：过了 urgentFromFrame 还没升完，
    // demand_research 就把它提到最前面下单，demand_army 也把靶场让出来。
    {
        int msPerMin  = 60 * 1000;
        int framesPer = TimePerFrame > 0 ? TimePerFrame : 40;
        int msBuild   = TIME_BUILDING_RANGE_UPGRADE_COMPOSITE_BOW * 1000;  // TIME_* 单位：秒
        // 按 action 找，而不是 researches.back()——以后往清单里插新科技也不会错位
        for (ResearchState &bow : researches) {
            if (bow.action != BUILDING_RANGE_UPGRADE_COMPOSITE_BOW) continue;
            bow.deadlineFrame = (COMPOSITE_BOW_DEADLINE_MIN * msPerMin) / framesPer;
            bow.urgentFromFrame = bow.deadlineFrame
                                - msBuild / framesPer
                                - (COMPOSITE_BOW_MARGIN_MIN * msPerMin) / framesPer;
            if (bow.urgentFromFrame < 0) bow.urgentFromFrame = 0;
            break;
        }
    }

    // 守备类（谷仓）：升级箭塔攻击/射程
    add("箭塔升级", BUILDING_GRANARY, BUILDING_GRANARY_ARROWTOWE_UPGRADE, 1,
        BUILDING_GRANARY_UPGRADE_ARROWTOWER_FOOD, 0,
        BUILDING_GRANARY_UPGRADE_ARROWTOWER_STONE, 0, 0, 0, 0, 0);

    // 攻防类（仓库）：两级科技（一级 → 二级）
    add("工具使用", BUILDING_STOCK, BUILDING_STOCK_UPGRADE_USETOOL, 2,
        BUILDING_STOCK_UPGRADE_CLOSER_ATTACK_FOOD, 0, 0, 0,
        BUILDING_STOCK_UPGRADE_CLOSER_ATTACK_2_FOOD, 0, 0,
        BUILDING_STOCK_UPGRADE_CLOSER_ATTACK_2_GOLD);
    add("步兵护甲", BUILDING_STOCK, BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY, 2,
        BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY_FOOD, 0, 0, 0,
        BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY_2_FOOD, 0, 0,
        BUILDING_STOCK_UPGRADE_DEFENSE_INFANTRY_2_GOLD);
    add("弓兵护甲", BUILDING_STOCK, BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER, 2,
        BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER_FOOD, 0, 0, 0,
        BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER_2_FOOD, 0, 0,
        BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER_2_GOLD);
    // 【2026-09 删除】骑兵护甲（BUILDING_STOCK_UPGRADE_DEFENSE_RIDER）：
    //   查过 Development.cpp:184-189 —— 它只被算进 `DEFENSE_RIDER` 的防御加成里，
    //   **没有任何科技/单位以它为前置**，删掉不会卡住别的研发。
    //   而骑兵只在 phase<3 时造（第三阶段"全部为复合弓服务"一律停产），
    //   推图的主力是复合弓兵（骑兵只是过渡兵种）—— 花 275 食 + 100 金给它们加防御不划算。
    //   删掉后仓库的护甲线只剩：步兵护甲、弓兵护甲（见上面两条）。
}

// 下单一条研发：先用 ins_ret 判断上一条是否成功/已满级，再按资源与空闲建筑下新单
void request_research(ResearchState &r)
{
    if (r.buildingType < 0) return;
    if (r.level >= r.maxLevel) return;
    // 阶段门槛按科技单独设（默认 2 = 进铜器后；伐木加工是 1 = 工具时代就能点）
    if (phase < r.minPhase) return;

    // 1) 处理上一帧在研指令的返回值
    if (r.pendingId >= 0) {
        auto it = info.ins_ret.find(r.pendingId);
        if (it == info.ins_ret.end()) {
            // ins_ret 一直不回来 → 那条指令被内核去重丢掉了（见 UsrAI.h 的
            // researchBuildingUsed 说明），必须超时重试，否则这条科技永久卡死。
            if (info.GameFrame - r.pendingFrame > 2000 / TimePerFrame) {
                r.pendingId = -1;
            }
            return;
        }
        int ret = it->second;
        if (ret == 0) r.level++;                        // 指令被接受：等级 +1
        else if (ret == ACTION_INVALID_BUILDACT_LOCK)   // 已研发过/已达上限
            r.level = r.maxLevel;
        // 其他错误（资源不足、建筑忙碌等）不改变等级，之后重试
        r.pendingId = -1;
        return;
    }

    // 2) 资源门槛（二级看二级消耗）
    int needFood  = (r.level == 0) ? r.food  : r.food2;
    int needWood  = (r.level == 0) ? r.wood  : r.wood2;
    int needStone = (r.level == 0) ? r.stone : r.stone2;
    int needGold  = (r.level == 0) ? r.gold  : r.gold2;
    if (info.Meat < needFood || info.Wood < needWood
        || info.Stone < needStone || info.Gold < needGold) return;

    // 3) 找一座空闲的对应建筑下单
    //    **同一帧同一座建筑只能下一条**（researchBuildingUsed 是本帧已用过的 SN，
    //    原因见 UsrAI.h 里它的说明），否则市场 5 条科技只有最后一条能升、其余全卡死。
    for (tagBuilding &b : info.buildings) {
        if (b.Type != r.buildingType) continue;
        if (b.Percent < 100) continue;
        if (b.Project != 0) continue;
        if (std::find(researchBuildingUsed.begin(), researchBuildingUsed.end(),
                      b.SN) != researchBuildingUsed.end()) continue;
        r.pendingId = BuildingAction(b.SN, r.action);
        r.pendingFrame = info.GameFrame;
        researchBuildingUsed.push_back(b.SN);
        return;
    }
}

// 复合弓科技相关查询：三个函数都只扫 researches（数量极小），每帧调用无所谓。
bool compositeBowReady()
{
    for (ResearchState &r : researches)
        if (r.action == BUILDING_RANGE_UPGRADE_COMPOSITE_BOW)
            return r.level >= r.maxLevel;
    return false;
}

bool compositeBowUrgent()
{
    for (ResearchState &r : researches)
        if (r.action == BUILDING_RANGE_UPGRADE_COMPOSITE_BOW) {
            if (r.level >= r.maxLevel) return false;
            // 【用户要求 2026-09】扛过第三波（phase>=3）之后一切为复合弓服务：
            // 不等 16:20 那个冲刺窗口，一到第三阶段就把它提到最前面抢资源
            // （用户反馈"复合弓兵没出来"）。
            return (r.deadlineFrame > 0 && info.GameFrame >= r.urgentFromFrame)
                || phase >= 3;
        }
    return false;
}

// 靶场是否需要让位给复合弓科技。
// 冲刺窗口内一律让位（哪怕资源暂时不够）：让位后不再拿食物去造弓箭手，
// 食物才能攒到 180；否则"靶场一直造兵 → 食物永远不到 180 → 科技永远开不了"
// 会死循环。代价是靶场在这段时间可能空转，但冲刺窗口是 16:20 起、只到 20:00，
// 换"复合弓准时到位"是值得的。
bool rangeReservedForResearch()
{
    return compositeBowUrgent();
}

// 现有复合弓兵数（rushing_composite_bowman / gold_demand 共用）
static int composite_bowman_count()
{
    int n = 0;
    for (tagArmy &a : info.armies)
        if (a.Sort == AT_COMPOSITE_BOWMAN) n++;
    return n;
}

// “正在冲复合弓兵”（用户 2026-09 要求“优先供给造复合弓兵”）：
//   条件 = 复合弓科技**已经升完**（否则靶场也造不了兵，该让位的是科技）
//          且现有复合弓兵 < 推图要的 ASSAULT_BOWMAN_MIN(18) 个。
// 调用点：demand_research（暂停要花食物/黄金的研发）、
//         demand_gather（采金人数）、gold_needed()（金够不够）。
bool rushing_composite_bowman()
{
    if (!compositeBowReady()) return false;
    return composite_bowman_count() < ASSAULT_BOWMAN_MIN;
}

// 金的需求量 = “还要造的复合弓兵”换算成金 + 周转
//   【用户 2026-09-24：“后期金不够，请对资源数量做出动态调整”】
//   旧写法是“按人口拍比例”（total/5、total/8、再减一），跟真实消耗对不上：
//   两座靶场 30 秒各出一个复合弓兵 = 40 金 / 30 秒 ≈ **80 金/分的固定支出**，
//   而 1 个采金村民只有 ≈25~30 金/分 ⇒ 必然断供、靶场干等金。
//   现在按欠账算：max(0, 推图底线 - 现有复合弓兵) × 20 金 + GOLD_KEEP_MAX(300)。
//   300 是周转额（兵一直在死要一直补，相当于 15 个兵的钱）。
static int gold_demand()
{
    const int left = ASSAULT_BOWMAN_MIN - composite_bowman_count();
    return (left > 0 ? left : 0)
           * BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_GOLD + GOLD_KEEP_MAX;
}

// 还要采金吗？—— demand_gather（派几个人）与 recycle_tasks（该不该把人调走）
//   **共用这一个判据**（石头那边是 stone_needed()，同一个模式）。
static bool gold_needed()
{
    if (phase < 2) return false;
    return info.Gold < gold_demand();
}

void demand_research()
{
    // **不再整体 gate 在 phase >= 2**：阶段门槛下放到每条科技的 minPhase
    // （伐木加工 minPhase=1，工具时代市场一建好就能点；其余默认 2）。
    if (researches.empty()) init_researches();

    // 本帧已下过研发单的建筑（同一帧同一座建筑只能下一条，理由见 request_research）
    researchBuildingUsed.clear();

    // "进入冲刺窗口的硬截止科技"= 有 deadlineFrame、已经到了 urgentFromFrame、
    // 且还没升完（目前只有复合弓）。
    auto isUrgent = [&](ResearchState &r) -> bool {
        // 硬截止科技：进冲刺窗口就算急；**第三阶段（扛过第三波）之后一律算急**。
        // 为真 = 它就是 `researchRank` 里的第 0 档（**绝对最前**）：
        // 仓库护甲、工具使用、经济科技全部让位（用户 2026-09 最终要求）。
        return r.deadlineFrame > 0 && r.level < r.maxLevel
            && (info.GameFrame >= r.urgentFromFrame || phase >= 3);
    };

    // ---- 下单顺序：用一张"档位表"明确写死，按档位从小到大依次下单 ----
    // 为什么"顺序"真的有用：`request_research` 里的资源门槛读的是**本帧**的
    //   `info.Meat/Wood/...`，而 `BuildingAction` 是异步的 —— 内核会**按这些指令
    //   被下发的先后顺序**去扣资源。同一帧里排在前面的科技先把食物/木头拿走，
    //   排在后面的就可能因"资源不足"被驳回（下一帧再重试）。
    //   【不要靠"仓库忙了复合弓自然能插进来"—— 那是碰运气，不是优先级。】
    //
    // 【用户 2026-09 最终要求：**护甲也让位** —— 复合弓冲刺时绝对最前】
    //   0) **复合弓科技**（`isUrgent`：进冲刺窗口，或扛过第三波 phase>=3 之后）
    //   1) 其余全部（仓库护甲 / **工具使用** / 经济 / 军事其余），按清单原顺序
    //   理由：复合弓硬截止是 COMPOSITE_BOW_DEADLINE_MIN(20) 分钟，而它一共只要
    //   180 食 + 100 木、研发 40 秒左右，升完就永久让出资源；护甲/工具使用那几条
    //   没有时间压力，晚几十秒升完全无妨。反过来则可能把复合弓拖过 20 分钟，
    //   整个反攻的输出主力就没了。
    auto researchRank = [&](ResearchState &r) -> int {
        return isUrgent(r) ? 0 : 1;
    };
    // 【用户 2026-09 要求：“优先供给造复合弓兵”】
    //   复合弓科技升完后，接下来的瓶颈是**兵**（40 食 + 20 金/个），
    //   而仓库那几条研发的二级要 200 食 + 120 金 —— 它们会把食物/黄金吃掉，
    //   靶场就只能干等“资源不足”，兵迟迟出不来（连带推图一直不开始）。
    //   所以在复合弓兵攒够 ASSAULT_BOWMAN_MIN 个之前：
    //     **暂停一切要花食物或黄金的研发**，资源全留给靶场造兵。
    //   （复合弓兵攒满后立即恢复；木头/石头的研发本来就不多，且不受影响。）
    const bool feedBowman = rushing_composite_bowman();
    auto needsFoodOrGold = [&](ResearchState &r) -> bool {
        return (r.level == 0) ? (r.food > 0 || r.gold > 0)
                              : (r.food2 > 0 || r.gold2 > 0);
    };
    for (int rank = 0; rank <= 1; ++rank)
        for (ResearchState &r : researches) {
            if (researchRank(r) != rank) continue;
            if (feedBowman && needsFoodOrGold(r)) continue;   // 让位给造兵
            request_research(r);
        }
}

// ---------- 第三阶段：集结 → 诱杀野战军 → 齐射拆箭塔 → 祭司转化（胜利条件）----------
//
// 作战流程（2026-09 用户要求）：
//   ① 第三波（14:00，phase>=3）防守打完后侦察骑兵才出门，在离家
//      （位置由 record_enemy_positions 每帧记，看到建筑/部队都算）→ 记录位置；
//   ② 所有战斗兵到敌营外的集结点集合（集结点保证在敌方箭塔射程之外）；
//   ③ 攒够 ASSAULT_BOWMAN_MIN(18) 个复合弓兵才开始推图；
//   ④ 野战军清完后**全军齐射**那座集火中的箭塔（不区分兵种，见下面投石车那段）；
//      最后由祭司转化敌方武器工程厂；
//   ⑤ **敌方野战军没清完之前，任何兵种都不主动进入敌方箭塔射程**
//      （箭塔**真实射程 10 格**、复合弓兵只有 7 格，站上去对射就是拿 45 血换 125 血）；
//      清完之后才压上去拆塔 —— 这时不再避让射程（对射不可避免，靠齐聚火抢时间）。
//   ⑥ **拉锯诱杀，但两段都是全军一起走**（用户 2026-09-22 两次要求合起来）：
//      进状态 2 后全军一起压到勾引线（BAIT_TRIGGER_DIST=19，踩进守军 20 格警戒圈），
//      站够 ASSAULT_PUSH_HOLD_MS（**从全队到位起算**）再一起退回诱杀线
//      （ASSAULT_STAGE_DIST=24，出警戒圈 ⇒ 守军抓不到目标、回原位）；
//      站够 ASSAULT_RETREAT_HOLD_MS 再压上…… 循环磨光野战军。
//      正在交战的单位不会被叫回头，会先把眼前这个打死。
//      全程没人上钩满 ASSAULT_BAIT_TIMEOUT_MS → 判定野战军清完，转拆塔。
//   ⑦ 兜底：两档开打门槛（硬上限 30:00）——
//      18 个复合弓（主力档，**不设时间上限**）→ 23:00 一到就**不管几个兵都开打**。
//      （“等 2 分钟就降到 6 个”那条已按用户要求删除。）
//
// 【投石车（AT_STONE_THROWER）】它只能由敌方武器工程厂（BUILDING_SIEGE=11）生产，
//   而 11 号建筑在 config.json 的 PLAYER_DISABLED_BUILDINGS 里：Core::filter_instruction
//   对玩家 0 的 INS_HUMANBUILD 一律回 ACTION_INVALID_HUMANBUILD_LOCK，
//   **我们造不出投石车**。但这不代表没得用 —— **祭司可以转化敌方的投石车**
//   （第三波里就有 2 台；Core_List.cpp 里转化成功会 setPlayerRepresent 变成我方），
//   所以实战中大概率是“有投石车”的那条路径。两种都要能跑：
//   拆塔时**不再区分兵种**（原来的“投石车独占 / 近战肉盾 + 弓兵第二排”已删）：
//   **全军齐射同一座塔**。注意这是**对射** —— 投石车射程 10，而敌方箭塔真实射程
//   也是 10（7+3 科技），没有“站在塔打不到的地方拆塔”这回事；复合弓兵更吃亏
//   （7 格）。靠的是齐聚火在最短时间内把塔（125 血）打掉，用换血换时间。
//
// 【绝不攻击武器工程厂】它是胜利条件（要祭司转化它才算赢），被我们自己打掉就再也
//   赢不了 —— 所以拆建筑时**只从敌方箭塔里挑目标**，绝不碰其它敌方建筑。

// 兵种分类（tagArmy 没暴露 attackType，只能按 Sort 对应 Army.cpp 里的 case）：
//   ATTACKTYPE_SHOOT → 远程（含投石兵、各种弓兵、战船）
//   AT_STONE_THROWER → 攻城：射程 10 格，专拆箭塔
// （原来的 army_is_melee 随“三排配合”一起删了：现在拆塔是全军齐射，不再分批进场。）
static bool army_is_siege(int sort) { return sort == AT_STONE_THROWER; }

// 敌方兵种的**实际**攻击范围（格）。
// 【数据来源】基础值取自 config.json 的 DIS_* 键（引擎启动时读进去的运行时真值），
//   已核对：DIS_STONE_THROWER=10 / DIS_SHIP=10 / DIS_COMPOSITE_BOWMAN=7 /
//     DIS_CHARIOT_ARCHER=7 / DIS_IMPROVEDBOWMAN1=6、DIS_IMPROVEDBOWMAN2=7 /
//     DIS_BOWMAN=5 / DIS_SLINGER=4 / DIS_PRIEST=12。
//   近战的 DIS_* 是 0，Army.cpp:385 里退化成 `DISTANCE_ATTACK_CLOSE + 目标半宽`
//   = 17.888/35.777 + 半格 ≈ 1 格（DISTANCE_ATTACK_CLOSE 单位是细节坐标）。
//   DIS_* 本身单位就是**格**（Army.cpp:385 另一分支要 `* BLOCKSIDELENGTH`）。
// **改这张表时请对着 config.json + Development.cpp 核，别凭印象。**
// **我方**兵种的攻击范围（格）。这里**不加**“敌方科技全满”的加成（那是给对面算的）。
static double own_attack_range(int sort)
{
    switch (sort) {
    case AT_STONE_THROWER:    return DIS_STONE_THROWER;      // 10
    case AT_SHIP:             return DIS_SHIP;               // 10
    case AT_COMPOSITE_BOWMAN: return DIS_COMPOSITE_BOWMAN;   // 7
    case AT_CHARIOT_ARCHER:   return DIS_CHARIOT_ARCHER;     // 7
    case AT_IMPROVED:         return DIS_IMPROVEDBOWMAN2;    // 7（按 2 级算）
    case AT_BOWMAN:           return DIS_BOWMAN;             // 5
    case AT_SLINGER:          return DIS_SLINGER;            // 4
    default:                  return 1.0;                    // 近战：≈1 格
    }
}

// 我方市镇中心的细节坐标（取块中心）。false = 中心还没建成（异常情况）。
static bool home_center(double &dr, double &ur)
{
    const double bsl = BLOCKSIDELENGTH;
    for (tagBuilding &b : info.buildings) {
        if (b.Type != BUILDING_CENTER || b.Percent < 100) continue;
        dr = (b.BlockDR + 0.5) * bsl;
        ur = (b.BlockUR + 0.5) * bsl;
        return true;
    }
    return false;
}

// (dr,ur) 到最近的**可见**敌方箭塔的距离（格）；没有可见箭塔时返回一个很大的值。
// 注意迷雾：只有探索过/在视野里的敌方建筑才会出现在 info.enemy_buildings 里，
// 所以推进路上会"走一段、发现一座"，这是正常的。
double nearest_enemy_tower_dist(double dr, double ur)
{
    const double bsl = BLOCKSIDELENGTH;
    double best = 1e18;
    for (tagBuilding &eb : info.enemy_buildings) {
        if (eb.Type != BUILDING_ARROWTOWER || eb.Percent < 100) continue;
        double d = calDistance(dr, ur, eb.BlockDR * bsl, eb.BlockUR * bsl);
        if (d < best) best = d;
    }
    return best / bsl;
}

bool point_in_enemy_tower_range(double dr, double ur, double marginBlocks)
{
    return nearest_enemy_tower_dist(dr, ur)
           <= (double)(DIS_ARROWTOWER + ENEMY_DIS_ADD_TOWER) + marginBlocks;
}

// 敌方“厂区祭司猎手小队”还剩几个（word 文档：3 骑兵 + 2 战车射手）。
// 只数**机动兵种**：步兵追不上速度 2.24 的祭司，真正能威胁到它的只有
// 骑兵(1.3 间隔/速度快)、四马战车、战车射手。祭司上场（状态 4）前要等它归零。
// 注意 info.enemy_armies 是**带迷雾**的（只在视野内），所以猎手退回厂区深处
// 看不见时这里会报 0 —— 那正是我们想要的结果（它们不在祭司必经之路上）。
int enemy_hunter_count()
{
    int n = 0;
    for (tagArmy &e : info.enemy_armies) {
        if (e.Sort == AT_CAVALRY || e.Sort == AT_CHARIOT
            || e.Sort == AT_CHARIOT_ARCHER) n++;
    }
    return n;
}

// ---------- 敌方位置记录 ----------
// 【什么时候记录（用户 2026-09 两次澄清）】
//   **只在第三阶段（第三波防守打完 14:00、侦察骑兵出门探图）之后才记**：
//   前两波的敌人是打上门来的，拿他们的位置当“敌营位置”会把反攻目标带偏；
//   而且那时的当务之急是防守，不是登记敌营坐标。
//   所以函数开头 `phase < 3` 直接 return（不记录也不锁存）。
// 【记什么】“不但看到敌方建筑要记录，看到单位也要记录”（用户要求）：
//   取“**离我家最远**”的敌方建筑 / 敌方部队（一起比），只往更远处单调更新。
//   · 为什么取最远：敌人可能在我方附近补箭塔/派小股部队，那些不是基地；基地总在远处。
//   · 武器工程厂（BUILDING_SIEGE）单独记：它是胜利目标，看到就实时刷新，优先级最高。
//   · 两个列表都是**带迷雾**的（只有探索/视野内的才发给我们），所以必须锁存
//     enemyFarDR/UR 一份，不然一走出视野就丢。
// 由 `bt_sync`（行为树第一个节点）每帧调用，但真正开始记要等 phase>=3。
void record_enemy_positions()
{
    if (phase < 3) return;      // 第三阶段之前不记录（见上）

    const double bsl = BLOCKSIDELENGTH;
    double homeDR = 0, homeUR = 0;
    const bool haveHome = home_center(homeDR, homeUR);

    // ① 胜利目标：武器工程厂
    for (tagBuilding &eb : info.enemy_buildings) {
        if (eb.Type != BUILDING_SIEGE) continue;
        enemySiegeSN = eb.SN;
        enemySiegeDR = eb.BlockDR * bsl;
        enemySiegeUR = eb.BlockUR * bsl;
    }

    // ② 敌营大致位置（锁存、单调往更远处更新）
    double recD = enemyFarFound && haveHome
                  ? calDistance(enemyFarDR, enemyFarUR, homeDR, homeUR) : -1.0;
    for (tagBuilding &eb : info.enemy_buildings) {
        if (eb.Type == BUILDING_SIEGE) continue;      // 上面已经单独记了
        double dR = eb.BlockDR * bsl;
        double dU = eb.BlockUR * bsl;
        double d = haveHome ? calDistance(dR, dU, homeDR, homeUR) : 0.0;
        if (d > recD) {
            recD = d;
            enemyFarDR = dR;
            enemyFarUR = dU;
            enemyFarFound = true;
        }
    }
    // ③ 敌方部队也算（用户要求）：敌人从基地走过来，方向就是基地方向。
    //    贴到我家门口打的那几支不会被记（它们离得近，不会比已记录的更远）。
    for (tagArmy &e : info.enemy_armies) {
        double d = haveHome ? calDistance(e.DR, e.UR, homeDR, homeUR) : 0.0;
        if (d > recD) {
            recD = d;
            enemyFarDR = e.DR;
            enemyFarUR = e.UR;
            enemyFarFound = true;
        }
    }
    // ④ 侦察骑兵阵亡 = “那个方向那个位置有敌兵”（用户 2026-09 原话：
    //    “打死了不就说明那个方向那个位置有敌兵？直接冲不就好了？”）。
    //    所以把它活着时的最后位置当成一次“目击”喂进上面那套记录（只做一次），
    //    这样即使它什么都没来得及看到就被打死，部队也有个方向可冲 ——
    //    第三阶段也就**不需要**再搞“祭司绝望探图”了（见 demand_scout）。
    //    要求阵亡点离家 > HOME_DEFEND_RADIUS：在自家门口被流兵打死的不能当敌营位置。
    bool scoutAlive = false;
    for (tagArmy &a : info.armies)
        if (a.Sort == AT_SCOUT) {
            scoutAlive = true;
            scoutLastDR = a.DR;
            scoutLastUR = a.UR;
            break;
        }
    if (scoutAlive) {
        scoutSeenAlive = true;
    } else if (scoutSeenAlive) {
        scoutSeenAlive = false;              // 只处理一次
        // 【2026-09】已经拿到武器工程厂的准确位置（enemySiegeSN）之后，侦察兵的消失
        //   不再有信息量：它现在是“探到敌营就自裁”（见 demand_scout），
        //   死在盯人点上只会把 enemyFar 带偏。
        // 【用户 2026-09-24 声明：“侦察骑兵死亡的位置就是敌方大本营大致位置！”】
        //   所以这里**直接采信、覆盖**（不再要求“比已记录的更远” `d > recD`）：
        //   以前只要之前记过一个更远的点（比如一座远处的敌方建筑），
        //   侦察兵死在敌营里的那个位置就会被丢掉，全军反攻会指向错的地方。
        //   仍然要求：还没拿到武器工程厂的准确位置（enemySiegeSN == -1），
        //   且死在离家 > HOME_DEFEND_RADIUS 的地方（在自家门口被流兵打死不算敌营）。
        double d = haveHome ? calDistance(scoutLastDR, scoutLastUR, homeDR, homeUR) : 0.0;
        if (enemySiegeSN == -1 && haveHome && d > HOME_DEFEND_RADIUS * bsl) {
            enemyFarDR = scoutLastDR;
            enemyFarUR = scoutLastUR;
            enemyFarFound = true;
        }
    }
}

// ---------- 前线集合点：村庄（市镇中心）外 RALLY_AWAY_DIST 格的一块空地 ----------
// 【用户 2026-09-21：“放弃 7*7 地块，到村庄 40 格外空地集合就好”】
// 【用户 2026-09-23：“集合点选的有问题，集合点周围有水域，下指令被判定不可达，
//   卡死。现在做中心点的要判断是否周围 7*7 可达，然后螺旋填充中间 5*5 位置”】
//   所以：
//     · **中心点必须满足“周围 7x7 全可站立”**（RALLY_CLEAR_HALF=3）——
//       block_is_standable 已经排除水域 / 水边一格 / 斜坡 / 建筑 / 资源 / 规划占位，
//       所以这块 7x7 是一整块干净空地，每个单位自己那格必定可站、也走得出去；
//     · 兵的落点只铺在**中间 5x5**（RALLY_HALF=2），从最中心那格起往外套圈。
// 为什么需要这个点：兵没凑够（composite < ASSAULT_BOWMAN_MIN）时要等兵营出人，
//   一等可能好几分钟。缩在家后方离前线太远（复合弓兵速度慢，兵凑齐那一刻要现
//   走一大段路）；放到敌营门口又是白送。所以在“家 → 敌营”方向上、离家
//   RALLY_AWAY_DIST(40) 格处找一块空地待命：既离前线近，又离敌营足够远。
//
// 把“第几个兵”（0 = 第一个）映射成格位偏移：
//   0 → 最中心的格，1..8 → 第一圈，9..24 → 第二圈（5x5 到这儿就满了）。
// 用户 2026-09：“从最中心点往外站”，所以是**由内向外螺旋填充**，每圈按顺时针取格。
// half = 铺开半径（5x5 → 2）。
static void rally_slot_offset(int slot, int half, int &di, int &dj)
{
    if (slot <= 0) { di = 0; dj = 0; return; }   // 第 0 个兵站正中心
    int idx = slot;
    for (int r = 1; r <= half; ++r) {
        const int cnt  = 8 * r;      // 第 r 圈有 8r 个格
        if (idx > cnt) { idx -= cnt; continue; }
        const int k    = idx - 1;    // 0 .. 8r-1
        const int side = 2 * r;      // 每边 2r 格
        if (k < side)          { di = -r + k;               dj = -r; }
        else if (k < side * 2) { di =  r;                    dj = -r + (k - side); }
        else if (k < side * 3) { di =  r - (k - side * 2);   dj =  r; }
        else                   { di = -r;                    dj =  r - (k - side * 3); }
        return;
    }
    di = 0; dj = 0;                  // 越界（兵比铺开格数还多时走不到这里）
}

// (dr,ur) 到最近的**可见**敌方投石车的距离（格）；没有可见投石车时返回很大的值。
// 【2026-09-23 为什么单独做这个】投石车射程 10 格（> 我们复合弓兵 7）、5 秒一发、
//   一发 50 点，而我们只有 45 血 —— **挨一发就死**。所以待命点/集合点绝不能
//   落在它的射程里（用户：“第一波肯定要在投石车以外打”）。
//   但反过来，它**不能进“驱动后撤”的威胁表** —— 见 demand_attack 风筝那段。
static double nearest_enemy_siege_dist(double dr, double ur)
{
    const double bsl = BLOCKSIDELENGTH;
    double best = 1e18;
    for (tagArmy &e : info.enemy_armies) {
        if (e.Sort != AT_STONE_THROWER) continue;
        double d = calDistance(dr, ur, e.DR, e.UR);
        if (d < best) best = d;
    }
    return best / bsl;   // 转成“格”
}

// 这个格子能不能当“走位落点”。
// 【用户 2026-09-23：“投石车是定点投射，我们很可能一个格子站两个人导致一起受伤”】
//   引擎里投石车的溅射半径只有 Missile_Boulders_Range = **0.5 格**，而且还会
//   减掉目标的碰撞半径 ⇒ **只有同一格上的两个兵会一起挨**，相邻格绝对安全。
//   所以规则很窄但很硬：**一个格子只能站一个兵**。
//   顺手把两个“下了也到不了”的坑一起堵了：
//     · 敌方建筑：block_is_standable 只查**我方**建筑 + 资源，把落点下在敌方
//       建筑上时 findPath 返回空 → 单位原地不动（就是那种“卡死”）；
//     · 已有己方单位站着的格（连农民一起算）：挤上去会把你推我我推你。
//  selfSN：**发起请求的那个单位自己**。他自己站在目标格上不算“被占”——
//   否则他已经到位时这里恒假，spread_slot 会去帮他找“另一格”再把他挪走，
//   形成“到位→被挪→再到位”的循环（在 info.armies 每帧被洗牌的情况下尤为明显）。
static bool landing_ok(int bx, int by, int selfSN)
{
    if (!block_is_standable(bx, by)) return false;
    for (tagBuilding &eb : info.enemy_buildings) {
        int s = building_size(eb.Type);
        if (bx >= eb.BlockDR && bx < eb.BlockDR + s &&
            by >= eb.BlockUR && by < eb.BlockUR + s) return false;
    }
    for (tagArmy &o : info.armies) {
        if (o.SN == selfSN) continue;
        if (o.BlockDR == bx && o.BlockUR == by) return false;
    }
    for (tagFarmer &f : info.farmers)
        if (f.BlockDR == bx && f.BlockUR == by) return false;
    // 本帧已经被别的单位预订了（否则同一帧里十几个人会抢同一格）
    if (cellClaim.count((bx << 12) | by)) return false;
    return true;
}




















// 朝家的方向退一步：返回 true = 本帧已经下了移动指令。
//   · 节流 unitStepFrame / RANGED_STEP_GAP：每帧重下会被内核 suspendRelation
//     + 清路径，单位在原地抖（这个坑踩过多次）。
//   · 落点过 landing_ok（能站 + 没被别人预订）；理想落点被同伴占着就在
//     附近 6 格内找一块空位，找不到就本帧不动。
static bool kite_retreat_home(tagArmy &a)
{
    double hDR = 0, hUR = 0;
    if (!home_center(hDR, hUR)) return false;
    const double bsl = BLOCKSIDELENGTH;
    // 方向必须是“自己 → 家”。以前写成 `自己 - 家`（= 朝敌人），撤退变成了往前冲。
    double dx = hDR - a.DR, dy = hUR - a.UR;
    const double len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6) return false;                 // 已经站在家中心：没地方退
    dx /= len; dy /= len;
    const int cx = a.BlockDR, cy = a.BlockUR;
    if (info.GameFrame - unitStepFrame[a.SN] < RANGED_STEP_GAP) return false;   // 位移节流

    const int tx = cx + (int)lround(dx * KITE_RETREAT_STEP);
    const int ty = cy + (int)lround(dy * KITE_RETREAT_STEP);
    int destX = tx, destY = ty;
    if (!landing_ok(destX, destY, a.SN)
        && !(find_free_spot_near(tx, ty, 1, 6, destX, destY)
             && landing_ok(destX, destY, a.SN)))
        return false;                               // 找不到空位：本帧不动
    unitStepFrame[a.SN] = info.GameFrame;
    cellClaim[(destX << 12) | destY] = a.SN;
    HumanMove(a.SN, (destX + 0.5) * bsl, (destY + 0.5) * bsl);
    return true;
}

// 弓箭手一步决策（“拉扯”，用户 2026-09-24 规则）：
//   · 判据是**到最近敌人的距离**（敌方没有农民，只数敌方军队）；
//   · 与“选中哪个目标”无关：目标会被 forbidTowerRange 过滤掉（追兵缩回自家
//     箭塔射程），只看目标的话弓兵正在挨打却“没有目标”，会继续往前走；
//   · < KITE_FLEE_DIST → 进入“脱离”，一直退到 ≥ 自己射程+3 才恢复攻击
//     （滞回必须有：否则退一步就重下攻击指令，内核把人走回来，净位移 0）。
// 返回 true = 本帧处理完了（调用方 continue）；false = 没目标也没在脱离，
//   交回调用方走“推进/走位”。
static bool kite_archer_step(tagArmy &a, int wantSN)
{
    double nd = 1e18;                     // 最近敌方军队的距离
    double td = 1e18;                     // 选中目标的距离
    for (tagArmy &e : info.enemy_armies) {
        const double dd = calDistance(a.DR, a.UR, e.DR, e.UR) / BLOCKSIDELENGTH;
        if (dd < nd) nd = dd;
        if (e.SN == wantSN) td = dd;
    }
    const double reach  = own_attack_range(a.Sort);
    const double resume = reach + 3.0;    // 退到这么远才恢复攻击

    // 下攻击指令的**唯一出口**：onlyIfInReach = 退不了时才要求“已在自己射程内”
    auto shoot = [&](bool onlyIfInReach) {
        if (wantSN < 0 || a.WorkObjectSN == wantSN) return;
        if (info.GameFrame - unitFireFrame[a.SN] < RANGED_FIRE_GAP) return;
        if (onlyIfInReach && td > reach) return;
        attackOrderSN[a.SN] = wantSN;
        HumanAction(a.SN, wantSN);
        unitFireFrame[a.SN] = info.GameFrame;
    };

    if (nd < KITE_FLEE_DIST) kiteRetreating[a.SN] = 1;

    // 脱离中：一直退到拉开距离为止
    if (kiteRetreating.count(a.SN)) {
        if (nd >= resume)              kiteRetreating.erase(a.SN);   // 拉开了：恢复
        else if (kite_retreat_home(a)) return true;                  // 退成功
        else                           shoot(true);                  // 退不了：够得着就打
        return true;                                                 // 反正**绝不往前冲**
    }
    // 没目标：身边还有敌人（< 自己射程+3）就**别跟着推进**（否则是往敌人怀里走）
    if (wantSN < 0) return nd < resume;
    shoot(false);                            // 有目标：交给内核走过去开火
    return true;
}

// 连通性 BFS 的共享状态（**建造选址、行军走位都用这一套**）。
static int reachStamp[505][505];      // 访问时间戳（避免每轮清 255KB）
static int reachCur = 0;
static std::vector<int> reachQueue;   // BFS 队列（x*505+y）

// 从 (sx,sy) 出发洪水填充。返回 false = 一步都走不出去（起点被完全困住，
// 或位图在这种地形上不可信），这时调用方应该关掉这个过滤。
// 可走判据 = rally_ground_ok（已探索 + 非海/水边 + 非建筑占地 + 非资源占地）。
//   资源/建筑是真正的障碍物，必须算进去。
// **4 邻域而不是 8**：单位占 1 格、只能上下左右走，两栋楼**只对角相接**时那条缝走不过去。
// 一次 BFS 覆盖整个连通分量，之后每个候选点判定都是 O(1)。
static bool reach_bfs(int sx, int sy)
{
    ++reachCur;
    if (reachCur <= 0) {                 // 时间戳回绕（几乎不可能）：整体清一次
        memset(reachStamp, 0, sizeof(reachStamp));
        reachCur = 1;
    }
    reachQueue.clear();
    reachQueue.push_back(sx * 505 + sy);
    reachStamp[sx][sy] = reachCur;       // 起点无条件算“可达”：他就站在这儿

    static const int dx[4] = { 1, -1, 0, 0 };
    static const int dy[4] = { 0, 0, 1, -1 };
    int head = 0;
    while (head < (int)reachQueue.size()) {
        const int v = reachQueue[head++];
        const int x = v / 505, y = v % 505;
        for (int k = 0; k < 4; ++k) {
            const int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= 505 || ny >= 505) continue;
            if (reachStamp[nx][ny] == reachCur) continue;
            if (!rally_ground_ok(nx, ny)) continue;
            reachStamp[nx][ny] = reachCur;
            reachQueue.push_back(nx * 505 + ny);
        }
    }
    return reachQueue.size() > 1;        // 一步都走不出去 ⇒ 这个起点的位图不可信
}

// 把一个“阵位中心”(cx,cy) 展开成 (2*half+1)^2 的格位矩阵，给 SN=sn 的单位分自己那一格。
// 格位算法与集合点完全一致（`rally_slot_offset` 的由内向外螺旋）。
// 返回 false = 连中心格都不能用（调用方本帧就别给它下指令）。
//
// 【走得到吗】除了 landing_ok（能站），还要过 `spreadReach*` 这道连通性检查：
//   landing_ok 只保证“那格站得住”，**不保证“从阵位中心走得到”** ——
//   隔一条河、被建筑围出的口袋地，下指令后内核 findPath 返回空，单位原地不动。
//   参考实现的行军就是先用 BFS 挑“可达且最靠近前进点”的格，这里用同一套。
//   同帧只做一次洪水填充（从阵位中心出发），整个循环共用。
static bool spread_cell_reachable(int bx, int by)
{
    if (bx < 0 || by < 0 || bx >= 505 || by >= 505) return false;
    return reachStamp[bx][by] == reachCur;
}

static bool spread_slot(int cx, int cy, int half, int sn, int &bx, int &by)
{
    // 本帧第一次调用时，从阵位中心做一次可达性洪水填充（同帧复用）
    static int spreadReachFrame = -1000000;
    static bool spreadReachReady = false;
    if (spreadReachFrame != info.GameFrame) {
        spreadReachFrame = info.GameFrame;
        spreadReachReady = reach_bfs(cx, cy);
    }

    int slot = 0;
    for (tagArmy &o : info.armies) {
        if (o.Sort == AT_PRIEST || o.Sort == AT_SCOUT) continue;
        if (o.SN < sn) slot++;
    }

    // ① 理想格位：找出能容纳这个 slot 的最小半径（不够就往外扩圈，见常量区）
    int h = half;
    while (h < half + SPREAD_EXTRA_RINGS
           && slot >= (2 * h + 1) * (2 * h + 1)) ++h;
    int di = 0, dj = 0;
    rally_slot_offset(slot, h, di, dj);
    if (landing_ok(cx + di, cy + dj, sn)
        && (spreadReachReady ? spread_cell_reachable(cx + di, cy + dj) : true)) {
        cellClaim[((cx + di) << 12) | (cy + dj)] = sn;
        bx = cx + di;
        by = cy + dj;
        return true;
    }

    // ② 理想格位用不了（有人站着 / 本帧被预订 / 是树/水/建筑 / 走不到）
    //    → 以中心为准一圈圈往外找第一个可用的格。
    //    **起点必须由 SN 决定**：引擎每帧都把 info.armies 洗牌
    //    （GlobalVariate.h:WLHHunYao 是 Fisher-Yates），任何“先到先得”
    //    的分配都会每帧变一次 → 单位在原地来回挪。
    for (int r = 0; r <= half + SPREAD_EXTRA_RINGS; ++r) {
        const int cnt      = (r == 0) ? 1 : 8 * r;
        // 第 r 圈的第一个 slot 序号（rally_slot_offset 是全局螺旋编号）
        const int ringBase = (r == 0) ? 0 : (1 + 4 * r * (r - 1));
        const int start    = (cnt > 1) ? (sn % cnt) : 0;
        for (int k = 0; k < cnt; ++k) {
            const int kk = (cnt > 1) ? ((start + k) % cnt) : 0;
            int e2i = 0, e2j = 0;
            rally_slot_offset(ringBase + kk, r, e2i, e2j);
            if (!landing_ok(cx + e2i, cy + e2j, sn)) continue;
            if (spreadReachReady && !spread_cell_reachable(cx + e2i, cy + e2j)) continue;
            cellClaim[((cx + e2i) << 12) | (cy + e2j)] = sn;
            bx = cx + e2i;
            by = cy + e2j;
            return true;
        }
    }
    return false;
}

// 【集结/待命点专用的“这一格能不能站”】
// 【用户 2026-09-23：“你的树根本不是在格子边缘，你的树在 7*7 范围内，
//   请问你是否遵从了以集结点为中心的 7*7 范围内都没有树！”】
//   排查结论：之前只做对了一件事，另外两件都是错的。
//   ① **原判据只排掉资源的左上角那一格**。block_is_standable 里是
//        for (tagResource &r : info.resources) if (r.BlockDR == i && r.BlockUR == j) ...
//      但资源是**有占地**的（Coordinate::BlockSizeLen）：
//        · 石头 / 金矿 / 渔场（StaticRes）= SIZELEN_SMALL(2) → **2x2**
//        · ANIMAL_FOREST                 = SIZELEN_SMALL(2) → **2x2**
//        · ANIMAL_TREE                   = SIZELEN_SINGEL(1) → 1x1
//      引擎那边（Map::loadBarrierMap → setBarrier(..., get_BlockSizeLen())）是按
//      **整块占地**设障碍的，我们只算了 1/4 —— 剩下 3 格被当成空地。
//      ⇒ 一块 2x2 的树林/石矿正好压在 7x7 里，校验照样通过。这是用户看到的“树在 7x7 里”。
//   ② **不是所有集结点都做了 7x7 校验**：rally_point()（第 1 阶段集合点）做了，
//      而 army_standby()（第 1/2 阶段的村外待命点）**只验了中心那一格**，
//      第三阶段的前线集结点更是**一格都没验**。
//   ③ MAPPATTERN_SHOAL / MAPPATTERN_DESERT **永远不会出现在 info.theMap 里**：
//      Core::updateCommon 的 GetTerrainType 只产出两种值 ——
//      MAPTYPE_OCEAN → MAPPATTERN_OCEAN，其它一律 → MAPPATTERN_GRASS；
//      未探索是 MAPPATTERN_UNKNOWN（InitPlayerMap）。所以“排掉浅滩”是**死代码**，
//      真正挡水的是 block_is_water_side 的 OCEAN 判据
//      （未探索格 height=-1，顺带也被它挡住）。
//
// 【为什么改成整图位图】原来每判一格都要遍历一遍 info.resources
//   （这张地图有上万个对象），而一次 7x7 校验要 49 格、一次 2D 选点要几百个候选
//   ⇒ 上千万次比较，一帧都算不完。现在“每个重算周期只走一遍地形 + 一遍建筑表
//   + 一遍资源表”，之后每格判断就是一次数组查询。
static unsigned char rallyOkMap[505][505];
static int rallyOkFrame = -1000000;

// 资源占几格（块）。**保守取大**：ANIMAL_TREE 与 ANIMAL_FOREST 在 info.resources 里
//   都报 RESOURCE_TREE，我们分不出来 ⇒ 按 2x2 算。多排除一格只是少一个候选，
//   漏排除一格就是部队站到树上（就是这次踩的坑）。
//   瞪羚/大象/狮子会走，不占格（返回 0），否则它们路过一下就把候选点废掉。
static int resource_footage(int type)
{
    switch (type) {
    case RESOURCE_STONE:
    case RESOURCE_GOLD:
    case RESOURCE_FISH:
    case RESOURCE_TREE:   return 2;
    case RESOURCE_BUSH:   return 1;
    default:              return 0;
    }
}

// 把 (bx,by) 起、边长 s 的方块在位图上清零（越界自动裁掉）。
// 注意只在 [0,w) x [0,h) 内清 —— 地图外的格子本来就是 0（不可站）。
static void rally_clear_box(unsigned char (&ok)[505][505], int bx, int by, int s, int w, int h)
{
    if (s < 1) return;
    for (int i = bx; i < bx + s; ++i) {
        if (i < 0 || i >= w) continue;
        for (int j = by; j < by + s; ++j) {
            if (j < 0 || j >= h) continue;
            ok[i][j] = 0;
        }
    }
}

// 重新生成位图：地形一遍 + 建筑一遍 + 资源一遍。
// 250ms 节流：地形/建筑/资源在这段时间里几乎不变
// （树林被砍掉最多晚 250ms 反映出来，代价只是“少排除一格”）。
static void ensure_rally_ok_map()
{
    const int toFrames = (TimePerFrame > 0) ? TimePerFrame : 40;
    if (rallyOkFrame >= 0 && info.GameFrame >= rallyOkFrame
        && info.GameFrame - rallyOkFrame < 250 / toFrames) return;
    rallyOkFrame = info.GameFrame;

    memset(rallyOkMap, 0, sizeof(rallyOkMap));
    if (info.theMap == nullptr) return;
    int w = (int)info.theMap->size();
    if (w <= 0) return;
    int h = (int)(*info.theMap)[0].size();
    if (w > 505) w = 505;
    if (h > 505) h = 505;

    // ① 地形：已探索（UNKNOWN 在这里被挡）+ 不是海/水边 + 没被我们规划的建造占位
    for (int i = 1; i < w - 1; ++i) {
        for (int j = 1; j < h - 1; ++j) {
            const int type = (*info.theMap)[i][j].type;
            if (type != MAPPATTERN_GRASS && type != MAPPATTERN_DESERT
                && type != MAPPATTERN_SHOAL) continue;
            if (MAP[i][j] != 0) continue;
            if (block_is_water_side(i, j)) continue;
            rallyOkMap[i][j] = 1;
        }
    }
    // ② 建筑：**整块占地**都不许站（自己的 + 敌人已探明的）
    for (tagBuilding &b : info.buildings)
        rally_clear_box(rallyOkMap, b.BlockDR, b.BlockUR, building_size(b.Type), w, h);
    for (tagBuilding &b : info.enemy_buildings)
        rally_clear_box(rallyOkMap, b.BlockDR, b.BlockUR, building_size(b.Type), w, h);
    // ③ 资源：按**完整足迹**排掉（见上面①的说明）
    for (tagResource &r : info.resources)
        rally_clear_box(rallyOkMap, r.BlockDR, r.BlockUR, resource_footage(r.Type), w, h);
}

// 集结/待命点专用的“这一格能不能站”：语义 = block_is_standable + 资源完整占地，
//   但不遍历 info.resources / info.buildings（都换成位图），因为一次选点要做上千次判定。
static bool rally_ground_ok(int bx, int by)
{
    if (bx < 0 || by < 0 || bx >= 505 || by >= 505) return false;
    ensure_rally_ok_map();
    return rallyOkMap[bx][by] != 0;
}

// 候选点的“周围开阔度”：在距离 4/5/6 格的三个环上取 8 个方向探一探，
//   数有多少个点是干净地面（树 / 水 / 建筑 / 资源占地都会让它掉分）。
// 【为什么要这个】只要求“7x7 内干净”是**不够**的 —— 7x7 紧贴着整片森林/水边也照样合格，
//   部队到齐后最外圈的人就站在林子里，再出发还得挤出来。
//   探针搬到 4~6 格外（远大于铺开半径 RALLY_HALF=2），才能把“紧贴着树林/水边”扣下来。
// 返回 0..24。
static int rally_openness(int cx, int cy)
{
    static const int dirX[8] = {  1,  1,  0, -1, -1, -1,  0,  1 };
    static const int dirY[8] = {  0,  1,  1,  1,  0, -1, -1, -1 };
    int open = 0;
    for (int k = 0; k < 8; ++k)
        for (int d = 4; d <= 6; ++d)
            if (rally_ground_ok(cx + dirX[k] * d, cy + dirY[k] * d)) ++open;
    return open;
}

// 以 (bx,by) 为中心、半径 half 的方块是否**全部是干净地面**（见 rally_ground_ok）。
// 【先验中心格】不通过就整块跳过 —— 虽然现在每次判定只是查一次位图，
//   但 49 次查表 + 越界判断在 2D 选点里会乘上几百个候选，便宜的剪枝还是值得。
static bool rally_patch_clear(int bx, int by, int half)
{
    if (!rally_ground_ok(bx, by)) return false;
    for (int di = -half; di <= half; di++)
        for (int dj = -half; dj <= half; dj++)
            if (!rally_ground_ok(bx + di, by + dj)) return false;
    return true;
}

// 在 (gx,gy) 附近找一个**中心**，使以它为中心、半径 half 的方块**全部**干净
//   （rally_patch_clear）。一圈一圈往外找（r = 0..maxR），
//   同一圈里取“周围开阔度”最高的那个；某圈已经足够开阔（≥ RALLY_OPEN_OK）就收。
// 【为什么要整块验】7x7 里只要有树/水/矿/建筑，部队就会：
//   要么全挤在中间那几格、要么下到“树里”的指令被引擎判不可达而原地不动（就是用户说的卡死）。
// 【为什么是“找”而不是“只验中心”】原来 army_standby / 前线集结点都只验中心那一格，
//   于是 7x7 里可以有树 —— 用户 2026-09-23 就是看到这个发火的。
static bool find_open_patch(int gx, int gy, int half, int maxR, int &bx, int &by)
{
    ensure_rally_ok_map();
    int bestX = -1, bestY = -1, bestOpen = -1;
    for (int r = 0; r <= maxR; ++r) {
        for (int di = -r; di <= r; ++di) {
            for (int dj = -r; dj <= r; ++dj) {
                // 只扫本圈那一层（r == 0 时就是中心那一格）
                if (r > 0 && di > -r && di < r && dj > -r && dj < r) continue;
                const int cx = gx + di, cy = gy + dj;
                if (!rally_patch_clear(cx, cy, half)) continue;
                const int open = rally_openness(cx, cy);
                if (open > bestOpen) { bestOpen = open; bestX = cx; bestY = cy; }
            }
        }
        if (bestX >= 0 && bestOpen >= RALLY_OPEN_OK) break;   // 本圈已经够开阔
    }
    if (bestX < 0) return false;
    bx = bestX;
    by = bestY;
    return true;
}

// 取（必要时重算）前线集合点中心；返回 false = 没有（调用方用老逻辑兜底）。
// 【为什么缓存】rally_patch_clear 一次要 49 次 block_is_standable，
//   而每个候选中心还要在附近横向试 3 个环半径 → 一次完整搜索上千次；
//   只在①首次 ②敌营位置明显变了（前半段 tx/ty 是 enemyFar 的粗略猜测）
//   ③ 老点已经站不住（被建筑/自己人占了）时才重算。
static bool rally_point(double hDR, double hUR, double eDR, double eUR)
{
    const double bsl = BLOCKSIDELENGTH;
    const int toFrames = (TimePerFrame > 0) ? TimePerFrame : 40;
    const bool moved = (calDistance(rallyAnchorDR, rallyAnchorUR, eDR, eUR) > 10 * bsl);
    // 没搜到时每 2 秒重试一次：迷雾只会散去不会回来，开头搜不到不代表以后搜不到。
    // （不能只靠"老点站不住"那个条件 —— rallyBX < 0 时它恒为假，会永远不再重试。）
    const bool retry = (rallyFrame >= 0) && (rallyBX < 0)
                       && (info.GameFrame - rallyFrame > 2000 / toFrames);
    const bool stale = (rallyFrame < 0) || moved || retry
                       || (rallyBX >= 0 && !rally_ground_ok(rallyBX, rallyBY));
    if (stale) {
        const int oldX = rallyBX, oldY = rallyBY;
        // 方向：家 → 敌营（沿这条线往外走 = “介于家和敌营之间”，但在家这一侧）
        double vx = eDR - hDR, vy = eUR - hUR;
        double len = sqrt(vx * vx + vy * vy);
        if (len < 1e-6) { vx = 1.0; vy = 0.0; len = 1.0; }
        const double ux = vx / len, uy = vy / len;
        const int hx = (int)(hDR / bsl), hy = (int)(hUR / bsl);
        const int ebx = (int)(eDR / bsl), eby = (int)(eUR / bsl);

        // ---- 候选点必须同时满足的 5 条（便宜的先查，最贵的 7x7 校验放最后）----
        // 【用户 2026-09-23：“状态一集结点的 7*7 范围不得与市中心 25 个建筑格重合。
        //   且集结点必须位于市中心与敌营间。注意，只要横坐标在之间且纵坐标在之间
        //   都算在之间”】
        // ① “在市中心与敌营之间” = **横、纵坐标都落在两个中心的区间内**（矩形，
        //    不是“在连线上”）。用户特别强调过这个口径。
        const int rectXMin = (hx < ebx) ? hx : ebx;
        const int rectXMax = (hx < ebx) ? ebx : hx;
        const int rectYMin = (hy < eby) ? hy : eby;
        const int rectYMax = (hy < eby) ? eby : hy;
        // ② 和“市中心周围那 25 个建筑格”一个都不许重合。
        //    那 25 格 = 5x5 网格（偏移 -2..+2、间距 BUILD_GRID_PITCH=4）、
        //    每格是 3x3 的建筑占地 ⇒ 外圈左上角在 中心 + (±2*4, ±2*4)，占地到 +2 格。
        //    这里是**逐格判 25 个 3x3 矩形**，而不是只判它们的外包围盒 ——
        //    网格之间还留着 1 格缝，按包围盒判会多排除掉一批其实合法的位置。
        int slotX[25], slotY[25];
        {
            int k = 0;
            for (int i = -2; i <= 2; ++i)
                for (int j = -2; j <= 2; ++j, ++k) {
                    slotX[k] = hx + i * BUILD_GRID_PITCH;
                    slotY[k] = hy + j * BUILD_GRID_PITCH;
                }
        }
        auto center_ok = [&](int cx, int cy) -> bool {
            // ① 必须在市中心与敌营之间（矩形判定）
            if (cx < rectXMin || cx > rectXMax || cy < rectYMin || cy > rectYMax)
                return false;
            // ② 它的 7x7 不许压到市中心那 25 个建筑格
            const int x0 = cx - RALLY_CLEAR_HALF, x1 = cx + RALLY_CLEAR_HALF;
            const int y0 = cy - RALLY_CLEAR_HALF, y1 = cy + RALLY_CLEAR_HALF;
            for (int k = 0; k < 25; ++k) {
                if (x1 >= slotX[k] && x0 <= slotX[k] + 2 &&
                    y1 >= slotY[k] && y0 <= slotY[k] + 2) return false;
            }
            // ③ 不许落在敌方箭塔射程（10 + 余量）里
            if (nearest_enemy_tower_dist(cx * bsl, cy * bsl)
                <= (double)(DIS_ARROWTOWER + ENEMY_DIS_ADD_TOWER) + TOWER_SAFE_MARGIN)
                return false;
            // ④ 不许落在可见敌方投石车射程里：
            //    它射程 10 格（> 我们复合弓兵 7）、5 秒一发 50 点，我们 45 血挨一发就死。
            //    用户：“第一波肯定要在投石车以外打” —— 连“在哪儿等”都得挑。
            if (nearest_enemy_siege_dist(cx * bsl, cy * bsl)
                <= (double)(DIS_STONE_THROWER) + TOWER_SAFE_MARGIN)
                return false;
            // ⑤ 中心周围 7x7 必须全可站立（最贵，放最后）
            return rally_patch_clear(cx, cy, RALLY_CLEAR_HALF);
        };

        // 起点 = 40 格；敌营太近的话收小（至少要留 RALLY_ENEMY_MARGIN 的余量）
        int r = RALLY_AWAY_DIST;
        const int maxR = (int)(len / bsl) - RALLY_ENEMY_MARGIN;
        if (r > maxR) r = (maxR < RALLY_DIST) ? RALLY_DIST : maxR;

        // 【2026-09-23 用户：“为什么你会选一个旁边有树有水的集结点”】
        //   原来是**沿“家 → 敌营”这一条线**前后挪（横向最多 ±3 格）、
        //   而且**第一个合格就收**。两个毛病：
        //     ① 只要线上那个点恰好紧贴着树林/水边，它就认了 —— 旁边 5 格外
        //        明明有整片空地也不去看（搜索空间根本不在那儿）；
        //     ② 合格判据只查了“7x7 里面”，**7x7 外面**是树是水它不管。
        //   现在改成：
        //     · 垂直连线向**两侧各 RALLY_SIDE_MAX(10) 格**铺开搜（真正的 2D 搜索，
        //       越界的点由 center_ok 的“必须在市中心与敌营之间”剔除）；
        //     · 用 rally_openness() 数**4~6 格外那三圈**有多少干净地面；
        //     · 按“开阔度档位 → 越靠前越好”取最优，而不是第一个碰上的。
        int px = -1, py = -1;
        int bestTier = -1, bestR = -1;
        for (int guard = 0; guard < 12 && r >= RALLY_DIST;
             ++guard, r -= ASSAULT_BACKSTEP) {
            for (int t = -RALLY_SIDE_MAX; t <= RALLY_SIDE_MAX; ++t) {
                // 垂直单位向量 = (-uy, ux)：沿它平移不改变“大致在连线中部”的位置
                const int ix = hx + (int)lround(ux * r - uy * t);
                const int iy = hy + (int)lround(uy * r + ux * t);
                if (!center_ok(ix, iy)) continue;      // 5 条硬过滤
                const int open = rally_openness(ix, iy);
                // 档位：0 = 勉强合格（周围被林/水包着）；1 = 还算开阔；2 = 很开阔
                const int tier = (open >= RALLY_OPEN_GOOD) ? 2
                               : (open >= RALLY_OPEN_OK)   ? 1 : 0;
                // 先比开阔度档位，同档取**更靠前**的（r 更大）
                if (tier > bestTier || (tier == bestTier && r > bestR)) {
                    bestTier = tier;
                    bestR    = r;
                    px = ix;
                    py = iy;
                }
            }
        }
        if (px >= 0 && (px != oldX || py != oldY)) {
            // 集合点挪了 → 让正在那儿待命的单位重下一次指令（否则它们会一直站在
            // 老坐标上：wantCode 恒为 -1，编码没变就不重下）。
            // **只清 -1 这一档**：攻击目标的记录不能动，重下会打断攻击蓄力。
            for (std::unordered_map<int,int>::iterator it = attackOrderSN.begin();
                 it != attackOrderSN.end(); ) {
                if (it->second == -1) it = attackOrderSN.erase(it);
                else ++it;
            }
        }
        rallyBX = px;
        rallyBY = py;
        rallyHalf = RALLY_HALF;
        rallyFrame    = info.GameFrame;
        rallyAnchorDR = eDR;
        rallyAnchorUR = eUR;
    }
    return (rallyBX >= 0);
}

// ---------- 军队待命点（第 1/2 阶段专用）：把部队从村里拉出去 ----------
// 【用户 2026-09-22：“兵种挡住农民的路了！”→ 澄清“第三波之前：军队就在村里/
//   生产建筑旁边不动，村民出不去”】
//   第 3 阶段有“前线集合点”把部队拉到村外 40 格；但第 3 波（14:00）之前
//   demand_attack 直接 return，**没有任何人指挥部队**，它们就停在生产建筑旁边
//   （靶场/马厩/兵营都在村子那一圈建筑带里）—— 村民必须从那儿挤出去，于是全堵了。
//   这里给它们一个村外待命点：从市镇中心朝**地图中心**方向走 ARMY_STANDBY_DIST 格。
//   为什么是“朝地图中心”：营地在地图角落、敌营在斜对面（map/map1~3 实测都是），
//   所以“背离最近的地图边”就是朝敌营方向 —— 和侦察兵 DFS 的初始方向同一个假设。
//   好处：① 出村后是开阔地，村民可以从两边绕；② 顺便朝着敌人，wave 来了少跑一段。
//   一格一兵用 rally_slot_offset 铺开（复用集合点那套），不堆成一坨。
void army_standby()
{
    if (phase >= 3) return;             // 第三阶段交给 demand_attack（它有前线集结点）
    if (bt_enemy_at_home()) return;     // 家里被打：交给 combat_tactic，别抢它的指令
    if (info.armies.empty()) return;

    const double bsl = BLOCKSIDELENGTH;
    double hx = 0, hy = 0;
    if (!home_center(hx, hy)) return;   // 市中心还没建成（异常）

    // 方向：市镇中心 → 地图中心
    const double mx = (MAP_L * 0.5) * bsl;
    const double my = (MAP_U * 0.5) * bsl;
    double dx = mx - hx, dy = my - hy;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6) { dx = 1.0; dy = 0.0; len = 1.0; }
    dx /= len;
    dy /= len;

    // 待命区中心：**整块 7x7 都必须干净**（见 find_open_patch）。
    // 【2026-09-23 用户：“你的树根本不是在格子边缘，你的树在 7*7 范围内”】
    //   这里原来只用 block_is_standable 验了**中心那一格** ——
    //   7x7 里可以有树/水/矿，部队于是挤在中间几格、或者走到树上被判不可达而原地不动。
    const int guessX = (int)(hx / bsl + dx * ARMY_STANDBY_DIST);
    const int guessY = (int)(hy / bsl + dy * ARMY_STANDBY_DIST);
    int bx = -1, by = -1;
    if (!find_open_patch(guessX, guessY, ARMY_STANDBY_HALF, ARMY_STANDBY_SEARCH_R, bx, by)) {
        // 附近实在找不到一块全干净的 7x7（村子外被建筑/树林塞满）→ 退回老办法：
        // 只要中心那一格能站就行。宁可挤一点，也不能让部队堵在村里不动
        // （那又把村民的出路堵死了，见 ARMY_STANDBY_DIST 那段注释）。
        if (block_is_standable(guessX, guessY)) { bx = guessX; by = guessY; }
        else if (find_free_spot_near(guessX, guessY, 1, ARMY_STANDBY_SEARCH_R, bx, by)) { }
    }
    if (bx < 0) return;

    // 那儿不安全（狮子 / 敌兵）就不过去 —— 复用采集那边同一个判据
    if (gather_spot_dangerous(bx * bsl, by * bsl)) return;

    const int half = ARMY_STANDBY_HALF;
    const double areaDR = (bx + 0.5) * bsl, areaUR = (by + 0.5) * bsl;

    // 【2026-09-23】本帧的“落点预订表”清空 —— 必须清，否则 phase<3 期间
    //   demand_attack 不跑（它才负责清），这张表会跨帧累积、把格位全占死。
    cellClaim.clear();

    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;  // 祭司守家、侦察兵探路
        if (a.NowState != HUMAN_STATE_IDLE) continue;             // 走路/交战都不打扰

        // 已经进到待命区里 → 不再下指令。
        //   每帧重下会被内核 suspendRelation + 清路径，单位就在原地抖
        //   （这个坑在集合点那儿已经踩过一次）。
        if (calDistance(a.DR, a.UR, areaDR, areaUR) <= half * bsl) continue;

        // 一格一兵：与集合点/推进线**共用同一套** spread_slot（同一个 bug 只修一处）。
        //   原来这里是手写的“slot = 比我小的战斗兵几个；slot >= cells 就取 cells-1”，
        //   和 spread_slot 原来那行一样会把第 50 个以后的人全塞进同一格 ——
        //   而第三阶段之前手里常有过 40 个兵（方阵兵/骑兵/弓兵），一样会卡死。
        int mx = -1, my = -1;
        if (!spread_slot(bx, by, half, a.SN, mx, my)) continue;   // 分不到：下帧再试
        // 已经站在自己那一格（或紧挨着）→ 不再重下
        const int ddx = a.BlockDR - mx, ddy = a.BlockUR - my;
        if (ddx * ddx + ddy * ddy <= 1) continue;
        HumanMove(a.SN, (mx + 0.5) * bsl, (my + 0.5) * bsl);
        attackOrderSN[a.SN] = -7;      // -7 = 去军队待命点（与其他编码区分开）
    }
}

void demand_attack()
{
    if (phase < 3) return;

    // 【2026-09-23】本帧的“落点预订表”清空（见 spread_slot / landing_ok）——
    //   必须在给任何单位下令**之前**清，否则会把上一帧的预订当真。
    cellClaim.clear();

    const double bsl = BLOCKSIDELENGTH;               // 1 格 = 多少细节坐标
    // 敌方箭塔射程：DIS_ARROWTOWER(7) + 谷仓升级/木材加工/工艺(+3) = 10 格（敌方科技全满）
    const double towerRange = (double)(DIS_ARROWTOWER + ENEMY_DIS_ADD_TOWER);
    const int toFrames = (TimePerFrame > 0) ? TimePerFrame : 40;

    // ---- 1) 敌方位置：在 record_enemy_positions() 里每帧无条件记录 ----
    // （bt_sync 最先调用；这里只负责读出来用，不再重复记录。）
    double homeDR = 0, homeUR = 0;
    const bool haveHome = home_center(homeDR, homeUR);

    const bool haveBase = (enemySiegeSN != -1);
    if (!haveBase && !enemyFarFound) return;   // 还没发现敌方目标：继续探图
    const double tx = haveBase ? enemySiegeDR : enemyFarDR;
    const double ty = haveBase ? enemySiegeUR : enemyFarUR;

    // ---- 2) 兵力统计（祭司、侦察兵都不算战斗兵）----
    int composite = 0, siegeCnt = 0, totalArmy = 0;
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
        totalArmy++;
        if (a.Sort == AT_COMPOSITE_BOWMAN) composite++;
        if (army_is_siege(a.Sort))  siegeCnt++;
    }
    if (totalArmy == 0) return;      // 兵已经打光：别下发空指令

    // 【2026-09-22】当前是第几分钟（“开打门槛”和下面的待命点选择都要用）
    const double gameMinNow = (double)info.GameFrame * toFrames / 60000.0;

    tagArmy *priest = nullptr;
    for (tagArmy &a : info.armies)
        if (a.Sort == AT_PRIEST) { priest = &a; break; }

    // 集结/推图途中家里被打 → 先交给 combat_tactic 守家（守住了再出门）。
    // 但兵力已经攒够（ATTACK_FORCE）时不再拖：反攻是唯一取胜手段，硬上限 30:00。
    if (assaultState < 2 && bt_enemy_at_home() && totalArmy < ATTACK_FORCE) return;

    // ---- 3) 集结点【分两级】----
    // 【用户 2026-09：怀疑是集结在敌人视野里，建议在不占用市中心附近的情况下
    //   在家附近空旷的地方集结】
    //   原来只有一个集结点（敌营外 26 格），于是一旦进了第三阶段而兵还没造够，
    //   部队就会先跑到**敌营门口**去干等 —— 那里有敌方野战军（24 格内）和箭塔，
    //   兵不够就是白送。现在拆成两级（见下面代码的 readyToAdvance 分支）。
    //   · 凑够了 → 才前出到敌营外的前线集结点（下面的 else 分支）：
    //     以敌营为圆心、朝我家的方向 ASSAULT_STAGE_DIST 格；若落在敌方箭塔射程内，
    // 就沿"离家方向"一步步往外退，直到退到射程 + TOWER_SAFE_MARGIN 之外。
    //   · 兵还没凑够（composite < ASSAULT_BOWMAN_MIN）→ 待命点按顺序取两个来源：
    //     ① **前线集合点**：家朝敌营方向 RALLY_AWAY_DIST(40) 格的一块空地
    //        （用户 2026-09-21：“到村庄 40 格外空地集合就好”）。离前线近、
    //        不占市中心周边，**一个格子站一个兵、从最中心那格起往外排**。
    //     ② 找不到（那段路还在迷雾里 / 全是水或密林）→ 退回老逻辑：
    //        防御锚点（箭塔，无塔则市中心）**背对敌营** RALLY_DIST 格。
    // 【2026-09-22】待命点必须跟“开打门槛”一致：一旦判定“要打了”就立刻切到
    //   **前线集结点**（敌营外 ASSAULT_STAGE_DIST 格），全军先走到那儿、
    //   集合完了（staged）才推进。
    //   原来这里只看 `composite >= 18`，而底线档可以在只有几个兵时就判定“够本开打”，
    //   两者不一致 ⇒ 部队还站在**家那边的待命点**（城中外 40 格）就被判开打，
    //   从 40 格外零散地冲过去、到几个被守军吃掉几个 ——
    //   就是用户问的“为什么没到时间没集合完毕就开始强攻”。
    //   另加 `assaultState >= 2`：已经开打了就不能因为伤亡掉到 18 个以下又往回缩。
    const bool readyToAdvance = (assaultState >= 2)
                                || (composite >= ASSAULT_BOWMAN_MIN)
                                || (gameMinNow >= ASSAULT_FORCE_MIN);
    double sx = tx, sy = ty;
    bool useRally = false;        // 本帧是否用上了“前线集合点”（决定第 6 节的落点算法）
    if (!readyToAdvance) {
        int cx = -1, cy = -1;
        // ① 前线集合点（村庄外 40 格）
        if (haveHome && rally_point(homeDR, homeUR, tx, ty)) {
            sx = (rallyBX + 0.5) * bsl;
            sy = (rallyBY + 0.5) * bsl;
            useRally = true;
        }
        // ② 兜底：**就站在自家防御锚点上等**（箭塔，没塔则市中心）。
        //   【用户 2026-09-24：“永远不要把集结点放在基地后方！！！！！！！！！”】
        //   原来这段是“从锚点沿**背离敌营**方向往外挑 RALLY_DIST 格的空地”
        //   = 把部队放到基地后面去 —— 既不能防守、开打时还要从后方绕一大圈。
        //   现在直接用锚点本身（在基地上/前），**绝不往后方放**。
        else if (get_defense_anchor(cx, cy)) {
            sx = cx * bsl;
            sy = cy * bsl;
        }
        // ③ 连锚点都没有（异常：市中心还没建成）→ 退回家中心；连家都没有就不下令
        else if (haveHome) {
            sx = homeDR;
            sy = homeUR;
        } else {
            sx = -1.0;
            sy = -1.0;      // 非法落点：spread_slot/landing_ok 会拒掉，本帧不下指令
        }
    } else {
        double dx = haveHome ? (homeDR - tx) : -1.0;
        double dy = haveHome ? (homeUR - ty) : 0.0;
        double len = sqrt(dx * dx + dy * dy);
        if (len < 1e-6) { dx = -1.0; dy = 0.0; len = 1.0; }
        dx /= len;
        dy /= len;
        sx = tx + dx * ASSAULT_STAGE_DIST * bsl;
        sy = ty + dy * ASSAULT_STAGE_DIST * bsl;
        for (int guard = 0; guard < 10; ++guard) {
            if (nearest_enemy_tower_dist(sx, sy)
                > towerRange + TOWER_SAFE_MARGIN) break;
            sx += dx * ASSAULT_BACKSTEP * bsl;
            sy += dy * ASSAULT_BACKSTEP * bsl;
        }
        // 【2026-09-23】前线集结点原来**一格都没验**（只有上面那条“别落进塔射程”）。
        //   用户看到部队站在树里/水边就是这里。现在在几何落点附近找一块
        //   **整块 7x7 都干净**的地；顺手把“别落进敌方塔射程”再复核一遍
        //   （新中心最多挪 ASSAULT_STAGE_SEARCH_R 格，所以要重新算一次距离）。
        //   找不到就保留几何落点 —— 宁可站得难受一点，也不能不出门。
        {
            const int gx = (int)(sx / bsl), gy = (int)(sy / bsl);
            int fx = gx, fy = gy;
            if (find_open_patch(gx, gy, RALLY_CLEAR_HALF, ASSAULT_STAGE_SEARCH_R, fx, fy)
                && nearest_enemy_tower_dist((fx + 0.5) * bsl, (fy + 0.5) * bsl)
                   > towerRange + TOWER_SAFE_MARGIN) {
                sx = (fx + 0.5) * bsl;
                sy = (fy + 0.5) * bsl;
            }
        }
    }
    // 【2026-09-22 用户反馈“兵力没集结到位就开打”】集结点换了地方（家侧待命点
    //   → 前线集结点）就**重置集结计时**。
    //   为什么必须重置：下面两条“超时”判定（够兵降档 2 分钟、集结超时 60 秒）
    //   读的都是 assaultStageFrame，而它是在**进入状态 1 的那一帧（≈14:00）**置的，
    //   之后再没动过。于是部队刚开拔到前线集结点，计时早就是“已超时”，
    //   6 个复合弓兵一到圈里就被判“集结超时 + 够本” → 全军压上，
    //   剩下 12 个还在半路（这就是“兵力没集结到位就开打”）。
    //   判定“换地方”用 10 格阈值：rally_point 每 2 秒会重算一次落点，
    //   同一块地的小抖动不该重置计时。
    {
        const bool stageMoved = (lastStageDR < 0
                                 || calDistance(lastStageDR, lastStageUR, sx, sy)
                                    > 10.0 * bsl);
        if (stageMoved) {
            lastStageDR = sx;
            lastStageUR = sy;
            if (assaultState <= 1) assaultStageFrame = info.GameFrame;
        }
    }
    stageDR = sx;
    stageUR = sy;

    // ---- 4) 战场态势 ----
    // 敌方野战军 = **已经贴到我方部队身上**的敌方军队。
    // 【2026-09 改成“拉锯诱杀”后去掉了一条】原来还包含“敌营 ASSAULT_FIELD_RADIUS(24)
    //   格内的所有敌人”，但那些敌人缩在厂区里 —— 只有我们进入警戒半径
    //   （enemyai.cpp 的 DEFENSE_ALERT_RANGE=20）才会出来，待命时根本碰不到。把它们算成“野战军”会让状态 2
    //   **永远清不完**（打不到的敌人也算数）。现在只算“咬上来的”，配合下面的诱杀循环。
    // 【2026-09-23 把半径从 ASSAULT_ENGAGE_DIST(20) 放宽到 26】
    //   原来用 20，而勾引线现在就在离厂 19 格 —— 守军追出来在 21~26 格之间徘徊时，
    //   它既没进我们单位的 20 格、也没真的回防，于是 `fieldUnits` 长期为 0，
    //   连续 40 秒后就误判“守军已清完” → 转状态 3 **顶着 5 座塔强攻**。
    //   26 仍然远小于“待命时根本碰不到”的距离（那是厂区深处、离我们的线 > 30）。
    const double fieldRadius = (ASSAULT_ENGAGE_DIST + 6) * bsl;
    auto threatensUs = [&](double dr, double ur) -> bool {
        for (tagArmy &a : info.armies) {
            if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
            if (calDistance(dr, ur, a.DR, a.UR) < fieldRadius) return true;
        }
        return false;
    };
    // 只数敌方军队（敌方没有农民）
    int fieldUnits = 0;
    for (tagArmy &e : info.enemy_armies)
        if (threatensUs(e.DR, e.UR)) fieldUnits++;

    // 敌营防御圈里还立着的敌方箭塔数量
    int towerCnt = 0;
    for (tagBuilding &eb : info.enemy_buildings) {
        if (eb.Type != BUILDING_ARROWTOWER || eb.Percent < 100) continue;
        if (calDistance(eb.BlockDR * bsl, eb.BlockUR * bsl, tx, ty)
            < ASSAULT_TOWER_RADIUS * bsl) towerCnt++;
    }

    // 敌方“厂区祭司猎手小队”还剩几个（见 enemy_hunter_count 的说明）
    const int enemyHunters = enemy_hunter_count();

    // ---- 5) 状态推进 ----
    if (assaultState < 1) {
        assaultState = 1;
        assaultStageFrame = info.GameFrame;
    }
    if (assaultState == 1) {
        // 集结：兵够 + 全队到位，才转推图
        // 【2026-09-22 定稿的开打门槛（两档）】
        //   ① 主力档：`composite >= ASSAULT_BOWMAN_MIN(18)` —— **不设时间上限**；
        //   ② 底线档：`ASSAULT_FORCE_MIN(23)` 分钟一到，**不管几个兵都开打**。
        //   原来那两条兜底（“在集结点等 2 分钟就降到 6 个”、“22 分钟用现有远程兵”）
        //   都已删除 —— 前者就是用户说的“等两分钟就开始攻太离谱了”。
        bool enough = (composite >= ASSAULT_BOWMAN_MIN)
                      || (gameMinNow >= ASSAULT_FORCE_MIN);
        int inRing = 0;                 // 已经站在集结圈里的战斗兵数量
        int outRing = 0;                // 还在外面赶路的
        for (tagArmy &a : info.armies) {
            if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
            if (calDistance(a.DR, a.UR, sx, sy) > ASSAULT_STAGE_RADIUS * bsl)
                outRing++;
            else
                inRing++;
        }
        // “就位” = 落后的人不超过 ASSAULT_STAGE_LAG_MAX 个。
        // 【为什么不要求全员到位】第三阶段兵营一直在造复合弓，新兵会不断从城里
        //   往外走，圈外永远有人 —— 按“全员到位”判就永远集不齐，每次都只能靠
        //   超时强推，那就等于没等（这就是用户看到的“没集结完就总攻”）。
        bool staged = (outRing <= ASSAULT_STAGE_LAG_MAX);
        // 集结超时：有人卡在路上/卡在墙角，别把 30 分钟耗光。
        // 【但必须够本才推】原来是无条件 `staged = true`，于是移速慢的复合弓兵还在
        //   半路时全军就开打了（用户 2026-09 反馈）。现在要求圈里至少已经站了
        //   ASSAULT_STAGE_MIN_READY 个人：落后的多半是刚出生、正在赶路的新兵，
        //   为它们无限等待不值得，但也不能一个没到就冲。
        // 【2026-09-22】两条同时满足才允许超时强推：圈里至少 MIN_READY 个人，
        //   **并且**掉队的不超过 ASSAULT_STAGE_STUCK_MAX 个（见常量区说明）。
        if (!staged && enough
            && info.GameFrame - assaultStageFrame
               > ASSAULT_STAGE_TIMEOUT_MS / toFrames
            && inRing >= ASSAULT_STAGE_MIN_READY
            && inRing + ASSAULT_STAGE_STUCK_MAX >= totalArmy)
            staged = true;
        if (enough && staged) {
            assaultState = 2;
            assaultStageFrame = info.GameFrame;
            baitPushing    = true;             // 开打先压上勾引（“进攻包括勾引”）
            baitPhaseFrame = info.GameFrame;
            baitHoldFrame  = 0;                // ★ 必须清：否则上一次留下的帧号会立刻触发翻转
        }
    } else if (assaultState == 2) {
        // ---- 状态 2：拉锯（**全军一起**压上 ↔ 一起退回诱杀线）----
        // 用户 2026-09-22 两句话合起来的要求：
        //   ① “开始进攻就一直进攻，不要有些兵在集结有些兵在进攻。我说的进攻包括勾引”
        //      → 压上勾引时**全军一起压**：没战斗目标的兵也跟着往前走，
        //        不再“一部分兵留在后面等、一部分兵冲上去”。
        //   ② “拉锯还是要拉啊，否则一次性拉太多兵会损伤很重”
        //      → 整队还要**一起退回诱杀线**把追兵甩掉（退到 20 格以外守军就抓不到目标）。
        //   所以“拉锯”是**全军一起做的两段循环**，而不是“一部分等、一部分冲”
        //     （具体线距见 BAIT_TRIGGER_DIST / ASSAULT_STAGE_DIST 的注释）。
        //   正在交战的单位不会被叫回头（逐单位下令里
        //   `NowState == HUMAN_STATE_ATTACKING → continue`）。
        if (fieldUnits > 0)
            assaultStageFrame = info.GameFrame;   // 有敌人上钩：重置“多久没敌人”计时
        else if (info.GameFrame - assaultStageFrame
                 > ASSAULT_BAIT_TIMEOUT_MS / toFrames) {
            assaultState = 3;                     // 一直没人出来 → 拆塔
            assaultStageFrame = info.GameFrame;
        }
        // 两段循环的翻转（到点就翻，不看有没有敌人）——
        //   **故意不做“有敌人就延长”**：敌人可以站在塔射程里不出来，
        //   我们既够不到（forbidTowerRange）、又不后退，会被塔一直打（死锁）。
        // 【2026-09-23】计时从“**全队到位**”那一刻起算（baitHoldFrame），
        //   而不是从翻转那一刻 —— 否则 8 秒里先花 ~2 秒走路，驻留只剩 6 秒。
        {
            const double nominal = baitPushing ? (double)BAIT_TRIGGER_DIST
                                               : (double)ASSAULT_STAGE_DIST;
            int nearCnt = 0, tot = 0;
            for (tagArmy &a : info.armies) {
                if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
                tot++;
                const double dep = calDistance(a.DR, a.UR, tx, ty) / bsl;
                if (dep >= nominal - 2.0 && dep <= nominal + 6.0) nearCnt++;
            }
            if (baitHoldFrame == 0 && tot > 0 && nearCnt * 2 >= tot)
                baitHoldFrame = info.GameFrame;          // 过半到位：开始计时

            const int hold = baitPushing ? ASSAULT_PUSH_HOLD_MS
                                         : ASSAULT_RETREAT_HOLD_MS;
            if (baitHoldFrame != 0 && info.GameFrame - baitHoldFrame > hold / toFrames) {
                baitPushing    = !baitPushing;
                baitPhaseFrame = info.GameFrame;
                baitHoldFrame  = 0;                      // 新的一段：等再次到位才开始计时
            }
        }
    } else if (assaultState == 3) {
        // 【用户 2026-09-24：“会莫名其妙在敌方大本营前集结一次”】
        //   根因：状态 3→2 的回退太灵 —— **一个**敌人贴上来就够 ⇒ 全军立刻
        //   从塔下走回勾引线、重新开始一轮拉锯（看着就是“又集结了一次”，
        //   还白挨一路的塔）。
        //   现在两道闸：① 要 ASSAULT_ROLLBACK_MIN(3) 个以上才算“冒出一队”；
        //   ② 刚进状态 3 的头 ASSAULT_TOWER_COMMIT_MS(15 秒) 内一律不回退。
        const bool committed = (info.GameFrame - assaultStageFrame
                                > ASSAULT_TOWER_COMMIT_MS / toFrames);
        if (fieldUnits >= ASSAULT_ROLLBACK_MIN && committed) {   // 真冒出一队 → 先打人
            assaultState = 2;
            assaultStageFrame = info.GameFrame;
            baitPushing    = true;             // 重新开始一轮拉锯：先压上
            baitPhaseFrame = info.GameFrame;
            baitHoldFrame  = 0;                // ★ 同上：别拿上一轮的帧号当本段起点
        } else if (towerCnt == 0) {
            // 塔清完了。但**祭司猎手**（骑兵/战车/战车射手）可能还躲在厂区里 ——
            // 它们专门猎杀距厂 20 格内的祭司（word 文档），所以先等部队把它们
            // 引出来打掉（用户 2026-09：“解决敌方祭司猎手后再到反攻区”）。
            // 等太久就放弃等待 —— 猎手可能一直不追出来，不转化就是输。
            const bool waited = (info.GameFrame - assaultStageFrame
                                 > ASSAULT_HUNTER_WAIT_MS / toFrames);
            if (enemyHunters == 0 || waited) {
                assaultState = 4;              // 猎手清完（或等到放弃）→ 祭司进场
                assaultStageFrame = info.GameFrame;
            }
        }
    } else if (assaultState == 4 && towerCnt > 0) {
        assaultState = 3;                      // 又看到塔（新探索到的）→ 回去拆
        assaultStageFrame = info.GameFrame;
    }

    // "不得进塔射程"只约束状态 1/2；状态 3/4 是"敌方兵力已清完"之后，放行。
    // 【2026-09-22 修正】“放行”**只对行军目标成立**（state>=2 时 advance=true，
    //   部队会一路压到勾引线），**选目标时状态 1/2 仍然一路守住塔射程** ——
    //   见下面 forbidTowerRange 的说明。
    // 【2026-09-22 用户反馈“没把敌方单位勾引出来，导致兵力损失严重”】
    //   原来诱敌期会把“不许打塔射程内的目标”这条**关掉**，
    //   于是压上勾引的时候，单位会顺手锁定缩在厂区里、站在箭塔火力圈里的守军，
    //   一头扎进 5 座箭塔的射程 —— 勾引没勾出来，自己先送进去。
    //   现在**状态 1/2 一律不许选塔射程内的目标**：只有真的追出来（离开塔射程）
    //   的守军才会被我们打，这才叫“勾引”。
    const bool forbidTowerRange = (assaultState <= 2);

    // ---- 6) 这一轮集火拆哪座箭塔（全队打同一座：拆得快、少挨打）----
    int focusTower = -1;
    if (towerCnt > 0) {
        double cDR = sx, cUR = sy;             // 全队质心
        double sumDR = 0, sumUR = 0;
        int n = 0;
        for (tagArmy &a : info.armies) {
            if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
            sumDR += a.DR;
            sumUR += a.UR;
            n++;
        }
        if (n > 0) { cDR = sumDR / n; cUR = sumUR / n; }
        double best = 1e18;
        for (tagBuilding &eb : info.enemy_buildings) {
            if (eb.Type != BUILDING_ARROWTOWER || eb.Percent < 100) continue;
            double d = calDistance(cDR, cUR, eb.BlockDR * bsl, eb.BlockUR * bsl);
            if (d < best) { best = d; focusTower = eb.SN; }
        }
    }

    // 【拆塔策略（用户 2026-09：“箭塔最后清完兵以后齐射解决”）】
    //   野战军清完之后**全军齐射**当前集火的那座箭塔，不再搞“近战第一排吸火力、
    //   弓兵第二排跟进、投石车独占”那套分批进场的配合 —— 此时敌方守军已经打光，
    //   塔只会打“正在攻击它的对象”，谁先上去都挨打，那就一起上、用最短时间把塔拆掉。
    //   （原来的 meleeEngaged / towerPushLate / siegeCnt>0 三分支已删除。）

    // ---- 7) 逐单位下令 ----
    // 【卡住兜底（用户 2026-09 反馈“兵种卡死”）】
    //   内核里只要关系是 `CoreEven_Attacking`（不管“正走过去打”还是“已经打起来了”）
    //   都报 `HUMAN_STATE_ATTACKING`，所以下面一律不打扰它们。但万一目标是走不到的
    //   地方（被建筑/水堵死、缩在围城里），单位就会永远停在半路、看超来就是“卡死”。
    //   这里每 3 秒采样一次位置，发现没挪窝就判断它究竟是不是在打：
    //     · 记录的是移动指令 → 直接清掉，重新寻路；
    //     · 记录的是攻击指令，但目标已经看不见了（死了/消失）→ 清掉；
    //     · 记录的是攻击指令，且离目标 > 12 格（远超任何兵种射程）→ 还没够着却不动，
    //       说明路上被卡住 → 清掉重新寻路。
    //   在射程内就地输出的情形（距离 ≤ 射程）不会被误伤，不会造成指令 churn。
    const int stuckSampleInterval = (3000 / toFrames) < 1 ? 1 : (3000 / toFrames);

    // ---- 本帧的勾引线 / 诱杀线 + 给远程兵的硬约束 noDeepenDist ----
    //   名义值：压上 BAIT_TRIGGER_DIST(19)、退回 ASSAULT_STAGE_DIST(24)。
    //   再沿“背离敌营”方向逐步往外退，直到不在任何**可见**敌方塔的射程内 ——
    //   塔是围绕厂区散布的（不是都在圆心），一座摆在离厂 12 格的塔能把 10 格射程
    //   抻到离厂 22 格，所以“离厂 19 格”并不等于安全。
    //   【缓存 + 滞回】这份计算依赖“可见的塔”，而迷雾里会不断发现新塔、
    //   敌方还会重建塔（enemyai.cpp 的 ifA）⇒ 每帧重算会让线**跳变**、
    //   全队落点跟着抖。所以只在①换段 ②敌营位置大变 ③当前线已不安全 时才重算。
    double lineDR = tx, lineUR = ty, noDeepenDist = 0.0;
    if (assaultState == 2 && haveHome) {
        const bool recalc = (baitLineDR < 0)
                            || (baitLinePushing != baitPushing)
                            || (calDistance(baitLineAnchorDR, baitLineAnchorUR, tx, ty)
                                > 5.0 * bsl)
                            || (nearest_enemy_tower_dist(baitLineDR, baitLineUR)
                                <= towerRange + TOWER_SAFE_MARGIN);
        if (recalc) {
            const double nominal = baitPushing ? (double)BAIT_TRIGGER_DIST
                                               : (double)ASSAULT_STAGE_DIST;
            double dx = homeDR - tx, dy = homeUR - ty;
            double len = sqrt(dx * dx + dy * dy);
            if (len < 1e-6) { dx = -1.0; dy = 0.0; len = 1.0; }
            dx /= len;
            dy /= len;
            double lx = tx + dx * nominal * bsl;
            double ly = ty + dy * nominal * bsl;
            for (int guard = 0; guard < 12; ++guard) {
                if (nearest_enemy_tower_dist(lx, ly)
                    > towerRange + TOWER_SAFE_MARGIN) break;
                lx += dx * ASSAULT_BACKSTEP * bsl;
                ly += dy * ASSAULT_BACKSTEP * bsl;
            }
            baitLineDR = lx;
            baitLineUR = ly;
            baitNoDeepen = calDistance(lx, ly, tx, ty) / bsl;
            baitLinePushing = baitPushing;
            baitLineAnchorDR = tx;
            baitLineAnchorUR = ty;
        }
        lineDR = baitLineDR;
        lineUR = baitLineUR;
        noDeepenDist = baitNoDeepen;
    }

    // 选敌人：就近；可选"不许在塔射程内"；可选"护祭司"（连正在咬祭司的远处敌人也打）
    // siegeFirst：状态 2/3 里把投石车排到最前
    const bool siegeFirst = (assaultState >= 2 && assaultState <= 3);
    // nearestOnly = 完全按距离取最近（**不给投石车加权**）——
    //   弓箭手用的是用户 2026-09-24 指定的“拉扯”规则，原话是“攻击**最近的**敌人”。
    auto pickEnemy = [&](tagArmy &a, bool forbidRange, bool protectPriest,
                         int maxDist, bool nearestOnly = false) -> int {
        int bestSN = -1;
        double best = 1e18;
        for (tagArmy &e : info.enemy_armies) {
            double d = calDistance(a.DR, a.UR, e.DR, e.UR);
            bool inReach = (d <= maxDist * bsl);
            if (!inReach && protectPriest && priest != nullptr)
                inReach = (calDistance(priest->DR, priest->UR, e.DR, e.UR)
                           <= ASSAULT_ENGAGE_DIST * bsl);
            if (!inReach) continue;
            if (forbidRange
                && point_in_enemy_tower_range(e.DR, e.UR, TOWER_SAFE_MARGIN))
                continue;
            // 【2026-09-23】“够不着”的目标不要选：勾引线约束下我们最深只能到
            //   noDeepenDist 格，而目标在 depth 处 —— 我们离它最少也有
            //   (noDeepenDist - depth) 格。大于射程就永远打不着，选了只会让这个兵
            //   站在线上空转（而且想靠近还会被勾引线夹住）。
            if (noDeepenDist > 0.0
                && noDeepenDist - calDistance(e.DR, e.UR, tx, ty) / bsl
                   > own_attack_range(a.Sort))
                continue;
            // 投石车优先：5 秒一发 50 点，而我们复合弓兵 45 血 ⇒ 挨一发就死。
            //   加成取 100 格（远大于 ASSAULT_ENGAGE_DIST）⇒ 在交战半径内就优先，
            //   但不影响“半径外不打”。只在状态 2/3 生效（状态 4 不能让队伍被拉走）。
            double sc = d;
            if (!nearestOnly && siegeFirst && e.Sort == AT_STONE_THROWER) sc -= 100.0 * bsl;
            if (sc < best) { best = sc; bestSN = e.SN; }
        }
        return bestSN;
    };

    // 放行“直接冲向敌营”的条件：诱敌超时（对方缩在塔下不出来）或已经进入拆塔/转化阶段。
    // 【用户 2026-09 反馈“反攻阶段除了祭司在动其他都没在动”】原来这两种情况下，
    //   没有攻击目标的部队只会 `HumanMove` 回集结点（它们本来就在那儿）—— 看上去
    //   全军干等、只有祭司在走。现在改成直接向敌营推进（"冲过去"）。
    // 【用户 2026-09-22】“开始进攻就一直进攻”：进了状态 2（推图 / 勾引）之后，
    //   没有攻击目标的单位一律**跟大部队走**（状态 2 走到勾引线或退回诱杀线、
    //   状态 3 冲敌营、状态 4 站保护位），**绝不回集结点待命** ——
    //   那正是“有些兵在集结、有些兵在进攻”。
    //   注：状态 2 的“往哪走”由 baitPushing 决定，具体落点见上面算好的 lineDR/lineUR。
    const bool advance = (assaultState >= 2);
    // 推进目标点：
    //   状态 2 → 用上面算好的 lineDR/lineUR（含与敌方箭塔射程的对账）。
    //     **绝不能冲敌营中心**：那里有 5 座箭塔（真实射程 10），进去就是送。
    //   状态 3 → 直接冲敌营（要拆塔，必须进去）；
    //   状态 4 → 停在敌营外 ASSAULT_PROTECT_DIST(8) 格，既护着祭司又不挡它转化的路
    //     （祭司转化建筑要求贴邻，被人挤住就走不过去了）。
    double advDR = tx, advUR = ty;
    if (assaultState >= 4 && haveHome) {
        double dx = homeDR - tx;
        double dy = homeUR - ty;
        double len = sqrt(dx * dx + dy * dy);
        if (len < 1e-6) { dx = -1.0; dy = 0.0; len = 1.0; }
        advDR = tx + dx / len * (ASSAULT_PROTECT_DIST * bsl);
        advUR = ty + dy / len * (ASSAULT_PROTECT_DIST * bsl);
    } else if (assaultState == 2 && haveHome) {
        advDR = lineDR;
        advUR = lineUR;
    }
    // 移动目标编码：-1 = 回集结点，-2 = 冲敌营，-4 = 回保护位，
    //                -6 = 压上勾引，-8 = 拉锯后撤（退回诱杀线）
    // （必须分开编号：否则状态切换时编码相同、内核“目标没变就不重下”会卡住）
    const int advCode = (assaultState >= 4) ? -4
                      : (assaultState == 3) ? -2
                      : (baitPushing ? -6 : -8);

    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) continue;
        if (a.Sort == AT_SCOUT) continue;   // 侦察骑兵只探路，不参加反攻
        // 【2026-09-23】demand_army 刚判了它自裁（腾人口给复合弓兵）：
        //   本帧不要给它下任何指令，否则会把自裁顶掉（见 weakKillSN 的说明）。
        if (a.SN == weakKillSN) continue;

        // ---- 卡住兜底（详见上面那段说明）----
        {
            std::unordered_map<int,int>::iterator itK = unitStuckKey.find(a.SN);
            std::unordered_map<int,int>::iterator itF = unitStuckFrame.find(a.SN);
            const int posKey = (a.BlockDR << 12) ^ a.BlockUR;
            if (itF == unitStuckFrame.end()
                || info.GameFrame - itF->second >= stuckSampleInterval) {
                if (itK != unitStuckKey.end() && itK->second == posKey) {
                    std::unordered_map<int,int>::iterator itO = attackOrderSN.find(a.SN);
                    const int code = (itO != attackOrderSN.end()) ? itO->second : -3;
                    // 本函数写进 attackOrderSN 的编码总共有：
                    //   >=0 攻击目标、-1 回集结点、-2 冲敌营、-4 回保护位、
                    //   -6 压上勾引、-7 去军队待命点（army_standby）、-8 拉锯后撤。
                    //   **每一个都必须在这里列出**，否则这个兵卡住时不会被重新寻路。
                    bool clearOrder = (code == -1 || code == -2 || code == -4
                                       || code == -6 || code == -7 || code == -8);
                    if (code >= 0) {
                        // 目标还在吗？离我们多远？
                        double d = -1.0;
                        for (tagArmy &e : info.enemy_armies)
                            if (e.SN == code) { d = calDistance(a.DR, a.UR, e.DR, e.UR); break; }
                        if (d < 0)
                            for (tagBuilding &e : info.enemy_buildings)
                                if (e.SN == code) {
                                    d = calDistance(a.DR, a.UR,
                                                    e.BlockDR * bsl, e.BlockUR * bsl);
                                    break;
                                }
                        if (d < 0)             clearOrder = true;   // 目标没了（死了/雾了）
                        else if (d > 12.0 * bsl) clearOrder = true; // 还没够着却不动 = 卡住
                    }
                    if (clearOrder) attackOrderSN.erase(a.SN);   // 下一帧重新下令（=重新寻路）
                }
                unitStuckKey[a.SN] = posKey;
                unitStuckFrame[a.SN] = info.GameFrame;
            }
        }

        // ---- 【2026-09-23 旧风筝机制已删除】
        //   原来这里是一整块“按**敌人射程**开窗 / 后撤”的逻辑（KITE_* 那套）：
        //   威胁是敌方弓兵 → 撤到 它射程+5+2 格；威胁是箭塔 → 撤到 17 格外。
        //   后果就是**我们永远站在自己射程之外**：一枪打不到、还被追着跑，
        //   也就是用户说的“勾引简直是拉了一坨大的 / 变成我们被拉扯”。
        //   现在这一整套也删掉了：位置管理交给用户 2026-09-24 指定的弓箭手“拉扯”规则。

        // 弓箭手（普通弓兵 / 复合弓兵）：用用户 2026-09-24 指定的“拉扯”规则
        //   ——“遍历所有敌人找最近的一个，距离 < 5 就往家跑，否则打最近的敌人”。
        //   所以① 选目标时**不给投石车加权**（nearestOnly），② 距离管理交给
        //   kite_archer_step。
        //   【定义必须放在下面那条 continue 之前，理由见它的注释】
        const bool archer = (a.Sort == AT_BOWMAN || a.Sort == AT_COMPOSITE_BOWMAN);

        // 【2026-09-23 关键修正】“正在交战：不打断”**不能把弓箭手一起挡掉**。
        //   内核事实（Core_List.cpp:2620 getNowPhaseNum）：关系只要是
        //   `CoreEven_Attacking` 就**一律**返回 HUMAN_STATE_ATTACKING ——
        //   **包括“正走过去打”的那一整段路**（走路是这条关系的一部分）。
        //   所以弓兵只要下过一次攻击指令，直到真的打到人之前都报 ATTACKING；
        //   旧代码在这里直接 continue ⇒ **kite_archer_step 一次都轮不到它**
        //   ⇒ 它一路走到敌人脸上站着对射（用户 2026-09-23：“拉扯根本不成功，
        //   一股脑冲上去全被杀了”）。现在让弓箭手自己判距离，其它兵种不变。
        if (a.NowState == HUMAN_STATE_ATTACKING && !archer) continue;

        int wantSN = -1;      // >=0 = 攻击目标；-1 = 没有可打的目标
        if (assaultState == 1) {
            // 集结途中：**只打贴脸（ASSAULT_STAGE_GUARD_DIST 格内）的敌人**，
            // 其余一律先去集结点站好。以前这里用 ASSAULT_ENGAGE_DIST(20) 格，
            // 于是先到的骑兵被“顺手打一下”拉走，集结永远集不齐（用户 2026-09 反馈）。
            wantSN = pickEnemy(a, true, false, ASSAULT_STAGE_GUARD_DIST, archer);
        } else if (assaultState == 2) {
            wantSN = pickEnemy(a, forbidTowerRange, false, ASSAULT_ENGAGE_DIST, archer);
        } else if (assaultState == 3) {
            // 状态 3 = 野战军已清完 → **全军齐射**当前集火的那座箭塔：
            // 先看这轮还有没有敌方单位（工厂可能又出新兵），有就先打人；
            // 没有就所有人一起打 focusTower（全队打同一座：集合火力、拆得快）。
            wantSN = pickEnemy(a, false, false, ASSAULT_ENGAGE_DIST, archer);
            if (wantSN == -1 && focusTower != -1)
                wantSN = focusTower;
        } else {
            // 状态 4：护着祭司，只打敌方单位（绝不碰武器工程厂）
            wantSN = pickEnemy(a, false, true, ASSAULT_ENGAGE_DIST, archer);
        }

        // ---- 弓箭手：距离管理全在 kite_archer_step 里（见函数头注释）----
        //   ※ 目标是建筑（状态 3 拆箭塔）时函数返回 false，落到下面的通用
        //     HumanAction —— 那里仍然没有对塔做位置管理（塔射程 10 > 我们 7）。
        if (archer && kite_archer_step(a, wantSN)) continue;

        // 没有攻击目标 → advance 时直接冲向敌营，否则回集结点（在塔射程之外）待命。
        // 集结点保留给状态 1（集结）和状态 2 的诱敌期：那两段就是不能进塔射程。
        const int wantCode = (wantSN >= 0) ? wantSN : (advance ? advCode : -1);

        int lastCode = -3;    // -3 = 这个单位还没有记录
        std::unordered_map<int,int>::iterator it = attackOrderSN.find(a.SN);
        if (it != attackOrderSN.end()) lastCode = it->second;

        // 只在"目标变了"或"单位空了（上一条已完成）"时重下：
        // 每帧重下会被内核 suspendRelation 掉关系，"走过去 → 攻击"的蓄力永远走不完。
        if (wantCode == lastCode && a.NowState != HUMAN_STATE_IDLE) continue;
        if (wantSN >= 0) {
            HumanAction(a.SN, wantSN);   // 打人/打塔：站位交给内核（它自己走到射程内开火）
        } else {
            // ---- 没有攻击目标 → 走位 ----
            // 【2026-09-23 用户：“投石车是定点投射，我们很可能一个格子站两个人导致一起受伤”】
            //   引擎里投石车溅射半径只有 Missile_Boulders_Range = **0.5 格**，
            //   还会减掉目标碰撞半径 ⇒ **只有同一格上的两个兵会一起挨**。
            //   而原来这里给**所有人下同一个坐标**（advDR/advUR 是单点、老逻辑的
            //   sx/sy 也是单点）⇒ 二十个兵往一个格子挤：既两人同格挨溅射，
            //   又互相碰撞谁也走不到位（看着像“卡死”）。
            //   现在四种走位（推进 / 勾引 / 后撤 / 回集结点）统一走 spread_slot：
            //   按 SN 分格位（与集合点同一套螺旋），落点还要过 landing_ok。
            int cx, cy, half;
            if (advance)       { cx = (int)(advDR / bsl); cy = (int)(advUR / bsl); half = ASSAULT_SPREAD_HALF; }
            else if (useRally) { cx = rallyBX;            cy = rallyBY;            half = rallyHalf; }
            else               { cx = (int)(sx / bsl);    cy = (int)(sy / bsl);    half = ASSAULT_SPREAD_HALF; }

            // 【用户 2026-09-24：“投石车太靠前，必须在弓箭手身后”】
            //   投石车（祭司转化来的）把站位中心整体往**背离敌营**方向挪
            //   SIEGE_BACK_DIST(6) 格：弓兵（射程 7）在前，投石车在 6 格之后
            //   （射程 10 照样够得着），追兵要打到它得先穿过弓兵线。
            //   状态 3 打塔时它自然也在后面（它停在 10 格外、弓兵要顶到 7 格）。
            if (army_is_siege(a.Sort) && haveHome) {
                double ux = homeDR - tx, uy = homeUR - ty;   // 敌营 → 家
                const double ul = sqrt(ux * ux + uy * uy);
                if (ul > 1e-6) {
                    cx += (int)lround(ux / ul * SIEGE_BACK_DIST);
                    cy += (int)lround(uy / ul * SIEGE_BACK_DIST);
                }
            }

            int mbx = -1, mby = -1;
            if (!spread_slot(cx, cy, half, a.SN, mbx, mby)) {
                // 分不到格位（中心格被占/站不住）→ 本帧不下指令，下一帧再试
                attackOrderSN[a.SN] = wantCode;
                continue;
            }
            // **到位就别再下移动指令了**（用户 2026-09-21）：
            //   · 已经站在自己那一格（或紧挨着）→ 算到位；
            //   · 或者已经进到阵区里（离中心 half 格以内）→ 也算到位。
            //   为什么不能只按“离自己那一格”判：几十个兵挤在一小块地上会互相
            //   碰撞、被挤离自己的格子，格子判就永远判“没到位”→ 每帧重下令
            //   （内核每次 addRelation 都 suspendRelation + 清路径）→ 原地抖。
            const int mdx = a.BlockDR - mbx, mdy = a.BlockUR - mby;
            const double dArea = calDistance(a.DR, a.UR,
                                             (cx + 0.5) * bsl, (cy + 0.5) * bsl);
            if (mdx * mdx + mdy * mdy > 1 && dArea > half * bsl)
                HumanMove(a.SN, (mbx + 0.5) * bsl, (mby + 0.5) * bsl);
        }
        attackOrderSN[a.SN] = wantCode;
    }

    // ---- 7.5) 每 5 秒报一次反攻状态（手动跑图调参用：不写日志的话，这套状态机
    //   "为什么没推图/卡在哪一步"基本靠猜）----
    if (info.GameFrame - assaultLogFrame >= 5000 / toFrames) {
        assaultLogFrame = info.GameFrame;
        int nIdle = 0, nWalk = 0, nAtk = 0, nWork = 0;
        int nStaged = 0, nTotal = 0, nMoved = 0;
        for (tagArmy &a : info.armies) {
            if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
            nTotal++;
            // 本帧刚做过战斗位移的单位数（替代原来的“风筝中”计数）
            std::unordered_map<int,int>::iterator itStep = unitStepFrame.find(a.SN);
            if (itStep != unitStepFrame.end() && itStep->second == info.GameFrame) nMoved++;
            if (calDistance(a.DR, a.UR, stageDR, stageUR)
                <= ASSAULT_STAGE_RADIUS * bsl) nStaged++;
            if (a.NowState == HUMAN_STATE_ATTACKING)      nAtk++;
            else if (a.NowState == HUMAN_STATE_WALKING)   nWalk++;
            else if (a.NowState == HUMAN_STATE_IDLE)      nIdle++;
            else                                          nWork++;
        }
        DebugText(std::string("反攻: 状态=") + std::to_string(assaultState)
                  + " 复合弓=" + std::to_string(composite)
                  + "/" + std::to_string(ASSAULT_BOWMAN_MIN)
                  + " 投石车=" + std::to_string(siegeCnt)
                  + " 野战军=" + std::to_string(fieldUnits)
                  + " 箭塔=" + std::to_string(towerCnt)
                  + " 猎手=" + std::to_string(enemyHunters)
                  + " 到位=" + std::to_string(nStaged)
                  + "/" + std::to_string(nTotal)
                  + " 集结点=(" + std::to_string((int)(stageDR / bsl))
                  + "," + std::to_string((int)(stageUR / bsl)) + ")"
                  + " 部队: 停=" + std::to_string(nIdle)
                  + " 走=" + std::to_string(nWalk)
                  + " 打=" + std::to_string(nAtk)
                  + " 忙=" + std::to_string(nWork)
                  + " 位移=" + std::to_string(nMoved));
    }

    // ---- 8) 祭司：**不参与反攻**，在家待命；塔和猎手都清完才上场转化 ----
    // 【用户 2026-09：“这样，祭司不参与反攻，解决敌方祭司猎手后再到反攻区”】
    //   原因（word 文档）：敌方守军里有**厂区祭司猎手小队 = 3 骑兵 + 2 战车射手**，
    //   专门猎杀距厂 20 格内的玩家祭司，直到杀死祭司或被消灭为止。
    //   祭司只有 45 血、速度 2.24，跟着部队压上去就是送人头，还会把猎手引出来
    //   拖慢整体推进。所以状态 1~3 一律让它守在**家里的防御锚点**（箭塔/市中心）
    //   旁边待命，顺便给伤兵回血（priest_heal 在 demand_scout 里）。
    if (priest == nullptr) return;
    if (assaultState < 4 || !haveBase) {
        // 家里被打了就交给 combat_tactic（本函数不跟它抢祭司）。
        if (bt_enemy_at_home()) return;
        recall_priest_home(priest);
        return;
    }

    if (bt_enemy_at_home()) return;   // 家里被打：祭司交给 combat_tactic

    // 转化：转换敌方建筑要求**贴邻**（Core_List.cpp 的 set_distance_AllowWork），
    // 内核会自己把祭司带过去，这里只要下 HumanAction 就行。
    // **只在目标变了或祭司空闲时重下**，每帧重下会把转化关系反复中止（转化要时间）。
    //
    // 卡住兜底：被墙/自己人堵住时内核可能一直走不过去 —— 2 秒没挪窝且不在转化
    // 动作中，就清掉记录，下一帧重新下令（等于让它重新寻路）。
    int stuckInterval = 2000 / toFrames;
    if (stuckInterval < 1) stuckInterval = 1;
    if (siegePriestStuckFrame == 0) {
        siegePriestStuckFrame = info.GameFrame;
        siegePriestStuckDR = priest->DR;
        siegePriestStuckUR = priest->UR;
    } else if (info.GameFrame - siegePriestStuckFrame >= stuckInterval) {
        double mDR = priest->DR - siegePriestStuckDR; if (mDR < 0) mDR = -mDR;
        double mUR = priest->UR - siegePriestStuckUR; if (mUR < 0) mUR = -mUR;
        if (mDR < 1.0 && mUR < 1.0 && priest->NowState != HUMAN_STATE_ATTACKING)
            attackConvertSN = -2;      // 强制下一帧重下
        siegePriestStuckFrame = info.GameFrame;
        siegePriestStuckDR = priest->DR;
        siegePriestStuckUR = priest->UR;
    }

    if (attackConvertSN != enemySiegeSN
        || priest->NowState == HUMAN_STATE_IDLE) {
        attackConvertSN = enemySiegeSN;
        HumanAction(priest->SN, enemySiegeSN);
    }
}

// 水域及其相邻一格都视为不可站立：
// 单位贴着水边寻路时容易卡住（岸边格常是斜坡或被判定为不可达），
// 因此选点时把"水域 + 岸边一格 + 斜坡"一并排除。
bool block_is_water_side(int x, int y)
{
    if (info.theMap == nullptr) return true;
    int w = (int)info.theMap->size();
    if (w <= 0) return true;
    int h = (int)(*info.theMap)[0].size();
    if (w > 505) w = 505;
    if (h > 505) h = 505;
    if (x < 0 || y < 0 || x >= w || y >= h) return true;   // 地图外按水处理

    // 自身是海洋，或是斜坡（height < 0）→ 不可站立
    if ((*info.theMap)[x][y].type == MAPPATTERN_OCEAN) return true;
    if ((*info.theMap)[x][y].height < 0) return true;

    // 水域边上一格也视为水域
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            if ((*info.theMap)[nx][ny].type == MAPPATTERN_OCEAN) return true;
        }
    }
    return false;
}

// 以 (cx,cy) 为圆心、半径 [r0,r1] 的环形范围内，找一个可站立的空块。
bool find_free_spot_near(int cx, int cy, int r0, int r1, int &bx, int &by)
{
    if (info.theMap == nullptr) return false;
    int w = (int)info.theMap->size();
    if (w <= 0) return false;
    int h = (int)(*info.theMap)[0].size();
    if (w > 505) w = 505;
    if (h > 505) h = 505;
    if (r0 < 0) r0 = 0;

    for (int r = r0; r <= r1; r++) {
        for (int i = cx - r; i <= cx + r; i++) {
            for (int j = cy - r; j <= cy + r; j++) {
                int di = (i > cx) ? (i - cx) : (cx - i);
                int dj = (j > cy) ? (j - cy) : (cy - j);
                if (di != r && dj != r) continue;          // 只扫本圈环上的点
                if (!block_is_standable(i, j)) continue;

                bx = i;
                by = j;
                return true;
            }
        }
    }
    return false;
}

// 单格是否可站立：排除水域/斜坡/水域边上一格、已被规划的建筑占位、
// 已有建筑与静态资源占用。不排除移动单位（它们会走开）。
bool block_is_standable(int i, int j)
{
    if (info.theMap == nullptr) return false;
    int w = (int)info.theMap->size();
    if (w <= 0) return false;
    int h = (int)(*info.theMap)[0].size();
    if (i < 1 || j < 1 || i >= w - 1 || j >= h - 1) return false;

    int type = (*info.theMap)[i][j].type;
    if (type != MAPPATTERN_GRASS && type != MAPPATTERN_DESERT
        && type != MAPPATTERN_SHOAL) return false;
    if (block_is_water_side(i, j)) return false;   // 水边/斜坡不站
    if (MAP[i][j] != 0) return false;              // 已被规划的建筑占位

    for (tagBuilding &b : info.buildings) {
        int bs = building_size(b.Type);
        if (i >= b.BlockDR && i < b.BlockDR + bs &&
            j >= b.BlockUR && j < b.BlockUR + bs) return false;
    }
    for (tagResource &r : info.resources)
        if (r.BlockDR == i && r.BlockUR == j) return false;

    return true;
}

// ---------- 祭司：环形广度优先 BFS（目的、参数、理由见上方 SCOUT_RING_* 常量区）----------
// 取下一个待访问的环上路点；选过的点记在 scoutSeen 里（侦察兵的 DFS 读的是引擎的
// 迷雾掩码，不用这张表）。
bool next_ring_point(int &bx, int &by)
{
    if (info.theMap == nullptr) return false;
    int w = (int)info.theMap->size();
    if (w <= 0) return false;
    int h = (int)(*info.theMap)[0].size();

    int cx = -1, cy = -1;
    for (tagBuilding &b : info.buildings) {
        if (b.Type == BUILDING_CENTER) {
            cx = b.BlockDR + 1;   // 营地中心格
            cy = b.BlockUR + 1;
            break;
        }
    }
    if (cx < 0) return false;

    const double TWO_PI = 6.283185307179586;
    int maxRing = SCOUT_RING_MAX;              // 环半径上限（再往外不是祭司的活）
    if (maxRing > w + h) maxRing = w + h;      // 小地图兜底
    if (ringRadius == 0) ringRadius = SCOUT_RING_START;

    for (int guard = 0; guard < 8192; guard++) {
        if (ringRadius > maxRing) return false;      // 该扫的扫完了

        int k = ringIndex++;

        // 本圈采样点数按弧长算：保证相邻路点间距 ≈SCOUT_ARC_SPACING 格。
        int nPts = (int)lround(TWO_PI * ringRadius / (double)SCOUT_ARC_SPACING);
        if (nPts < 6) nPts = 6;
        if (k >= nPts) { ringIndex = 0; ringRadius += SCOUT_RING_STEP; continue; }

        // 每圈起点错开一点角度，避免每圈都从同一方向开始、留下放射状空隙
        double ang = TWO_PI * (k / (double)nPts)
                     + (ringRadius / (double)SCOUT_RING_STEP) * 0.4;
        int i = cx + (int)lround(ringRadius * cos(ang));
        int j = cy + (int)lround(ringRadius * sin(ang));

        if (i < 1 || j < 1 || i >= w - 1 || j >= h - 1) continue;  // 图外
        if (scoutSeen[i][j]) continue;                             // 去过
        if (!block_is_standable(i, j)) continue;                   // 不可站立

        scoutSeen[i][j] = 1;
        bx = i;
        by = j;
        return true;
    }
    return false;
}

// ---------- 侦察骑兵：DFS（盯着"前沿格"，一路往深处扎）----------
// 取下一枝的落点。stuck=true 表示上次的目标走不到 —— **只把那一格拉黑**，
// 方向保留（走不到是“那一格到不了”，不代表方向错了）；
// **到目标角一带之后** stuck 还会把左右扫的方向翻面（“向两侧探”）。
// 返回 false = 附近完全没有可去的前沿格，调用方应让单位回村。
//
// 【探图策略（2026-09-22 用户要求重写）】
//   ① 离家还远（离**目标角** > SCOUT_GOAL_NEAR 格）→ 头方向对准目标角：
//      目标角 = 离我方市镇中心最远的地图角（营地在地图角落、敌营在斜对面）。
//   ② 到了那一带 → 头方向 = 垂直于“家 → 角”，先扫一侧，
//      那一侧走不通就翻面扫另一侧。
//
// 打分（分越高越优先）：
//     score = dot × SCOUT_DFS_HEAD_W          // dot = 目标方向与头方向的夹角余弦
//           + dist/(R) × SCOUT_DFS_FAR_W      // 同样顺路时优先更远的
//           − (1 − dotAway) × BACK_HOME       // 别往回（家）的方向跑
//           − SCOUT_DFS_BACK_W（dot < 0 时）  // 身后重罚：前方没路才允许回头
// R = SCOUT_DFS_RANGE 内找不到前沿 → 返回 false。
bool next_dfs_point(tagArmy *walker, bool stuck, int &bx, int &by)
{
    if (walker == nullptr || info.theMap == nullptr) return false;
    int w = (int)info.theMap->size();
    if (w <= 0) return false;
    int h = (int)(*info.theMap)[0].size();
    if (w > 505) w = 505;
    if (h > 505) h = 505;

    const double bsl = BLOCKSIDELENGTH;
    // 先把单位坐标取成普通 double：BLOCKSIDELENGTH 是引擎的 Double（定点类型），
    // 与 double 混算会触发重载歧义。
    const double wx = walker->DR, wy = walker->UR;
    const double cx = wx / bsl, cy = wy / bsl;

    // ---- 卡住：只把上次给出的目标拉黑一段时间 ----
    // 【用户 2026-09-21】原来这里连 scoutHead 一起清（“怕下一轮又挑中同一格”）——
    //   但同一格已经被 dfsBad 拉黑了，根本不会再被选中；清方向反而把“朝前”这项
    //   变成常数，让它掉头往身后的最远格跑。现在**方向保留**：
    //   同一方向上换一格继续试（这才是“只有一条路走不通才回头”）。
    if (stuck) {
        if (curTargetX >= 0) {
            // 顺手清掉过期条目（表一直很小）
            for (std::unordered_map<long long,int>::iterator it = dfsBad.begin();
                 it != dfsBad.end(); ) {
                if (it->second <= info.GameFrame) it = dfsBad.erase(it);
                else ++it;
            }
            long long key = ((long long)curTargetX << 20)
                          | (long long)(curTargetY & 0xFFFFF);
            dfsBad[key] = info.GameFrame + SCOUT_BAD_MS / TimePerFrame;
        }
        // 【用户 2026-09-22】到角之后是“左右两侧扫”：这一侧的那格走不到
        //   （已经被拉黑）→ 翻到另一侧。在外奔目标角的路上翻面无影响
        //   （头方向由目标角决定，不看这个符号）。
        scoutSideSign = -scoutSideSign;
    }

    // "离家方向"（同时把市中心块坐标记下来，下面挑目标角要用）
    double awayDR = 0, awayUR = 0, awayLen = 0;
    int hbx = -1, hby = -1;
    for (tagBuilding &b : info.buildings) {
        if (b.Type != BUILDING_CENTER) continue;
        hbx = b.BlockDR;
        hby = b.BlockUR;
        double hx = b.BlockDR * bsl, hy = b.BlockUR * bsl;
        double adr = wx - hx, aur = wy - hy;
        awayLen = sqrt(adr * adr + aur * aur);
        if (awayLen > 1e-6) { awayDR = adr / awayLen; awayUR = aur / awayLen; }
        break;
    }

    // 【用户 2026-09-22 改探图策略：“朝我方大本营最远的角探路，然后再向两侧探”】
    //   目标角 = **离我方市镇中心最远的地图角**。营地在地图角落、敌营在斜对面
    //   （map/map1~3 实测都是），所以那个角就是敌营所在的那一带。
    //   为什么必须改成“瞄一个固定角”：原来的头方向是“离家方向”＋每步用
    //   选中的格子更新，走着走着会因为前沿格的位置偏掉、拐到**不是敌营的那个角**，
    //   然后再也回不来 —— 这就是“没把敌方大本营探出来”。
    //   4 个角算一次只要几次平方距离，不用缓存（缓存反而会在换地图时变成脏数据）。
    int goalBX = -1, goalBY = -1;
    if (hbx >= 0) {
        const int gcx[4] = { 1, w - 2, 1, w - 2 };
        const int gcy[4] = { 1, 1, h - 2, h - 2 };
        double bestD = -1.0;
        for (int k = 0; k < 4; k++) {
            double dx = (double)(gcx[k] - hbx), dy = (double)(gcy[k] - hby);
            double d = dx * dx + dy * dy;
            if (d > bestD) { bestD = d; goalBX = gcx[k]; goalBY = gcy[k]; }
        }
    }

    // ---- 头方向：每帧根据“到没到目标角”现算 ----
    const double gwx = (goalBX + 0.5) * bsl, gwy = (goalBY + 0.5) * bsl;
    const double gdx = gwx - wx, gdy = gwy - wy;
    const double goalDist = (goalBX >= 0) ? sqrt(gdx * gdx + gdy * gdy) : 0.0;
    if (goalBX >= 0) {
        if (goalDist > SCOUT_GOAL_NEAR * bsl) {
            // ① 还没到那一带：头方向对准目标角（“朝最远的角探路”）
            scoutHeadDR = gdx / goalDist;
            scoutHeadUR = gdy / goalDist;
        } else {
            // ② 到了那一带：朝**垂直于“家 → 角”**的方向左右扫
            //    （awayDR/UR 就是“家 → 角”方向；营地在地图角落时两者同向）。
            //    这一侧的目标走不到（被拉黑）时会在上面 stuck 分支里翻到另一侧。
            double ax = 1.0, ay = 0.0;
            if (awayLen > 1e-6) { ax = awayDR; ay = awayUR; }
            scoutHeadDR = -ay * (double)scoutSideSign;
            scoutHeadUR =  ax * (double)scoutSideSign;
        }
    } else if (scoutHeadDR == 0.0 && scoutHeadUR == 0.0 && awayLen > 1e-6) {
        // 还没建市中心（异常）→ 退回老逻辑：拿“离家方向”当初始方向
        scoutHeadDR = awayDR;
        scoutHeadUR = awayUR;
    }
    const bool haveHead = (scoutHeadDR != 0.0 || scoutHeadUR != 0.0);

    const int R = SCOUT_DFS_RANGE;
    const int cxi = (int)cx, cyi = (int)cy;
    double bestScore = -1e18;
    int bestX = -1, bestY = -1;

    for (int i = cxi - R; i <= cxi + R; i++) {
        if (i < 1 || i >= w - 1) continue;
        for (int j = cyi - R; j <= cyi + R; j++) {
            if (j < 1 || j >= h - 1) continue;

            // 先做便宜的筛选：太近的不要（避免原地抖），超范围的不要
            double dxg = (double)i - cx, dyg = (double)j - cy;
            double d2g = dxg * dxg + dyg * dyg;
            if (d2g < (double)SCOUT_DFS_MIN * SCOUT_DFS_MIN) continue;
            if (d2g > (double)R * (double)R) continue;

            // 必须是**前沿格**：邻域里存在未探索格（走到它就能把迷雾往前推一格）
            bool frontier = false;
            for (int di = -1; di <= 1 && !frontier; di++) {
                for (int dj = -1; dj <= 1; dj++) {
                    int ni = i + di, nj = j + dj;
                    if (ni < 0 || nj < 0 || ni >= w || nj >= h) { frontier = true; break; }
                    if ((*info.theMap)[ni][nj].type == MAPPATTERN_UNKNOWN) {
                        frontier = true; break;
                    }
                }
            }
            if (!frontier) continue;

            // 已知可站立（内部已排除水 / 水边 / 建筑 / 资源 / 规划中的建筑占位）
            if (!block_is_standable(i, j)) continue;

            // 拉黑：最近走不到过的目标点不再选
            long long key = ((long long)i << 20) | (long long)(j & 0xFFFFF);
            std::unordered_map<long long,int>::iterator bi = dfsBad.find(key);
            if (bi != dfsBad.end() && bi->second > info.GameFrame) continue;

            double tx = (i + 0.5) * bsl, ty = (j + 0.5) * bsl;
            double dx = tx - wx, dy = ty - wy;
            double dist = sqrt(dx * dx + dy * dy);
            if (dist < 1e-6) continue;

            double dot = haveHead ? (dx * scoutHeadDR + dy * scoutHeadUR) / dist : 1.0;
            double score = dot * SCOUT_DFS_HEAD_W
                         + (dist / ((double)R * bsl)) * SCOUT_DFS_FAR_W;
            if (awayLen > 1e-6) {
                double dota = (dx * awayDR + dy * awayUR) / dist;
                score -= (1.0 - dota) * SCOUT_BACK_HOME_PENALTY;
            }
            // 【身后重罚】只要正前方还有任一前沿格，它就必须赢过身后的所有候选；
            //   于是“回头”只会在前方真的没路可走时发生（用户 2026-09-21）。
            if (haveHead && dot < 0.0) score -= SCOUT_DFS_BACK_W;

            if (score > bestScore) {
                bestScore = score;
                bestX = i;
                bestY = j;
            }
        }
    }

    if (bestX < 0) return false;      // 附近已经没有可去的前沿格了

    // 记下目标（卡住时用来拉黑）。
    // 【2026-09-22】**不再用选中的格子反推头方向** —— 头方向现在由“目标角”
    //   每帧现算（见上面的 ①②），这里写回去下一帧也会被覆盖。
    curTargetX = bestX;
    curTargetY = bestY;
    bx = bestX;
    by = bestY;
    return true;
}

// 取"防御锚点"块坐标：优先己方已建成的箭塔（祭司躲到塔下才有掩护），
// 没有箭塔时退回市镇中心。找不到返回 false。
bool get_defense_anchor(int &cx, int &cy)
{
    for (tagBuilding &b : info.buildings) {
        if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
        cx = b.BlockDR;
        cy = b.BlockUR;
        return true;
    }
    for (tagBuilding &b : info.buildings) {
        if (b.Type != BUILDING_CENTER) continue;
        cx = b.BlockDR + 1;   // 中心建筑几何中心格
        cy = b.BlockUR + 1;
        return true;
    }
    return false;
}

// 在"防御锚点"附近找一个可站立空块作为落脚点。
// 锚点优先取己方**箭塔**：祭司躲到塔下，敌兵打它时会被箭塔射击，才有人掩护；
// 没有箭塔时退回市镇中心。attempt 越大搜索半径越向外扩，用于卡住后换点重试。
bool find_home_spot(int &bx, int &by, int attempt)
{
    int cx = -1, cy = -1;
    if (!get_defense_anchor(cx, cy)) return false;

    int r0 = 2 + attempt * 3;        // 先在锚点紧邻处找，卡住再向外扩
    return find_free_spot_near(cx, cy, r0, r0 + 3, bx, by);
}

// 探图途中遇到敌人的处置：**不是回村**，而是朝"背离附近所有敌人"的方向撤离，
// 拉开距离后继续探图。撤离指令下得勤一些（500ms），别让慢速单位追上来。
void scout_retreat(tagArmy *priest)
{
    if (priest == nullptr) return;

    // 方向 = 各附近敌人"指向祭司"的单位向量之和（即背离敌人的合成方向）
    double awayDR = 0, awayUR = 0;
    auto accumulate = [&](double edr, double eur) {
        double dx = priest->DR - edr;
        double dy = priest->UR - eur;
        double d = sqrt(dx * dx + dy * dy);
        if (d > SCOUT_THREAT_RADIUS * BLOCKSIDELENGTH) return;
        if (d < 1e-6) { dx = 1; dy = 0; d = 1; }
        awayDR += dx / d;
        awayUR += dy / d;
    };
    for (tagArmy &e : info.enemy_armies) accumulate(e.DR, e.UR);

    double len = sqrt(awayDR * awayDR + awayUR * awayUR);
    if (len < 1e-6) return;
    awayDR /= len;
    awayUR /= len;

    // 撤离方向就是新的"行进方向"：脱离接触后不会马上掉头撞回去
    scoutHeadDR = awayDR;
    scoutHeadUR = awayUR;

    // 撤离指令下得勤一些（500ms），别让慢速单位追上来；
    // 节流判断放在找落点之前，避免每帧都做一次环形搜索。
    int gap = 500 / TimePerFrame;
    if (gap < 1) gap = 1;
    // 节流用"单位自己的"帧号：侦察骑兵和祭司各走各的，混用会互相拖慢
    int &ordFrame = (priest->Sort == AT_SCOUT) ? scoutUnitOrderFrame : priestOrderFrame;
    if (ordFrame != 0 && info.GameFrame - ordFrame < gap) return;
    ordFrame = info.GameFrame;

    // 落点：背离方向 SCOUT_FLEE_STRIDE 格处，尽量落在一个可站立的空块上
    double gx = priest->DR + awayDR * SCOUT_FLEE_STRIDE * BLOCKSIDELENGTH;
    double gy = priest->UR + awayUR * SCOUT_FLEE_STRIDE * BLOCKSIDELENGTH;
    int gbx = (int)(gx / BLOCKSIDELENGTH);
    int gby = (int)(gy / BLOCKSIDELENGTH);

    int bx = -1, by = -1;
    if (find_free_spot_near(gbx, gby, 0, 6, bx, by)) {
        gx = (bx + 0.5) * BLOCKSIDELENGTH;
        gy = (by + 0.5) * BLOCKSIDELENGTH;
    }

    HumanMove(priest->SN, gx, gy);
}

// 派祭司回村（躲到箭塔下）。要点：
//   1) 目的地取"箭塔（没有塔则市中心）附近的可站立空块"，而不是建筑自己占的块
//      （后者是建筑，必然不可达，单位会贴到旁边卡住）；
//   2) 每 2 秒最多重下一次指令，避免每帧刷同一道命令；
//   3) 1 秒内没有位移（卡住）就换个更靠外的落脚点；多次都回不去则放弃下令，
//      让祭司原地待命，免得把 AI 卡死在这一步。
void recall_priest_home(tagArmy *priest)
{
    if (priest == nullptr) return;

    // 回村会改变行进方向，清掉探索方向，避免恢复探图后把"该去的方向"误判为回头
    scoutHeadDR = 0;
    scoutHeadUR = 0;

    // 已经守在防御锚点（箭塔/市中心）旁边：不再下任何指令，
    // 否则会在塔边反复换落脚点、来回徘徊。
    {
        int ax = -1, ay = -1;
        if (get_defense_anchor(ax, ay)
            && calDistance(priest->DR, priest->UR,
                           ax * BLOCKSIDELENGTH, ay * BLOCKSIDELENGTH)
               <= HOME_STAY_RADIUS * BLOCKSIDELENGTH) {
            homeSpotX = -1;
            homeSpotY = -1;
            homeSpotTry = 0;
            return;
        }
    }

    // 卡住检测（与探图共用采样变量）
    int interval = 1000 / TimePerFrame;
    if (interval < 1) interval = 1;
    bool stuck = false;
    if (scoutCheckFrame == 0) {
        scoutCheckFrame = info.GameFrame;
        scoutCheckDR = priest->DR;
        scoutCheckUR = priest->UR;
    } else if (info.GameFrame - scoutCheckFrame >= interval) {
        double mDR = priest->DR - scoutCheckDR; if (mDR < 0) mDR = -mDR;
        double mUR = priest->UR - scoutCheckUR; if (mUR < 0) mUR = -mUR;
        if (mDR < 1.0 && mUR < 1.0) stuck = true;
        scoutCheckFrame = info.GameFrame;
        scoutCheckDR = priest->DR;
        scoutCheckUR = priest->UR;
    }

    if (stuck) {                 // 卡住：向外换个落脚点
        homeSpotTry++;
        homeSpotX = -1;
        homeSpotY = -1;
    }
    if (homeSpotTry > 6) return; // 多次都回不去：放弃下令，原地待命

    if (homeSpotX < 0) {
        int bx = -1, by = -1;
        if (find_home_spot(bx, by, homeSpotTry)) {
            homeSpotX = bx;
            homeSpotY = by;
        }
    }
    if (homeSpotX < 0) return;

    double tx = (homeSpotX + 0.5) * BLOCKSIDELENGTH;   // 用块中心，避免贴角卡住
    double ty = (homeSpotY + 0.5) * BLOCKSIDELENGTH;

    if (calDistance(priest->DR, priest->UR, tx, ty) < 4 * BLOCKSIDELENGTH) {
        homeSpotTry = 0;             // 已到家门口：清状态，不再下令
        homeSpotX = -1;
        homeSpotY = -1;
        return;
    }

    int reissue = 2000 / TimePerFrame;   // 2 秒
    if (reissue < 1) reissue = 1;
    if (priestOrderFrame != 0 && info.GameFrame - priestOrderFrame < reissue) return;
    priestOrderFrame = info.GameFrame;
    HumanMove(priest->SN, tx, ty);
}

// ---------- 祭司空余时间治疗伤兵 ----------
// 返回 true 表示"本帧已接管祭司"（调用方不要再下别的指令）。
// 内核里祭司对同阵营目标执行 HumanAction 会走治疗分支（见 Core_List::object_Attack：
// 同阵营 → 治疗，异阵营 → 转化），所以这里直接给伤兵下 HumanAction，
// 祭司会自己走过去并持续回血，不需要我们管中间过程。
bool priest_heal(tagArmy *priest)
{
    if (priest == nullptr) { healTargetSN = -1; return false; }
    // 过了治疗窗口、或家里有敌袭：清掉状态交回给战斗逻辑
    if (info.GameFrame > (int)(PRIEST_HEAL_UNTIL_MIN * 60 * 1000.0 / TimePerFrame)
        || bt_enemy_at_home()) {
        healTargetSN = -1;
        return false;
    }

    const double bsl = BLOCKSIDELENGTH;
    const double maxDist = HEAL_MAX_DIST * bsl;

    // ---- 选目标：**离祭司最近的伤兵**，选中就一路治到满血 ----
    // 【用户 2026-09：“不要优先治疗血量最低的，不然会反复横跳，要治疗离自己最近的
    //   且持续治疗”】原来按“血最少”挑，而“血最少的那个人”会随着每一跳变化，
    //   于是祭司在两个伤兵之间来回走、谁也治不满。现在：
    //     ① 手上这个还值得治（还在 / 没满血 / 没跑出半径）→ **继续治它，绝不换人**；
    //     ② 治好了或目标没了 → 才重新挑，挑**最近的**。
    int target = -1;
    if (healTargetSN >= 0) {
        for (tagArmy &a : info.armies) {
            if (a.SN != healTargetSN) continue;
            if (a.Sort != AT_PRIEST && a.Sort != AT_SCOUT
                && a.MaxBlood > 0 && a.Blood < a.MaxBlood
                && calDistance(priest->DR, priest->UR, a.DR, a.UR) <= maxDist)
                target = a.SN;
            break;
        }
    }
    if (target == -1) {
        double bestDist = 1e18;
        for (tagArmy &a : info.armies) {
            if (a.Sort == AT_PRIEST) continue;
            // 侦察骑兵不参与防御，也不占用祭司的治疗额度（它的命不值钱，主力兵值钱）
            if (a.Sort == AT_SCOUT) continue;
            if (a.MaxBlood <= 0 || a.Blood >= a.MaxBlood) continue;   // 满血不用治
            double d = calDistance(priest->DR, priest->UR, a.DR, a.UR);
            if (d > maxDist) continue;
            if (d < bestDist) { bestDist = d; target = a.SN; }
        }
    }

    if (target == -1) {           // 没有伤兵：交回给回村逻辑
        healTargetSN = -1;
        return false;
    }

    // 目标变了、或祭司空闲（上一条指令已完成）时才重新下令，
    // 否则不要每帧重下，免得打断正在进行的治疗
    if (target != healTargetSN || priest->NowState == HUMAN_STATE_IDLE) {
        healTargetSN = target;
        HumanAction(priest->SN, target);
    }
    return true;                  // 已接管祭司
}

// ---------- 探路：祭司走环形 BFS（next_ring_point），侦察骑兵走 DFS（next_dfs_point）----------
// 两者的目的、参数、理由见上方"祭司：前期找家附近资源点" / "侦察骑兵：后期找敌军大本营"
// 两段常量说明；没造出侦察兵时先由祭司代劳。
void demand_scout()
{
    // 探路者：优先用侦察兵（速度 4.07），没有才退回祭司（2.24）。
    // 用侦察兵探路还有个好处：祭司可以一直留在家里，治疗和转化都不用跑远。
    tagArmy *priest = nullptr;
    tagArmy *scout  = nullptr;
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) { priest = &a; continue; }
        if (a.Sort == AT_SCOUT && scout == nullptr) scout = &a;
    }
    if (scout == nullptr) scout = priest;   // 还没造出侦察兵：只能让祭司去探
    if (scout == nullptr) return;           // 祭司也不在（死亡即游戏结束）
    const bool scoutIsUnit = (scout != priest);   // true = 有独立的侦察骑兵

    // 【祭司的空闲时间先拿去治伤兵（13 分钟前）】
    //   必须在这里就做，而且结果要挡住后面的 recall_priest_home(priest) ——
    //   回村和治疗是**同一个单位的两条指令**，内核只保留最后一条，
    //   先治后回村 = 治疗被覆盖（所以下面那处 recall 用 !priestHealing 兜住）。
    //   只在“探路者是侦察骑兵”时在这里治：探路者就是祭司的话它得去探图，
    //   只有 3.5 分钟之后的 timeUp 才空闲（那条路径由下面的 else if (timeUp) 处理）。
    const bool priestHealing = (priest != nullptr) && scoutIsUnit && priest_heal(priest);

    // 【用户要求】第三波（14:00，phase>=3）防守打完之前，侦察骑兵不出门：
    //   那段时间敌方三波部队正在地图上走，侦察兵单枪匹马撞上去必死
    //   （它不参战、也不参加防守），而家附近本来就是祭司环扫的覆盖范围。
    //   等第三波结束再出门找敌军大本营 —— 找到后回村待命（见本函数后面的分支）。
    if (scoutIsUnit && phase < 3) {
        recall_priest_home(scout);
        return;
    }

    // （敌方位置 / 武器工程厂的记录已统一挪到 record_enemy_positions()：由 bt_sync 每帧调用，
    //   但**只在第三阶段（侦察骑兵出门探图之后）真正记录**，看到建筑/部队都算。）

    // ---- 第三阶段：不再探图（用户 2026-09 要求）----
    // “打死了不能用绝望探图，打死了不就说明那个方向那个位置有敌兵？直接冲不就好了？”
    //   侦察骑兵一旦阵亡，它的最后位置已经被 record_enemy_positions() 记成敌方位置，
    //   部队直接朝那儿冲就行；祭司交给 demand_attack 指挥，**不再让它自己跑出去探路**
    //   （祭司跑远了家里既没治疗也没防守，反攻还等着它去转化）。
    if (phase >= 3 && !scoutIsUnit) return;

    // ---- 第三阶段：侦察骑兵的活干完了 —— **不躲避，直接扎进敌军** ----
    // 【用户 2026-09-21：“侦察骑兵别躲避了，扎进敌军让敌军打死得了。毕竟看到敌军
    //   也是代表找到了武器厂，记得看到敌军就可以标记大致方位了”】
    //   ① 它占 1 个人口却不是战斗兵，躲来躲去既探不完图、也永远死不掉，白占名额；
    //   ② **死在哪里就说明敌人在哪里** —— record_enemy_positions ④ 会把它的阵亡点
    //      记成敌方位置，所以“扑上去被打死”本身就是一次成功的侦察；
    //   ③ 所以“看到敌方部队/建筑”（enemyFarFound）**就算找到大本营**，不必非得先
    //      看到武器工程厂 —— 部队推过去、视野一开，那个自然会补上。
    //   （上面 `phase>=3 && !scoutIsUnit → return` 已经保证这里 scoutIsUnit，
    //     所以绝不会误伤祭司。）
    //
    // 【用户 2026-09-21 第二次改口：“侦察骑兵无视所有敌人”】
    //   上一版是“一见敌军就掉头扑过去（enemyFarFound 也算）”—— 实战里它经常被
    //   半路遇上的**一支流兵**引走、半途被打死，**真正的敌营反而从来没找到**。
    //   现在改成：**不躲、也不冲**，敌人出现在视野里完全不影响它的行动 ——
    //   该走哪一格还走哪一格（next_dfs_point 从头到尾不看敌人，
    //   全文件里只有祭司会用 SCOUT_THREAT_RADIUS 撤离）。
    //   它死在哪儿，record_enemy_positions ④ 照样把那里记成敌方位置，
    //   所以“不主动扑上去”并没有丢掉任何侦察信息。
    //   唯一的收工条件：真的看到了敌方**武器工程厂**（enemySiegeSN，胜利目标）
    //   —— 这时它的活干完了，自裁把 1 个人口让给复合弓兵。
    if (phase >= 3 && scoutIsUnit && enemySiegeSN != -1) {
        HumanAction(scout->SN, scout->SN);
        return;
    }

    // 探路者不是祭司时，把祭司收回村待命（治疗/防守都在家附近做）。
    // 两个保护：只在它手里没活干时下令；有敌袭时不下（否则会覆盖掉
    // combat_tactic 同一帧刚下的转化指令，这个坑之前踩过）。
    if (scoutIsUnit && priest != nullptr && !priestHealing
        && !bt_enemy_at_home() && priest->NowState == HUMAN_STATE_IDLE) {
        recall_priest_home(priest);
    }

    // ---- 敌方正在打我方的家：祭司交给 combat_tactic（箭塔拉仇恨 + 转化）----
    // 这一段必须放在"时间到回村"之前，否则回村指令会把同一帧刚下的转化指令覆盖掉。
    // 只有祭司还在外面很远时才叫它回村，已经在塔/中心附近就让它专心转化。
    if (bt_enemy_at_home()) {
        // 祭司必须回村防守（转化），不管它是不是探路者
        if (priest != nullptr) {
            int ax = -1, ay = -1;
            if (get_defense_anchor(ax, ay)
                && calDistance(priest->DR, priest->UR,
                               ax * BLOCKSIDELENGTH, ay * BLOCKSIDELENGTH)
                   > SCOUT_HOME_CALL_RADIUS * BLOCKSIDELENGTH) {
                recall_priest_home(priest);
            }
        }
        // 探路者就是祭司：这一帧交给 combat_tactic，不要下移动指令覆盖转化
        if (scout == priest) return;
        // 探路者是独立侦察兵：它现在不躲避了（要扑上去），继续往下走
    }

    // ---- 探图何时收工 ----
    // 祭司：两个条件任一满足就收工回村 ——
    //   ① 家附近（环半径 SCOUT_RING_MAX 以内）扫完了（next_ring_point 返回 false）；
    //   ② 3.5 分钟到点（实际先到的通常是这个：3.5 分钟大约扫到半径 35 格）。
    //   注意**没有**"见着矿就提前收工"：那样 1 分半就会回村，半径 20~40 那圈全黑着。
    // 侦察骑兵：**不受这些限制**。马厩是工具时代才解锁的，侦察骑兵通常 4 分钟以后
    //   才出生；如果也套用"3.5 分钟回村"，它一出马厩就被叫回家、站在塔下不动
    //   ——这就是"侦察骑兵造出来不侦察"的原因。它的任务只有找到敌方大本营，
    //   找到之前一直探，找到之后回村待命（见本函数开头）。
    int returnFrame = (int)(3.5 * 60 * 1000.0 / TimePerFrame);
    bool timeUp = (info.GameFrame > returnFrame);

    if (!scoutIsUnit && timeUp) {
        // 探路者就是祭司：到点收工 → 先给伤兵回血，没伤兵就回村待命
        if (priest_heal(priest)) return;
        recall_priest_home(priest);
        return;
    }

    // ---- 探图途中遇到敌人：**祭司**朝背离方向撤离（不回村）；侦察骑兵不躲 ----
    // 在自家范围内时不撤：交给 combat_tactic 的「箭塔拉仇恨 + 祭司转化」
    bool atHome = false;
    for (tagBuilding &b : info.buildings) {
        if (b.Type == BUILDING_CENTER && b.Percent >= 100) {
            atHome = (calDistance(scout->DR, scout->UR,
                                  b.BlockDR * BLOCKSIDELENGTH,
                                  b.BlockUR * BLOCKSIDELENGTH)
                      < HOME_DEFEND_RADIUS * BLOCKSIDELENGTH);
            break;
        }
    }
    // 【2026-09-21】只对祭司生效：侦察骑兵现在**不许躲避**（要扑上去让敌军打死）。
    if (!scoutIsUnit && !atHome
        && enemy_near(scout->DR, scout->UR, SCOUT_THREAT_RADIUS * BLOCKSIDELENGTH)) {
        scout_retreat(scout);
        return;
    }

    // 卡住检测：1 秒内位移不足 1 格 → 该路点不可达，跳到下一个。
    // 【坑】采样变量必须按"是谁在走"分开：recall_priest_home() 用的是同一组
    // 变量，混用会把祭司的位置写进侦察兵的采样里，卡住检测就永远不成立——
    // 一旦某个路点走不到（隔着水/树林），侦察兵会永远停在原地，看起来就是"不侦察"。
    bool stuck = false;
    int interval = 1000 / TimePerFrame;
    if (interval < 1) interval = 1;
    int    &chkFrame = scoutIsUnit ? scoutUnitCheckFrame : scoutCheckFrame;
    double &chkDR    = scoutIsUnit ? scoutUnitCheckDR    : scoutCheckDR;
    double &chkUR    = scoutIsUnit ? scoutUnitCheckUR    : scoutCheckUR;
    if (chkFrame == 0) {
        chkFrame = info.GameFrame;
        chkDR = scout->DR;
        chkUR = scout->UR;
    } else if (info.GameFrame - chkFrame >= interval) {
        double mDR = scout->DR - chkDR; if (mDR < 0) mDR = -mDR;
        double mUR = scout->UR - chkUR; if (mUR < 0) mUR = -mUR;
        if (mDR < 1.0 && mUR < 1.0) stuck = true;
        chkFrame = info.GameFrame;
        chkDR = scout->DR;
        chkUR = scout->UR;
    }

    // 走到路点（空闲）或被卡住时才取下一个路点
    if (scout->NowState != HUMAN_STATE_IDLE && !stuck) return;

    // 指令节流：同样按单位分开，别让祭司的回村指令卡住侦察兵的取点节奏
    int &ordFrame = scoutIsUnit ? scoutUnitOrderFrame : priestOrderFrame;

    // 取点节流：刚下过指令、单位可能还没进入行走状态时不重复取点
    if (!stuck) {
        int gap = 300 / TimePerFrame;
        if (gap < 1) gap = 1;
        if (ordFrame != 0 && info.GameFrame - ordFrame < gap) return;
    }

    int tx = -1, ty = -1;
    // 取下一枝/下一个路点：
    //   侦察骑兵 → DFS（一路往深处扎，找敌军大本营）
    //   祭司     → 环形 BFS（找家附近资源点），环半径封顶 SCOUT_RING_MAX。
    //              （“侦察骑兵没了就让祭司往外扩”的绝望探图已按用户要求删除。）
    bool gotPoint = scoutIsUnit ? next_dfs_point(scout, stuck, tx, ty)
                                : next_ring_point(tx, ty);
    if (!gotPoint) {
        // 侦察兵：四面八方都被挡死；祭司：该扫的环都扫完了。→ 回村待命
        recall_priest_home(scout);
        return;
    }

    ordFrame = info.GameFrame;
    HumanMove(scout->SN, (tx + 0.5) * BLOCKSIDELENGTH, (ty + 0.5) * BLOCKSIDELENGTH);
}


// ---------- 派发：排序 + 派发 ----------
void bt_dispatch()
{
    sort_tasks();
    assign_tasks();
}

// 建造位外圈一圈是否干净：检查 (x,y) 起 size×size **外扩 1 格**的环形区域，
// 里面不能有建筑 / 敌方建筑 / 资源 / 我们规划中的占位（MAP）/ 水边一格。
// 用途：**农田和房屋** —— 农田外圈堵住村民就站不进去；房屋必须留缝，
// 否则连成一道墙会把村民围死（现在这是保证“家里四通八达”的**唯一**手段，
//  原那条“市中心南侧专用通道”已随布局网格一起删除）。
// 注意：不检查移动单位（村民/军队会走动，不能因为路过就否掉一个位置）。
bool build_margin_clear(int x, int y, int size)
{
    for (int i = x - 1; i <= x + size; i++) {
        for (int j = y - 1; j <= y + size; j++) {
            if (i >= x && i < x + size && j >= y && j < y + size) continue;  // 本体跳过
            // 【第五遍复查】上界必须一起判：外圈比建筑本体大 1 格，而 find_block
            //   只保证了本体不越界 ⇒ 地图边上的候选（x+size == MAP_L）会越到这里，
            //   MAP[i][j] 是定长 505 数组，越界就是 UB。（本工程 MAP_L=100 碰不到，
            //   但这条判据本身应该是完整的。）
            if (i < 0 || j < 0 || i >= 505 || j >= 505) return false;
            if (MAP[i][j] != 0) return false;              // 我们自己规划的占位
            if (block_is_water_side(i, j)) return false;   // 水边站不住

            for (tagBuilding &b : info.buildings) {
                int bs = building_size(b.Type);
                if (i >= b.BlockDR && i < b.BlockDR + bs &&
                    j >= b.BlockUR && j < b.BlockUR + bs) return false;
            }
            for (tagBuilding &b : info.enemy_buildings) {
                int bs = building_size(b.Type);
                if (i >= b.BlockDR && i < b.BlockDR + bs &&
                    j >= b.BlockUR && j < b.BlockUR + bs) return false;
            }
            for (tagResource &r : info.resources) {
                if (r.BlockDR == i && r.BlockUR == j) return false;
            }
        }
    }
    return true;
}

// ---------- 建造址“工人走得到吗”的连通性检查 ----------
// 【用户 2026-09-23：“工人被拦路了卡死了没法建建筑。然后一直没建起来”】
//   反复 HumanAction / HumanBuild 治不了根：内核只要判定“无用地形移动次数超限”
//   就会**强制中断建造关系**（村民退回 IDLE、工地停在 0%），我们再催一次、再被中断。
//   根因在**选址那一刻**：find_block / build_site_ok / build_margin_clear
//   问的都是“这块地**本身**合法吗”，**从来没人问过“村民走得到它旁边吗”**。
//   地址一旦落在被建筑/树林/矿围出的口袋里、或唯一入口被另一栋楼堵上，就永远进不去。
//   ⇒ 把**连通性**提到选址判据里（BFS 见上面的 reach_bfs）。
//   【它只是“优选”不是硬门槛】这张位图比引擎真实寻路保守（多排了浅滩/水边一格），
//     所以下面用 xBak/yBak 兜底，不能让建造任务永远卡 WAITING。
// 候选地基的**外圈**（建筑本体那一圈不算）里，有没有 BFS 摸到过的格子。
// 有 ⇒ 工人能站到工地旁边，能开工。
static bool site_reachable(int bx, int by, int size)
{
    for (int i = bx - 1; i <= bx + size; ++i) {
        for (int j = by - 1; j <= by + size; ++j) {
            if (i >= bx && i < bx + size && j >= by && j < by + size) continue;  // 本体跳过
            if (i < 0 || j < 0 || i >= 505 || j >= 505) continue;
            if (reachStamp[i][j] == reachCur) return true;
        }
    }
    return false;
}

void sort_tasks()
{
    std::sort(taskQueue.begin(), taskQueue.end(),
        [](const Task &a, const Task &b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.id < b.id;
        });
}

void assign_tasks()
{
    std::set<int> assignedThisFrame;
    std::set<int> lockedRes;
    // 只有"农田"需要互斥（说明：多人采同一块农田只有一人能拿到食物）；
    // 树/石/金/浆果允许多人同时采——否则每个资源点只能挂 1 个村民，
    // 其余同类任务会一直找不到目标、永远卡在 WAITING。
    for (Task &t : taskQueue)
        if (t.type == TASK_GATHER && t.resourceType == GATHER_FARM && t.targetSN != -1
            && t.state != TASK_DONE && t.state != TASK_FAILED)
            lockedRes.insert(t.targetSN);

    // 【统一判据】只有 farmer_available() 说"他真闲"才用他 ——
    //   它同时挡住"内核说他忙"和"我刚派过他"（派活是异步生效的）。
    auto find_idle = [&]() -> tagFarmer* {
        for (tagFarmer &f : info.farmers) {
            if (f.FarmerSort != FARMERTYPE_FARMER) continue;
            if (assignedThisFrame.count(f.SN)) continue;
            // 正负责一个还没建完的建造任务：绝不能被派去干别的。
            // 一旦被拉走，那栋楼就永远烂尾（同位置不能再下建造单，见 on_build_task 注释）。
            if (on_build_task(f.SN)) continue;
            if (!farmer_available(f)) continue;
            return &f;
        }
        return nullptr;
    };

    for (Task &t : taskQueue) {
        if (t.state != TASK_WAITING) continue;

        if (t.type == TASK_GATHER) {
            // ---- 农田特殊处理：只派"这块田的主人"，不随便挑空闲村民 ----
            // 内核是地主制（Building_Resource::isGathererAsLandlord）：一块田只认
            // 第一个到的采集者，换个人到了也采不到，只能白站。谁是主人存在
            // farmHolder 里（农田建成时内核会把建造者转成地主，我们同步登记）。
            if (t.resourceType == GATHER_FARM) {
                int farmSN = -1, farmerSN = -1;

                // ① 有田的主人正闲着 → 优先派他回自己的田
                for (tagBuilding &b : info.buildings) {
                    if (b.Type != BUILDING_FARM) continue;
                    if (b.Percent < 100 || b.Cnt <= 0) continue;
                    if (lockedRes.count(b.SN)) continue;
                    auto it = farmHolder.find(b.SN);
                    if (it == farmHolder.end()) continue;
                    tagFarmer *owner = nullptr;
                    for (tagFarmer &q : info.farmers)
                        if (q.SN == it->second) { owner = &q; break; }
                    if (owner == nullptr) continue;                     // 主人没了
                    if (owner->NowState != HUMAN_STATE_IDLE) continue;  // 主人正忙
                    if (assignedThisFrame.count(owner->SN)) continue;
                    // 主人同时还在负责一个没建完的工地 → 先让它把楼建完
                    if (on_build_task(owner->SN)) continue;
                    farmSN = b.SN;
                    farmerSN = owner->SN;
                    break;
                }

                // ② 没有"有主又闲着"的田 → 找一块无主的田，派个空闲村民并登记为新主人
                //    没有无主田了就不硬塞，保持 WAITING 等主人腾出手来——
                //    以前这里会退化成"随便挑一块"，那正是派第二个人过去的根源。
                if (farmSN == -1) {
                    tagFarmer *f = find_idle();
                    if (f == nullptr) continue;
                    double best = 1e18;
                    for (tagBuilding &b : info.buildings) {
                        if (b.Type != BUILDING_FARM) continue;
                        if (b.Percent < 100 || b.Cnt <= 0) continue;
                        if (lockedRes.count(b.SN)) continue;
                        if (farmHolder.find(b.SN) != farmHolder.end()) continue;
                        double d = calDistance(f->DR, f->UR,
                                               b.BlockDR * BLOCKSIDELENGTH,
                                               b.BlockUR * BLOCKSIDELENGTH);
                        if (d < best) { best = d; farmSN = b.SN; }
                    }
                    if (farmSN == -1) continue;
                    farmerSN = f->SN;
                    farmHolder[farmSN] = farmerSN;   // 登记新主人
                }

                HumanAction(farmerSN, farmSN);
                t.farmerSN = farmerSN;
                t.targetSN = farmSN;
                t.state = TASK_ASSIGNED;
                t.startFrame = info.GameFrame;
                assignedThisFrame.insert(farmerSN);
                lockedRes.insert(farmSN);
                continue;
            }

            tagFarmer *f = find_idle();
            if (f == nullptr) continue;

            int resSN = -1;
            int bestUsed = 0x7fffffff;   // 主判据：该点上已经派了几个人（**少的优先**）
            double best = 1e18;          // 平手用：村民到资源的距离
            double bestHaul = 1e18;      // 次判据：资源到“最近可用存放建筑”的距离
            {
                // 两遍扫描：
                //   第一遍只在"没挤满"（已经挂了 < GATHER_PER_RESOURCE_MAX 人）
                //   的资源点里选最近的；
                //   第一遍一个都没找到（资源点太少）时，第二遍放开**政策**上限
                //   （允许超过 3 人），宁可挤一点也不要让村民干等。
                // 【但"站位数"是硬上限，两遍都绕不过去】res_stand_spots() 返回该点
                //   周围能站几个人：0 = 村民根本走不到跟前（密林深处/水里/被建筑围住），
                //   派过去只会被卡住、内核判"行动无用"强制中断关系（用户反馈的"砍不到"）。
                for (int pass = 0; pass < 2 && resSN == -1; pass++) {
                    // 【每遍都要重置比较基准】否则第二遍会拿第一遍的残值当基准
                    bestUsed = 0x7fffffff;
                    bestHaul = 1e18;
                    best = 1e18;
                    for (tagResource &r : info.resources) {
                        if (r.Type != t.resourceType) continue;
                        // 活动物（Cnt=0 但 Blood>0）也允许选中，用于打猎；尸体/普通资源看 Cnt
                        if (r.Cnt <= 0 && r.Blood <= 0) continue;
                        if (lockedRes.count(r.SN)) continue;
                        if (res_too_far(r.Type, r.BlockDR, r.BlockUR)) continue;  // 太远：不采
                        if (gather_spot_dangerous(r.DR, r.UR, gather_danger_radius(r.Type))) continue;  // 危险：不派
                        int spots = res_stand_spots(r.SN);
                        if (spots <= 0) continue;          // 够不到：任何时候都不派
                        // 第一遍：守住"政策上限 3 人"和"物理站位上限"——把人分散开，
                        //         每个人都能真的站到位置上下手；
                        // 第二遍：只剩这些资源点了，允许超员硬挤（总比站着不动强，
                        //         但"够不到"的点(failed spots<=0)依然不派）。
                        const int used = gatherers_on(r.SN);
                        if (pass == 0) {
                            if (used >= GATHER_PER_RESOURCE_MAX) continue;
                            if (used >= spots) continue;
                        }
                        // 【2026-09 用户要求】先认仓库、再找资源：主判据 = 该资源到
                        // **最近的可用存放建筑**的距离（搬一趟来回最短），同距离时
                        // 再用村民到资源的距离平手。这样村民总是优先去仓库旁边干活，
                        // 也就不会再出现“为了远处的资源天天补建仓库”（那套逻辑已删）。
                        // 【主判据 = 已经派在这个点上的人数】少的优先，把人力摊开。
                        //   【为什么必须这样（用户 2026-09 质疑“明明有六个浆果，
                        //   非要让他们在一个浆果工作”）】原来的主判据是 haul（到仓库的
                        //   距离），而开局的 6 丛浆果离同一个谷仓的距离往往只差几十个
                        //   细节单位、甚至完全相等 —— 于是前几个村民的“最优解”永远是
                        //   同一丛，直到撞上 GATHER_PER_RESOURCE_MAX(3) 才换下一丛。
                        //   把 used 提到最前面，“摊开”成为首要目标，“就近”退居其次。
                        double haul = nearest_dropoff_dist(t.resourceType, r.DR, r.UR);
                        double d = calDistance(f->DR, f->UR, r.DR, r.UR);
                        if (pass == 0) {
                            if (used < bestUsed
                                || (used == bestUsed && haul < bestHaul)
                                || (used == bestUsed && haul == bestHaul && d < best))
                            { bestUsed = used; bestHaul = haul; best = d; resSN = r.SN; }
                        } else if (haul < bestHaul || (haul == bestHaul && d < best)) {
                            // 第二遍是“只剩这些点了、硬挤也要上”的兜底，按距离挑就行
                            bestHaul = haul; best = d; resSN = r.SN;
                        }
                    }
                }
            }
            if (resSN == -1) continue;   // 暂无可用资源，保持等待

            HumanAction(f->SN, resSN);
            t.farmerSN = f->SN;
            t.targetSN = resSN;
            t.state = TASK_ASSIGNED;
            t.startFrame = info.GameFrame;
            assignedThisFrame.insert(f->SN);
        }
        else if (t.type == TASK_BUILD) {
            tagFarmer *f = find_idle();
            if (f == nullptr) continue;

            // ---- 【续建】这个任务已经有工地了：必须接着修原来那块地 ----
            // blockDR 有效 = 之前已经下过 HumanBuild，而内核在那次下单时就把建筑对象
            // 建好了（0%，见 Core_List::addRelation 的 CoreEven_CreatBuilding 分支），
            // 所以工地上一定有个 Percent<100 的建筑。这时**不能另找新址**：
            //   · 另找新址 → 旁边再起一个工地，原来那个永远烂尾；
            //   · 对原地重新 HumanBuild → is_BuildingCanBuild 判"与其他物体重叠"直接驳回。
            // 唯一办法是 HumanAction(村民, 工地SN)：内核 handleFarmerAction 对己方
            // 未完工建筑走 CoreEven_FixBuilding = 续建。
            // 走到这里的情形：原建造者阵亡后任务被 recycle_tasks 退回 WAITING。
            if (t.blockDR != -1) {
                for (tagBuilding &b : info.buildings) {
                    if (b.Type != t.buildingType) continue;
                    if (b.BlockDR != t.blockDR || b.BlockUR != t.blockUR) continue;
                    if (b.Percent >= 100) { t.state = TASK_DONE; break; }  // 已经完工了
                    t.targetSN = HumanAction(f->SN, b.SN);   // 续建
                    t.farmerSN = f->SN;
                    t.state = TASK_ASSIGNED;
                    t.startFrame = info.GameFrame;
                    t.resendFrame = 0;
                    t.resendCount = 0;
                    assignedThisFrame.insert(f->SN);
                    break;
                }
                // 已完工（等 recycle_tasks 清掉）或已派去续建 → 都不要另选新址
                if (t.state == TASK_DONE || t.state == TASK_ASSIGNED) continue;
                // 工地上什么都没有（指令丢了/被拆了）→ 按下面的正常流程重选位置
            }

            // 【2026-09-23】要选**新址**了：从这位村民所在格做一次连通性洪水填充
            //   （见 reach_bfs）。必须按“具体某个人”算 —— 换个人站的位置不同，
            //   能走到的范围也不同。同一帧、同一个人只算一次。
            //   放在“续建”分支之后：续建不需要选新址，白算一次全图洪水填充很浪费。
            static int reachMemoSN = -1, reachMemoFrame = -1000000, reachMemoOK = 0;
            bool reachUsable;
            if (reachMemoSN == f->SN && reachMemoFrame == info.GameFrame) {
                reachUsable = (reachMemoOK != 0);
            } else {
                const int fsx = (int)(f->DR / BLOCKSIDELENGTH);
                const int fsy = (int)(f->UR / BLOCKSIDELENGTH);
                reachUsable = (fsx >= 0 && fsy >= 0 && fsx < 505 && fsy < 505)
                              ? reach_bfs(fsx, fsy) : false;
                reachMemoSN = f->SN;
                reachMemoFrame = info.GameFrame;
                reachMemoOK = reachUsable ? 1 : 0;
            }

            int size = building_size(t.buildingType);

            // 锚点优先级：
            //   房屋 → 挨着"**离地图边最近**的那座房屋"（按 edge_distance 比较），
            //          落点要比它更贴边（往边缘排成居住带，不占市中心周围的地面）
            //   资源旁的谷仓/仓库（resourceType/targetSN 有值）→ 挨着目标资源点
            //   箭塔 → 挨着已有的箭塔（聚成一座火力集中的塔群）
            //   其余 → 以市镇中心为中心
            int ax = -1, ay = -1;
            bool houseOutward = false;     // 房屋：是否只要"比锚点更贴地图边"的落点
            int  houseEdge = 0;            // 锚点到最近地图边的距离（房屋外扩判定用）

            // 市中心块坐标：房屋"往外建"和箭塔"聚群"都要拿它当参考
            int cx = -1, cy = -1;
            for (tagBuilding &b : info.buildings) {
                if (b.Type == BUILDING_CENTER) { cx = b.BlockDR; cy = b.BlockUR; break; }
            }

            if (t.buildingType == BUILDING_HOME) {
                // 锚点 = **离地图边缘最近**的那座房屋：新房子接着它继续往那条边排。
                // 用户 2026-09 要求：“直接放在离我方较近的地图最边缘就好，
                // 而不是扩到地图中心去”。
                // 【坑】不能用“离市中心最远”当判据：那只保证离市中心越来越远，
                // 方向可能朝地图中间那一侧跑（营地在角落时特别明显），
                // 所以现在改用“到最近地图边的距离”，并要求落点比锚点更贴边。
                int bestEdge = 0;
                for (tagBuilding &b : info.buildings) {
                    if (b.Type != BUILDING_HOME) continue;
                    int e = edge_distance(b.BlockDR, b.BlockUR);
                    if (ax == -1 || e < bestEdge) {
                        ax = b.BlockDR; ay = b.BlockUR; bestEdge = e;
                    }
                }
                houseOutward = (ax != -1);
                houseEdge = bestEdge;
            }
            if (ax == -1 && t.buildingType == BUILDING_ARROWTOWER) {
                // 箭塔聚群：锚定**离市中心最近的那座已建成箭塔**，新塔贴着它建。
                // 原来的做法是以市中心为锚点环形外扩（startR=6、step=4），
                // 结果三座塔散落在市中心四周、火力互相照应不上（用户反馈"箭塔分布过于零散"）。
                int best2 = 0;
                for (tagBuilding &b : info.buildings) {
                    if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
                    int dx = (cx >= 0) ? b.BlockDR - cx : 0;
                    int dy = (cy >= 0) ? b.BlockUR - cy : 0;
                    int d2 = dx * dx + dy * dy;
                    if (ax == -1 || d2 < best2) {
                        ax = b.BlockDR; ay = b.BlockUR; best2 = d2;
                    }
                }
            }
            if (ax == -1 && t.resourceType != -1 && t.targetSN != -1) {
                for (tagResource &r : info.resources) {
                    if (r.SN != t.targetSN) continue;
                    ax = r.BlockDR;
                    ay = r.BlockUR;
                    break;
                }
            }
            if (ax == -1) {
                for (tagBuilding &b : info.buildings) {
                    if (b.Type == BUILDING_CENTER) {
                        ax = b.BlockDR; ay = b.BlockUR;
                        break;
                    }
                }
            }
            if (ax == -1) continue;   // 无锚点（异常）

            int x = -1, y = -1;
            // 备选落点：“其它校验全过、就是这位村民走不到旁边”。
            // 【为什么要有它】连通性位图比引擎真实寻路保守（多排了水边一格等），
            //   所以它只能当**优选**；真的一份都找不到时才拿这个兜底，
            //   否则建造任务会永远卡在 WAITING（比让工人去碰运气更糟）。
            int xBak = -1, yBak = -1;
            // 以锚点为中心一圈一圈向外扩（环形搜索）；
            //   · 资源旁的存放建筑 → 从 2 格起找，尽快贴着资源建；
            //   · 农田 → 紧贴市中心（环距 1 格），村民采完走一格就能上交；
            //   · 房屋 → 以“**离地图边最近**的那座房屋”为锚点，只收更贴边的落点
            //     （往地图边缘排出一条居住带，市中心周围留给农田/科技建筑）；
            //   · 箭塔 → 以“离市中心最近的已建成箭塔”为锚点，3 格起、每 2 格
            //     一圈地找，贴着已有塔建（聚成塔群，火力集中）；
            //   · 其余（市场/兵营/靶场/马厩/学院…）→ 从 CENTER_BUILD_START_R
            //     格起找，把市中心周围那圈地面让给农田。
            // ---- 【用户 2026-09】候选落点的统一校验（网格搜索与环形兜底共用）----
            auto trySite = [&](int bx, int by, bool strictMargin) {
                if (x != -1) return;
                // 放得下吗（find_block 内部已含“建造位上有没有单位正站着”）
                if (!find_block(bx, by, size, size)) return;
                // 被内核驳回过、还在拉黑期的位置直接跳过，否则会反复挑中它、
                // 反复被驳回（见 UsrAI.h 里 badBuildSite 的说明）
                if (!build_site_ok(bx, by)) return;
                // 农田：周围一圈必须干净。虽然占 3x3，但村民要站到旁边才能干活，
                //   四周被别的东西堵住时会卡住不动。
                if (t.buildingType == BUILDING_FARM
                    && !build_margin_clear(bx, by, size)) return;
                // 房屋：外圈留 1 格空地（房子之间永远留缝，不可能连成一道墙把
                //   某片地方围死）。只在严格那一遍要求，避免房屋任务永远卡 WAITING。
                if (strictMargin && !build_margin_clear(bx, by, size)) return;
                // 【2026-09-23 关键新增】最后一道：**这位村民真能走到这块地旁边吗**。
                //   上面几条问的都是“地本身合不合法”，从没管过“人进不进得去”。
                //   不可达就先记成备选（不是直接否定），整圈搜完都没有可达的再用它兜底。
                if (reachUsable && !site_reachable(bx, by, size)) {
                    if (xBak == -1) { xBak = bx; yBak = by; }
                    return;
                }
                x = bx;
                y = by;
            };

            // ---- 【用户 2026-09】先在“布局网格”上找 ----
            //   只对**锚点是市中心**的建筑生效（房屋 / 资源旁的谷仓仓库 /
            //   第二座以后的箭塔，锚点都不是市中心，各走各的老逻辑）：
            //     · 农田 → 先八宫格（偏移 ±1，8 个、正好 FARM_MAX），再借外圈（±2）
            //     · 其它 → 直接外圈（偏移 ±2，16 个），内圈留给市中心和农田
            //   间距 4 格 = 3 格建筑 + 1 格缝，所以每两块建筑之间天然有路可走。
            if (cx >= 0 && cy >= 0 && ax == cx && ay == cy) {
                const int ringFrom = (t.buildingType == BUILDING_FARM) ? 1 : 2;
                for (int ring = ringFrom; ring <= 2 && x == -1; ++ring) {
                    for (int gi = -ring; gi <= ring && x == -1; ++gi) {
                        for (int gj = -ring; gj <= ring && x == -1; ++gj) {
                            if (gi > -ring && gi < ring
                                && gj > -ring && gj < ring) continue;   // 只取本环
                            if (gi == 0 && gj == 0) continue;           // 中心是市中心自己
                            trySite(cx + gi * BUILD_GRID_PITCH,
                                    cy + gj * BUILD_GRID_PITCH, false);
                        }
                    }
                }
            }

            int startR = CENTER_BUILD_START_R;
            int step   = 4;
            if (t.resourceType != -1) startR = 2;
            if (t.buildingType == BUILDING_HOME) startR = 4;
            // 箭塔：从锚点（已有箭塔）边上 3 格开始、每 2 格一圈地找，尽量贴着建
            if (t.buildingType == BUILDING_ARROWTOWER) { startR = 3; step = 2; }
            if (t.buildingType == BUILDING_FARM) { startR = 2; step = 1; }
            // 房屋分两遍：第一遍只接受"比锚点更靠外"的落点（往地图边缘排）；
            // 一遍都没找到（外面被水/山/资源堵死）→ 第二遍放开方向。
            // **这个兜底必须有**：找不到落点就会一直卡在 WAITING，
            // 人口上限跟着卡死（那是灾难性的）。
            // 【三遍搜索】原来只有两遍，而第一遍一旦失败就会**同时**放开“方向”和
            //   “留缝”两个约束 —— 那正是“房屋连成墙把村民围死”的隐患，
            //   而且第一遍长期失败时它就变成了常态。现在拆开：
            //     pass0：方向 + 留缝都严格（首选）
            //     pass1：**只**放宽留缝，方向仍然严格 —— “沿最近那条边排成居住带”
            //            主要靠这一遍（同一条边上的格子 edge_distance 相等，
            //            用 >= 比较会被全部否掉，所以下面的判据是 >）
            //     pass2：两条都放开（真正的最后兜底）
            for (int pass = 0; pass < 3 && x == -1; ++pass) {
                const bool outward = houseOutward && (pass < 2);
                const bool strictPass = (pass == 0);
                for (int r = startR; r <= 48 && x == -1; r += step) {
                    for (int i = -r; i <= r && x == -1; i++) {
                        for (int j = -r; j <= r && x == -1; j++) {
                            int di = i < 0 ? -i : i;
                            int dj = j < 0 ? -j : j;
                            if (di != r && dj != r) continue;   // 只取本圈环上的点
                            // 房屋：只收"比锚点更贴地图边缘"的落点 —— 房子会沿着
                            // 最近的那条边排，而不会往地图中心扩。
                            if (outward
                                && edge_distance(ax + i, ay + j) > houseEdge)
                                continue;
                            // 上面那套校验（find_block / build_site_ok / 外圈干净）
                            // 已经抽成 trySite，与网格搜索共用。
                            // strictMargin=strictPass：房屋留缝只在前两遍要求。
                            trySite(ax + i, ay + j, strictPass);
                        }
                    }
                }
            }
            // 一次可达的都没找到 → 用之前记下的备选（宁可去碰运气，也不能不出门）
            if (x == -1 && xBak != -1) { x = xBak; y = yBak; }
            if (x == -1) continue;   // 找不到空地，保持等待

            t.targetSN = HumanBuild(f->SN, t.buildingType, x, y);  // 记录指令 id，下一帧查返回码
            t.blockDR = x;
            t.blockUR = y;
            t.farmerSN = f->SN;
            t.state = TASK_ASSIGNED;
            t.startFrame = info.GameFrame;
            assignedThisFrame.insert(f->SN);

            for (int i = x; i < x + size; i++)
                for (int j = y; j < y + size; j++)
                    MAP[i][j] = 1;   // 立即占位，防止重复选址
        }
        else if (t.type == TASK_PRODUCE || t.type == TASK_UPGRADE) {
            for (tagBuilding &b : info.buildings) {
                if (b.Type != t.buildingType) continue;
                if (b.Percent < 100) continue;
                if (b.Project != 0) continue;
                BuildingAction(b.SN, t.targetSN);
                t.state = TASK_DONE;   // 下发即完成（内核异步执行）
                break;
            }
        }
    }

    // 【记下本帧派过活的村民】派活是异步生效的，他们此刻的 NowState 还是 IDLE；
    // 必须靠 farmerOrderFrame 才能认出“我刚派过他”，否则同一帧稍后的
    // demand_gather 兜底 2 会把他再派一次、把这里的指令覆盖掉。
    for (int sn : assignedThisFrame) mark_farmer_order(sn);
}

// 该村民是否正挂着一个"还没建完的建造任务"。
// 【为什么必须保护】内核里建房是**下单那一刻建筑对象就创建好（0%）**，
// 村民再走过去把它一点点修起来（Core_List::addRelation 的 CoreEven_CreatBuilding
// 分支会立刻 addBuilding + theMap->add_Map_Object）。后果：
//   1) 建造关系一旦被强制中断（被自己人挤开、路被堵死、无用地形移动次数超限），
//      那栋楼就永远停在半成品，没人能接手；
//   2) **同一位置不能重新下建造单** —— is_BuildingCanBuild 里 theMap->isHaveObject
//      会把这栋半成品判成"与其他物体重叠"直接驳回，等于那笔木/石白花了。
// 所以：只要 BUILD 任务还挂在某个村民身上没建完，任何派工逻辑都不许动他。
// ---- 村民派活的统一判据 ----
// 【要解决的问题】用户 2026-09 反馈“在干一件事的村民不要让他干别的事”。
//   派村民下指令的入口不止一处（assign_tasks 的任务系统、 demand_gather 末尾的
//   “兜底 2”直接下指令），而“他闲不闲”只能看内核的 `NowState`。
//   但 `HumanAction` 是**异步**的：本帧刚下的指令，要到**下一帧**的 infoShare
//   才会把 NowState 从 IDLE 改掉。于是同一帧里第二个入口会把他再派一次、
//   把前一条指令覆盖掉（“一直在被分配任务、来回跑”就是这么来的）；
//   跨帧还可能撞上内核因“行动无用”中断关系的那一瞬间。
// 【不变量】任何派活入口，只要“内核说他在忙”或“我刚派过他”，一律不碰他。
// 仅查询“我最近刚派过他吗”（无副作用）—— 给 recycle_tasks 用
bool farmer_just_ordered(int sn)
{
    std::unordered_map<int,int>::iterator it = farmerOrderFrame.find(sn);
    return it != farmerOrderFrame.end() && info.GameFrame - it->second <= 1;
}

bool farmer_available(tagFarmer &f)
{
    std::unordered_map<int,int>::iterator it = farmerOrderFrame.find(f.SN);

    if (f.NowState != HUMAN_STATE_IDLE) {
        // 内核说他正忙（走路 / 干活 / 交战）→ 绝不打搅；顺手把过期记录清掉
        if (it != farmerOrderFrame.end()) farmerOrderFrame.erase(it);
        return false;
    }
    // 内核说他空闲 —— 但这可能是“我刚派过他、内核还没反应”，给一帧的宽限
    if (farmer_just_ordered(f.SN)) return false;
    if (it != farmerOrderFrame.end()) farmerOrderFrame.erase(it);   // 确实闲下来了
    return true;
}

// 派活成功后调用（把“我这帧派过他”记下来）
void mark_farmer_order(int sn)
{
    farmerOrderFrame[sn] = info.GameFrame;
}

bool on_build_task(int farmerSN)
{
    if (farmerSN == -1) return false;
    for (Task &t : taskQueue)
        if (t.type == TASK_BUILD && t.state == TASK_ASSIGNED
            && t.farmerSN == farmerSN)
            return true;
    return false;
}

// 把一个“手上还在挖矿”的村民调到最近的树（伐木永远有活）；
// 连树都没有（异常）才让他回家待命。
// 【为什么必须“主动调走”】recycle_tasks 把任务判 TASK_DONE 只是收回**我们自己**
//   的名额，村民手上的采集关系还在 —— 他会把整座矿（石头 CNT_STONE=250、
//   金矿 CNT_GOLDORE=400）挖完才变空闲。石头那次（用户：“挖石头挖到 1020 个”）、
//   金子这次（用户：“金子到后期也太多了”）都是同一个坑。
void send_gatherer_to_wood(int farmerSN)
{
    if (farmerSN == -1) return;
    double fx = 0, fy = 0;
    bool found = false;
    for (tagFarmer &f : info.farmers)
        if (f.SN == farmerSN) { fx = f.DR; fy = f.UR; found = true; break; }
    if (!found) return;                      // 人已经没了

    int bestTree = -1;
    double bestD = 1e18;
    for (tagResource &r : info.resources) {
        if (r.Type != RESOURCE_TREE) continue;
        if (r.Cnt <= 0 && r.Blood <= 0) continue;
        if (res_too_far(r.Type, r.BlockDR, r.BlockUR)) continue;
        if (res_stand_spots(r.SN) <= 0) continue;
        double d = calDistance(fx, fy, r.DR, r.UR);
        if (d < bestD) { bestD = d; bestTree = r.SN; }
    }
    if (bestTree != -1) {
        HumanAction(farmerSN, bestTree);
        // 【必须】与 assign_tasks / 兜底 2 抢人：派活是异步的，内核此刻还说他在 IDLE，
        //   不记一下的话同一帧稍晚的 assign_tasks 会再给他派一个采集任务、把这条顶掉。
        mark_farmer_order(farmerSN);
        return;
    }
    double hx = 0, hy = 0;
    if (home_center(hx, hy)) {
        HumanMove(farmerSN, hx, hy);
        mark_farmer_order(farmerSN);
    }
}

// 【用户 2026-09-23】15:00 之后把**已经在矿上的人**也拉走。
//   为什么必须单独扫一遍：任务表只认得“任务”，而兜底 2 是**直接下指令**派的人
//   （没有任务）⇒ recycle_tasks 那套按任务回收对他们完全无效，
//   他们会把整座矿（CNT_STONE=250）挖完才变空闲 —— 这就是“控不下来石头量”的裂缝。
//   判据用内核真值 `f.WorkObjectSN`（Core.cpp:459 写入的“当前关系目标 SN”），
//   不靠我们自己的记账（兜底 2 那类派活不会更新任何任务表）。
//   节流 STONE_SWEEP_MS：每次都要遍历一遍资源表，不必每帧做。
static int stoneSweepFrame = -1000000;
static void recall_stone_miners()
{
    const int tpf = (TimePerFrame > 0) ? TimePerFrame : 40;
    if (info.GameFrame - stoneSweepFrame < STONE_SWEEP_MS / tpf) return;

    // 先收一遍石矿 SN（资源表可能上千条，别对每个人再遍历一遍）
    std::unordered_map<int,char> stoneSN;
    for (tagResource &r : info.resources)
        if (r.Type == RESOURCE_STONE) stoneSN[r.SN] = 1;
    if (stoneSN.empty()) return;

    stoneSweepFrame = info.GameFrame;
    for (tagFarmer &f : info.farmers) {
        if (f.FarmerSort != FARMERTYPE_FARMER) continue;
        if (f.WorkObjectSN == -1) continue;
        if (!stoneSN.count(f.WorkObjectSN)) continue;
        send_gatherer_to_wood(f.SN);    // 已有的 helper：找最近的树，没树才回家
    }
}

void recycle_tasks()
{
    // 【用户 2026-09-23】15:00 之后先把矿上的人拉走（含兜底 2 派的“没任务”的人）
    if (stone_forbidden_now()) recall_stone_miners();

    // 【超额伐木任务主动回收 —— 用户 2026-09 反馈“砍树的人太多导致卡死”】
    //   active_gather() 是**只增不减**的（demand_gather 里的 while 只补不撤），
    //   所以一旦历史上派超了（封顶生效之前堆积的、或者树被砍掉导致
    //   wood_capacity() 变小、配额跟着降），那群人会一直挂在伐木上堵在树林里。
    //   这里按队列顺序（先派的先留）发 wood_gather_limit() 个名额，超出的直接判 DONE。
    //   注意：**不去打断村民手上的活** —— 他砍完当前这棵（内核会自动中断关系）变空闲即可；
    //   每帧取消一次关系反而是拖动。配额降下来后人数会自动收敛。
    int woodKept = 0;
    const int woodQuota = wood_gather_limit();

    for (Task &t : taskQueue) {
        if (t.state == TASK_DONE || t.state == TASK_FAILED) continue;

        if (t.type == TASK_GATHER) {
            // 【关键修正】WAITING + 还没选到目标 ≠ “目标没了”：
            //   行为树把 `gather` 排在 `dispatch` 之后，任务创建后要等**下一帧**的
            //   assign_tasks 才会写 targetSN。不区分的话，这里会把刚建出来的任务
            //   全部判 DONE 删掉 —— assign_tasks 的采集/农田分支就永远不可达，
            //   active_gather() / gatherers_on() 也恒为 0（配额、限流全部失效）。
            //   给它一帧的机会；仍然没派出去，才说明真的“派不出去”
            //   （资源没了 / 没田 / 站位不够）→ 回收，别占着 active_gather 名额。
            if (t.state == TASK_WAITING && t.targetSN == -1) {
                if (info.GameFrame - t.startFrame <= 1) continue;
                t.state = TASK_DONE;
                continue;
            }
            bool targetGone = (t.targetSN == -1);
            if (t.targetSN != -1) {
                targetGone = true;
                for (tagResource &r : info.resources)
                    if (r.SN == t.targetSN) { targetGone = false; break; }
                // 农田目标是建筑：只要还有剩余食物就不算消失
                if (targetGone) {
                    for (tagBuilding &b : info.buildings)
                        if (b.SN == t.targetSN && b.Cnt > 0) { targetGone = false; break; }
                }
            }
            bool farmerIdle = false;
            bool farmerGone = false;   // 被派工的村民已不存在（阵亡）
            if (t.farmerSN != -1) {
                farmerGone = true;
                for (tagFarmer &f : info.farmers)
                    if (f.SN == t.farmerSN) {
                        farmerGone = false;
                        // 判“他闲下来了”必须排除“我刚派过他、内核还没反应”的窗口，
                        // 否则刚派出去的任务会被当成已完成收掉（“来回折腾”的来源之一）
                        farmerIdle = (f.NowState == HUMAN_STATE_IDLE)
                                     && !farmer_just_ordered(f.SN);
                        break;
                    }
            }
            // 注意：村民阵亡时目标可能还在，必须用 farmerGone 收尾，
            // 否则任务永远停在 ASSIGNED、占着 active_gather 名额，导致该资源不再派新任务。
            // 农田不需要特殊处理："谁占着这块田"存在 farmHolder 里（见 assign_tasks），
            // 任务就算被回收，下次派工时也会按 owner 把主人派回去，
            // 而且不会把已经有人占的田派给第二个人。
            if (targetGone || farmerGone) {
                t.state = TASK_DONE;          // 目标没了 / 人阵亡 → 任务作废
            }
            else if (farmerIdle) {
                // 【退回 WAITING，而不是作废】村民“闲下来”往往只是被自己人挤开、
                //   或内核判“行动无用”中断了关系。作废 + 让 demand_gather 新建一个，
                //   会造成“任务被收掉 → 新建 → 派到另一个点 → 又被中断”的来回折腾
                //   （用户 2026-09：“在干一件事的村民不要让他干别的事”）。
                //   退回 WAITING 后 assign_tasks 会重新派他，多数情况还是原来那个点。
                t.state = TASK_WAITING;
                t.farmerSN = -1;
                t.targetSN = -1;
                t.startFrame = info.GameFrame;   // 重置帧龄，别被“WAITING 超时”立刻收掉
            }
            else if (t.resourceType == RESOURCE_TREE) {
                // 活着走到这里的伐木任务才占名额：超出配额的一律释放
                // （村民不会被召回，他砍完手上这棵就会变空闲；见函数开头的注释）
                if (woodKept >= woodQuota) { t.state = TASK_DONE; continue; }
                woodKept++;
            }
            else if (t.resourceType == RESOURCE_STONE
                     && (info.Stone >= STONE_KEEP_MAX || !stone_needed())) {
                // 【石头够了就回收，并且**主动把人调走** —— 用户 2026-09-23：
                //   “你最后挖石头挖到 1020 个是要干嘛？”】
                //   原来只 `t.state = TASK_DONE`（只收回我们的名额），而村民手上的
                //   **采集关系还在**：他会把整座矿（CNT_STONE = 250）挖完才变空闲。
                //   所以这里必须补一刀：直接把他调到最近的树（伐木永远有活）；
                //   连树都没有（异常）才让他回家待命。
                //   这也是“超额不再派新人”和“已经在挖的人怎么办”之间的那道裂缝。
                t.state = TASK_DONE;
                send_gatherer_to_wood(t.farmerSN);
                continue;
            }
            else if (t.resourceType == RESOURCE_GOLD && !gold_needed()) {
                // 【金子够了也回收 + 把人调走】与石头同一个坑：只收任务不打断村民，
                //   而一块金矿点自带 CNT_GOLDORE = 400（config.json），
                //   采集关系一旦建立他就会把整座矿挖完。
                //   判据与 demand_gather 派人数**共用 gold_needed()**（以前两边不一致：
                //   那边按人口比例、这边只看固定上限 300 ⇒ “金堆到 300 就没人挖，
                //   可弓兵还在一直死一直补” = 用户说的“后期金不够”）。
                t.state = TASK_DONE;
                send_gatherer_to_wood(t.farmerSN);
                continue;
            }
        }
        else if (t.type == TASK_BUILD) {
            int builtSN = -1;   // 建成后的建筑 SN（农田要用）

            // 0) 被派去建造的村民不在了（阵亡）→ 按工地是否已存在分别处理
            //    · 工地还在 → **退回 WAITING 并保留工地坐标**，下一帧 assign_tasks
            //      走"续建分支"派新人接着修原来那块地（判 FAILED 会释放占位 + 另选新址，
            //      原工地就永远烂尾，而同一位置又没法重新下建造单）；
            //    · 连地基都没有（指令从没生效）→ 判 FAILED，顺带释放占位。
            if (t.state != TASK_DONE && t.state != TASK_FAILED && t.farmerSN != -1) {
                bool gone = true;
                for (tagFarmer &f : info.farmers)
                    if (f.SN == t.farmerSN) { gone = false; break; }
                if (gone) {
                    bool siteExists = false;
                    for (tagBuilding &b : info.buildings)
                        if (b.Type == t.buildingType && b.BlockDR == t.blockDR
                            && b.BlockUR == t.blockUR && b.Percent < 100) {
                            siteExists = true;
                            break;
                        }
                    if (siteExists) {
                        t.state = TASK_WAITING;
                        t.farmerSN = -1;
                        t.resendFrame = 0;
                        t.resendCount = 0;
                    } else {
                        t.state = TASK_FAILED;
                    }
                }
            }

            // 0.5) 建造者还在、但已经空闲下来了（内核把建造关系中断了：被自己人挤开、
            //      被判定"无用地形移动次数超限"等）→ 同样退回 WAITING，
            //      下一帧 assign_tasks 走"续建分支"把他叫回工地接着修。
            //      【为什么必须有】on_build_task() 会保护他（不让他被派去干别的），
            //      可如果任务一直挂 ASSIGNED、他又一直 IDLE，他就**永远站在那里**
            //      —— 这正是用户反馈的"村民站着不动"的另一种形态。
            if (t.state == TASK_ASSIGNED && t.farmerSN != -1) {
                for (tagFarmer &f : info.farmers)
                    if (f.SN == t.farmerSN) {
                        if (f.NowState == HUMAN_STATE_IDLE
                            && !farmer_just_ordered(f.SN)) {
                            t.state = TASK_WAITING;
                            t.farmerSN = -1;
                            t.startFrame = info.GameFrame;
                        }
                        break;
                    }
            }

            // 1) 内核返回错误：建造被拒绝 → 立即失败并释放占位。
            //    【必须这么判】ins_ret 里存的是内核的返回码，而 config.h 的
            //    ACTION_STATUS_CODE 里**0 才是成功，错误码全是正数**（1..22）。
            //    这里以前写的是 `ret < 0`，所以这个判断从来没生效过：
            //    被驳回的建造任务会一直挂 ASSIGNED，建造者被 on_build_task 锁在原地
            //    干等到 4.8 分钟超时；如果又碰上位置搜索的确定性重试，
            //    就变成"每几秒重下一单、每次都被内核驳回"的循环（用户反馈的
            //    "尝试建仓库多次但没成功"）。
            if (t.targetSN >= 0 && info.ins_ret.count(t.targetSN)) {
                int ret = info.ins_ret[t.targetSN];
                if (ret != 0) {
                    t.state = TASK_FAILED;
                    // "选址被否"类的错误（有重叠/越界/未探索/高度差/位置不合适…）：
                    // 把这块地拉黑一段时间，否则下一帧位置搜索还会挑中它，无限重试。
                    bool siteRejected = (ret >= ACTION_INVALID_HUMANBUILD_DIFFERENTHIGH
                                      && ret <= ACTION_INVALID_POSITION_NOT_FIT);
                    if (siteRejected && t.blockDR != -1) {
                        // 工地上已经有建筑（哪怕 0%）说明位置本身没问题，
                        // 是"续建"失败（比如村民不空闲），不该拉黑这块地
                        bool siteExists = false;
                        for (tagBuilding &b : info.buildings)
                            if (b.BlockDR == t.blockDR && b.BlockUR == t.blockUR) {
                                siteExists = true; break;
                            }
                        if (!siteExists) mark_build_site_bad(t.blockDR, t.blockUR);
                    }
                }
            }

            // 2) 建筑建成
            //    注意：这里**不排除已判 FAILED 的任务**——内核可能先回了错误码、
            //    实际却建成了（错误码非 0 但建筑已经在图上）。只要位置上真的
            //    有这个建筑就按建成处理，否则会漏掉"农田已建好、任务却判失败"，
            //    接着就会再派一个人去同一块田。
            if (t.blockDR != -1) {
                for (tagBuilding &b : info.buildings) {
                    if (b.Type == t.buildingType && b.BlockDR == t.blockDR
                        && b.BlockUR == t.blockUR && b.Percent >= 100) {
                        t.state = TASK_DONE;
                        builtSN = b.SN;
                        break;
                    }
                }
            }

            // 2.5) 建造者"掉线"了 → 把它拽回工地接着修（这是唯一的补救办法）
            //      内核把"走路去建/正在建"都算 WORKING（getNowPhaseNum 的
            //      CoreEven_FixBuilding/CreatBuilding 分支），所以建造者一旦变回 IDLE，
            //      说明那条建造关系已经被内核断掉了（被自己人挤开、路被堵死、
            //      无用地形移动次数超限……），而那栋楼还没建完。
            //      以前这里不管这件事：任务一直是 ASSIGNED，active_build() 也算着它，
            //      demand_build 不会再派第二个人，于是那块地就永远是个半成品
            //      （用户反馈的"有没建完的建筑"）。
            //      补救手段：HumanAction(原建造者, 那栋楼)。内核 handleFarmerAction
            //      对己方**未完工**建筑走的是 CoreEven_FixBuilding（续建），
            //      不是采集——所以这就是"回去接着修"。
            if (t.state == TASK_ASSIGNED && t.farmerSN != -1
                && t.blockDR != -1 && t.buildingType != -1) {
                int siteSN = -1;
                for (tagBuilding &b : info.buildings) {
                    if (b.Type == t.buildingType && b.BlockDR == t.blockDR
                        && b.BlockUR == t.blockUR) {
                        siteSN = b.SN;
                        break;
                    }
                }

                tagFarmer *f = nullptr;
                for (tagFarmer &ff : info.farmers)
                    if (ff.SN == t.farmerSN) { f = &ff; break; }

                if (f != nullptr && f->NowState == HUMAN_STATE_IDLE
                    && info.GameFrame - t.startFrame > 500 / TimePerFrame) {
                    int gap = 1000 / TimePerFrame;      // 1 秒最多催一次
                    if (gap < 1) gap = 1;
                    if (t.resendFrame == 0 || info.GameFrame - t.resendFrame >= gap) {
                        if (siteSN != -1) {
                            // 地基已经立在那儿了（哪怕 0%）：把原建造者叫回去修完
                            if (t.resendCount < BUILD_RESUME_MAX) {
                                t.resendCount++;
                                t.resendFrame = info.GameFrame;
                                HumanAction(t.farmerSN, siteSN);
                            } else {
                                // 反复叫不动（地形卡死）：认输，释放占位重排新任务
                                t.state = TASK_FAILED;
                            }
                        } else if (info.GameFrame - t.startFrame
                                   > BUILD_LOST_GRACE_MS / TimePerFrame) {
                            // 位置上连地基都没有：建造指令被去重丢掉或被驳回了，
                            // 给 3 秒宽限仍无消息就判失败（下一帧会重排一次）
                            t.state = TASK_FAILED;
                        }
                    }
                }
            }

            // 3) 超时
            if (t.state != TASK_DONE && t.state != TASK_FAILED
                && info.GameFrame - t.startFrame > 60 * 120)
                t.state = TASK_FAILED;

            // 4) 释放占位。
            // 【2026-09-23 第五遍复查修正：**建成也要释放**】
            //   原来只有 FAILED 才释放，注释写的是“建成则保留占位”，意图是
            //   “农田被内核采完删掉后，补建回到原来那一格”（见 BUILD_GRID_PITCH）。
            //   但**这个意图根本实现不了**：find_block 开头就把 MAP!=0 的格子否掉
            //   （`if (MAP[x+i][y+j] != 0) return 0;`）⇒ 被永久占着的八宫格反而
            //   再也建不了农田，农田只能一圈圈往外飘、越建离市中心越远
            //   （后期食物全靠农田，这是实打实的损耗）；更糟的是
            //   **被打坏的建筑**（第三波的箭塔/房屋）也永远占着那块地。
            //   而“占位”真正要防的只有一件事：从“我们决定这块地”到“内核把那栋楼
            //   建出来”之间，别的任务又挑中同一块地。这段时间结束的标志就是
            //   我们真的在 info.buildings 里看到那栋楼（builtSN != -1）——
            //   而 find_block / block_is_standable / ensure_rally_ok_map
            //   都会按 info.buildings 的占地判定，所以这时释放 MAP 不会重叠选址。
            if (t.blockDR != -1
                && (t.state == TASK_FAILED
                    || (t.state == TASK_DONE && builtSN != -1))) {
                int size = building_size(t.buildingType);
                for (int i = t.blockDR; i < t.blockDR + size; i++)
                    for (int j = t.blockUR; j < t.blockUR + size; j++)
                        MAP[i][j] = 0;   // 释放占位
            }

            // 5) 农田刚建成：内核会把**建造者本人**自动转成这块田的采集者
            //    （Core_List::object_FinishAction 里 CoreEven_FixBuilding 结束时
            //     发现目标是农田 → addRelation(Gather)），地主当场就定了。
            //    我们只需把它登进 farmHolder，之后 assign_tasks 只会派这位主人去。
            if (t.state == TASK_DONE && t.buildingType == BUILDING_FARM
                && builtSN != -1 && t.farmerSN != -1) {
                farmHolder[builtSN] = t.farmerSN;
            }
        }
        else if (t.type == TASK_PRODUCE || t.type == TASK_UPGRADE) {
            t.state = TASK_DONE;
        }
    }

    taskQueue.erase(
        std::remove_if(taskQueue.begin(), taskQueue.end(),
            [](const Task &t) { return t.state == TASK_DONE || t.state == TASK_FAILED; }),
        taskQueue.end());
}

// ==================== 战斗：箭塔拉仇恨 + 祭司转化 ====================
// 箭塔全图索敌并攻击，把敌人从祭司身边拉走（拉仇恨）；祭司趁机转化敌人。
// 仅在"敌方逼近我方城市"时启用（见 bt_enemy_at_home），避免与祭司探图互相干扰。
void combat_tactic()
{
    // 防御触发条件：敌方必须已逼近我方城市
    if (!bt_enemy_at_home()) return;

    // 收集可见敌人（敌方没有农民，只数军队）
    std::vector<int> enemies;
    for (tagArmy &e : info.enemy_armies) enemies.push_back(e.SN);
    if (enemies.empty()) {
        towerFocusSN = -1;      // 无敌人：清空仇恨标记
        convertTargetSN = -1;
        return;
    }

    // 找己方祭司
    tagArmy *priest = nullptr;
    for (tagArmy &a : info.armies)
        if (a.Sort == AT_PRIEST) { priest = &a; break; }

    // 某敌兵当前是否正在攻击祭司
    auto attackingPriest = [&](int sn) -> bool {
        if (priest == nullptr) return false;
        for (tagArmy &e : info.enemy_armies)
            if (e.SN == sn && e.WorkObjectSN == priest->SN) return true;
        return false;
    };

    // ---- 1) 箭塔集火 ----
    // 目标优先级：
    //   a) 正在攻击祭司的敌人（必须优先拉走，否则弓箭手这类远程会站着白嫖祭司）
    //   b) 离市镇中心最近的敌人
    // 关键：不能"锁定后就不换"——只要有新的敌人开始打祭司，就要切过去拉仇恨。
    bool focusAlive = false;
    for (int sn : enemies)
        if (sn == towerFocusSN) { focusAlive = true; break; }

    int priestAttacker = -1;
    for (int sn : enemies)
        if (attackingPriest(sn)) { priestAttacker = sn; break; }

    // 当前目标已经咬着祭司 → 保持不变（不要反复横跳）
    bool focusOnPriest = (towerFocusSN != -1 && attackingPriest(towerFocusSN));

    if (priestAttacker != -1 && priestAttacker != towerFocusSN && !focusOnPriest) {
        towerFocusSN = priestAttacker;          // 切到正在打祭司的敌人
    } else if (!focusAlive) {
        // 目标没了：优先挑离市镇中心最近的敌人
        // （enemy_armies 每帧被打乱，按下标取可能锁到很远的敌人，导致箭塔打空）
        double cx = -1, cy = -1;
        for (tagBuilding &b : info.buildings) {
            if (b.Type == BUILDING_CENTER) {
                cx = b.BlockDR * BLOCKSIDELENGTH;
                cy = b.BlockUR * BLOCKSIDELENGTH;
                break;
            }
        }
        towerFocusSN = -1;
        double best = 1e18;
        for (tagArmy &e : info.enemy_armies) {
            double d = (cx < 0) ? 0.0 : calDistance(cx, cy, e.DR, e.UR);
            if (d < best) { best = d; towerFocusSN = e.SN; }
        }
        if (towerFocusSN == -1) towerFocusSN = enemies[0];   // 兜底
    }

    // 所有箭塔集中攻击同一个目标（该目标即"已被吸引仇恨"的标记）。
    // 关键：只在"集火目标变了"或"每 2 秒刷新一次"时才下令。
    // 每帧重复下令会让箭塔不断重新索敌、永远打不出伤害（频繁索敌 bug）。
    {
        int refresh = 2000 / TimePerFrame;
        if (refresh < 1) refresh = 1;
        bool needOrder = (towerFocusSN != lastTowerFocusSN)
                      || (info.GameFrame - towerOrderFrame >= refresh);
        if (needOrder) {
            if (towerFocusSN != lastTowerFocusSN)
                towerAggroFrame = info.GameFrame;   // 换了新集火目标：重新计拉仇恨时间
            lastTowerFocusSN = towerFocusSN;
            towerOrderFrame = info.GameFrame;
            for (tagBuilding &tower : info.buildings) {
                if (tower.Type != BUILDING_ARROWTOWER) continue;
                if (tower.Percent < 100) continue;
                towerTargetSN[tower.SN] = towerFocusSN;
                HumanAction(tower.SN, towerFocusSN);
            }
        }
    }

    // ---- 2) 祭司转化：必须等箭塔先拉到仇恨 ----
    // 敌人被打后会优先转火箭塔（"攻击自己的第一个对象"优先级最高），
    // 所以先让箭塔开火一段时间，祭司再上去转化，否则仇恨会直接招到祭司身上。
    bool hasTower = false;
    for (tagBuilding &b : info.buildings)
        if (b.Type == BUILDING_ARROWTOWER && b.Percent >= 100) { hasTower = true; break; }

    int aggroDelay = CONVERT_AGGRO_DELAY / TimePerFrame;
    if (aggroDelay < 1) aggroDelay = 1;
    bool aggroReady = !hasTower
                   || (towerAggroFrame != 0
                       && info.GameFrame - towerAggroFrame >= aggroDelay);

    bool priestEngaged = false;
    {
        int ax = -1, ay = -1;
        if (priest != nullptr && get_defense_anchor(ax, ay))
            priestEngaged = (calDistance(priest->DR, priest->UR,
                                         ax * BLOCKSIDELENGTH,
                                         ay * BLOCKSIDELENGTH)
                             <= PRIEST_ENGAGE_RADIUS * BLOCKSIDELENGTH);
    }

    if (priest != nullptr && priest->ConvertCooldown == 0 && priestEngaged && aggroReady) {
        // 目标失效（转化成功/死亡/离开视野）→ 重新选
        if (convertTargetSN != -1) {
            bool stillEnemy = false;
            for (int sn : enemies)
                if (sn == convertTargetSN) { stillEnemy = true; break; }
            if (!stillEnemy) convertTargetSN = -1;
        }

        // 选目标打分（分越低越优先）：
        //   - 正在攻击祭司的：大幅加分（自卫优先，先把咬在祭司身上的拉下来）
        //   - 敌方投石车：一档固定加成（PRIEST_CONVERT_SIEGE_BONUS）—— 它射程 10 > 我们
        //     箭塔的 7，站在塔打不到的地方拆塔（一发 50 伤害），不先抢下来箭塔掉得太快
        //   - 其余：按距离，但超出 PRIEST_CONVERT_RADIUS 要加罚，
        //     免得祭司丢下脚边的敌人跑去追远处那个（路上还会被反杀）。
        // 不能只按"最近"排：enemy_armies 每帧都会被打乱，按下标取会选到很远的目标。
        int target = -1;
        double best = 1e18;
        const double bsl = BLOCKSIDELENGTH;      // 1 格 = 多少细节坐标
        const double nearMax = PRIEST_CONVERT_RADIUS * bsl;
        // 【用户 2026-09-24：“第二波祭司优先转化方阵兵”】
        //   窗口 = 第二波发动(9:00)到第三波(14:00)之间：那段时间上门的正是第二波
        //   （以及可能落在院里没走的残兵），方阵兵是里面最硬、抢下来最划算的一个。
        const bool inWave2 = (info.GameFrame >= WAVE2_START_FRAME
                              && info.GameFrame < ATTACK_START_FRAME);
        auto consider = [&](int sn, double dr, double ur, int sort, bool attacking) {
            if (sn == towerFocusSN) return;   // 留给箭塔继续拉仇恨
            double d = calDistance(priest->DR, priest->UR, dr, ur);
            double score = d;
            if (d > nearMax) score += 100.0 * bsl;   // 离太远：加罚，别丢下近的去追
            if (attacking)   score -= 200.0 * bsl;   // 正咬着祭司：最优先拉下来
            if (sort == AT_STONE_THROWER)            // 敌方投石车：优先抢（射程比我们塔远）
                score -= PRIEST_CONVERT_SIEGE_BONUS * bsl;
            if (inWave2 && sort == AT_HOPLITE)       // 第二波：优先抢方阵兵
                score -= PRIEST_CONVERT_PHALANX_BONUS * bsl;
            if (score < best) { best = score; target = sn; }
        };
        for (tagArmy &e : info.enemy_armies)
            consider(e.SN, e.DR, e.UR, e.Sort, attackingPriest(e.SN));
        // 实在只剩箭塔集火目标了，就转化它（总比站着不动好）
        if (target == -1) {
            for (tagArmy &e : info.enemy_armies)
                if (e.SN == towerFocusSN) { target = e.SN; break; }
        }

        // 卡住检测：有目标却 2 秒没挪过窝，说明它被自己人或建筑挡住、或目标不可达。
        // 这时清掉目标，下一帧会重新下令（等于让它重新寻路），否则祭司会永远卡在
        // 那儿一动不动——看上去就是"被挡住、不转化"。
        if (convertTargetSN != -1) {
            int interval = 2000 / TimePerFrame;
            if (interval < 1) interval = 1;
            if (convertStuckFrame == 0) {
                convertStuckFrame = info.GameFrame;
                convertStuckDR = priest->DR;
                convertStuckUR = priest->UR;
            } else if (info.GameFrame - convertStuckFrame >= interval) {
                double mDR = priest->DR - convertStuckDR; if (mDR < 0) mDR = -mDR;
                double mUR = priest->UR - convertStuckUR; if (mUR < 0) mUR = -mUR;
                if (mDR < 1.0 && mUR < 1.0
                    && priest->NowState != HUMAN_STATE_ATTACKING)
                    convertTargetSN = -1;
                convertStuckFrame = info.GameFrame;
                convertStuckDR = priest->DR;
                convertStuckUR = priest->UR;
            }
        } else {
            convertStuckFrame = 0;
        }

        // 目标变了、或祭司空闲（上一条指令已完成）时才重新下令；
        // 否则不要每帧重下，免得打断正在进行的转化
        if (target != -1
            && (target != convertTargetSN
                || priest->NowState == HUMAN_STATE_IDLE)) {
            convertTargetSN = target;
            convertStuckFrame = info.GameFrame;
            convertStuckDR = priest->DR;
            convertStuckUR = priest->UR;
            HumanAction(priest->SN, target);
        }
    }

    // ---- 3) 我方士兵主动迎战 ----
    // 【2026-09-23 的两个限制】原版是“每个兵都去就近攻击最近的**可见**敌人”，
    //   既没有距离限制、也不节流。后果很硬：
    //   ① **会把反攻部队整支劫走**。`bt_enemy_at_home()` 的半径是 35 格，
    //      反攻途中只要家里 35 格内出现一个敌人，全军（包括在敌营外围的）都被改成
    //      “去追最近的可见敌人”；而 `demand_attack` 对“同编码且非空闲”的单位是
    //      `continue`（不重下）的，**根本盖不回来** —— 下一帧那单位已变成 ATTACKING，
    //      两边都跳过它。看起来就是“勾引到一半部队自己跑了”。
    //   ② **每帧重下**：内核每次 addRelation 都 suspendRelation + 清路径，
    //      “走过去 → 攻击”的蓄力永远走不完（这个坑本文件里已经踩过多次）。
    // 所以：只指挥**真的在家附近**的兵（离家超过 HOME_DEFEND_RADIUS 的交给
    // demand_attack 的反攻状态机），并且每个兵最多 DEFENSE_ORDER_MS 催一次。
    double hDR = 0, hUR = 0;
    const bool haveHome = home_center(hDR, hUR);
    int defenseGap = DEFENSE_ORDER_MS / TimePerFrame;
    if (defenseGap < 1) defenseGap = 1;
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) continue;
        if (a.Sort == AT_SCOUT) continue;
        if (a.NowState == HUMAN_STATE_ATTACKING) continue;

        // ① 离家太远的兵不归这里管（它们是反攻部队）
        if (!haveHome
            || calDistance(a.DR, a.UR, hDR, hUR)
               > HOME_DEFEND_RADIUS * BLOCKSIDELENGTH) continue;
        // ② 节流
        if (info.GameFrame - defenseOrderFrame[a.SN] < defenseGap) continue;

        int target = -1;
        double best = 1e18;
        for (tagArmy &e : info.enemy_armies) {
            double d = calDistance(a.DR, a.UR, e.DR, e.UR);
            if (d < best) { best = d; target = e.SN; }
        }
        if (target != -1) {
            HumanAction(a.SN, target);
            defenseOrderFrame[a.SN] = info.GameFrame;
        }
    }
}

// ==================== 行为树节点实现 ====================
BTStatus BTSelector::tick(BTContext &ctx)
{
    for (BTNodePtr &c : children) {
        BTStatus s = c->tick(ctx);
        if (s != BTStatus::Failure) return s;   // Success / Running 都直接返回
    }
    return BTStatus::Failure;
}

BTStatus BTSequence::tick(BTContext &ctx)
{
    for (BTNodePtr &c : children) {
        BTStatus s = c->tick(ctx);
        if (s != BTStatus::Success) return s;
    }
    return BTStatus::Success;
}

BTStatus BTLeaf::tick(BTContext &ctx)
{
    if (cond && !cond(ctx)) return BTStatus::Failure;                 // 条件不满足
    if (action) return action(ctx) ? BTStatus::Success : BTStatus::Failure;
    return BTStatus::Success;                                          // 仅条件且通过
}

// ---------- 叶子行为 ----------
// 指定点半径内是否有可见敌军（用于"敌军是否已逼近某处"的判定）
bool enemy_near(double dr, double ur, double radius)
{
    for (tagArmy &e : info.enemy_armies)
        if (calDistance(dr, ur, e.DR, e.UR) <= radius) return true;
    return false;
}

// 敌方是否已逼近我方城市：以已建成的市镇中心为圆心、HOME_DEFEND_RADIUS 格内出现可见敌军。
bool bt_enemy_at_home()
{
    for (tagBuilding &b : info.buildings) {
        if (b.Type == BUILDING_CENTER && b.Percent >= 100) {
            double hDR = b.BlockDR * BLOCKSIDELENGTH;
            double hUR = b.BlockUR * BLOCKSIDELENGTH;
            return enemy_near(hDR, hUR, HOME_DEFEND_RADIUS * BLOCKSIDELENGTH);
        }
    }
    return false;   // 中心尚未建成（异常）：不触发防御
}

// ---------- 构建行为树 ----------
void build_behavior_tree()
{
    auto leaf = [](const char *name,
                   std::function<bool(BTContext&)> cond,
                   std::function<bool(BTContext&)> action) -> BTNodePtr {
        auto n = std::make_shared<BTLeaf>();
        n->btName = name;
        n->cond = cond;
        n->action = action;
        return n;
    };
    auto seq = [](std::initializer_list<BTNodePtr> cs) -> BTNodePtr {
        auto n = std::make_shared<BTSequence>();
        n->btName = "seq";
        n->children.assign(cs.begin(), cs.end());
        return n;
    };
    auto sel = [](std::initializer_list<BTNodePtr> cs) -> BTNodePtr {
        auto n = std::make_shared<BTSelector>();
        n->btName = "sel";
        n->children.assign(cs.begin(), cs.end());
        return n;
    };

    // root = Sequence(
    //   sync     : 阶段推进 + 回收任务
    //   defense  : Selector —— 敌方逼近我方城市时「箭塔拉仇恨 + 祭司转化 + 士兵迎战」，否则跳过
    //   build    : 建造需求（房屋 / 冲铜器链 / 靶场 / 农田 / 箭塔）
    //   standby  : 第 1/2 阶段的军队村外待命点（不占村子里的路）
    //   produce  : 生产村民
    //   army     : 第二阶段造兵（方阵兵 / 骑兵 / 弓箭手 / 棍棒兵）
    //   research : 第二阶段科技研发（市场 / 靶场 / 谷仓 / 仓库）
    //   scout    : 探图（祭司，第三阶段停止）
    //   attack   : 第三阶段反攻 + 祭司转化敌方武器工程厂
    //   repair   : 修塔（前两波打完之后派 1 个村民去修最惨的那座塔）
    //   dispatch : 任务排序 + 派发（sort_tasks + assign_tasks）
    //   gather   : 采集需求（食物 / 木 / 石 / 金 / 打猎）
    //              **必须排在 dispatch 之后**：它末尾的"兜底 2"是直接给村民下指令的，
    //              排在前面会抢走本该去建造的村民（详见下面 leaf 旁的说明）
    // )
    btRoot = seq({
        leaf("sync", nullptr,
             [](BTContext &) { bt_sync(); return true; }),

        sel({
            seq({
                leaf("enemy_at_home", [](BTContext &) { return bt_enemy_at_home(); }, nullptr),
                leaf("defense",        nullptr, [](BTContext &) { combat_tactic(); return true; })
            }),
            leaf("no_threat", nullptr, [](BTContext &) { return true; })
        }),

        leaf("build",    nullptr, [](BTContext &) { demand_build();   return true; }),
        // 【军队待命点必须排在 defense 之后】combat_tactic 已经下过攻击指令的那一帧，
        //   这里再下移动指令会把同一单位的攻击指令顶掉（内核只保留每个 self 的最后一条）。
        //   所以我们自己用 bt_enemy_at_home() 挡住 —— 家里有敌人时整个函数直接 return。
        leaf("standby",  nullptr, [](BTContext &) { army_standby();   return true; }),
        leaf("produce",  nullptr, [](BTContext &) { demand_produce(); return true; }),
        leaf("army",     nullptr, [](BTContext &) { demand_army();    return true; }),
        leaf("research", nullptr, [](BTContext &) { demand_research();return true; }),
        leaf("scout",    nullptr, [](BTContext &) { demand_scout();   return true; }),
        leaf("attack",   nullptr, [](BTContext &) { demand_attack();  return true; }),
        // 【修塔必须排在 dispatch **之前**】dispatch 里的 assign_tasks 会把**所有**
        //   空闲村民都派出去采集，排在它后面就一个村民都抢不到；而我们在派活后立刻
        //   mark_farmer_order()，所以同一帧稍后的 assign_tasks / 兜底 2 不会把他抢走。
        leaf("repair",   nullptr, [](BTContext &) { demand_repair();  return true; }),
        leaf("dispatch", nullptr, [](BTContext &) { bt_dispatch();    return true; }),
        // 【gather 必须排在 dispatch **之后**】用户 2026-09 两轮反馈的结论：
        //   demand_gather 末尾的“兜底 2”是用 HumanAction **直接**给村民下采集指令的。
        //   · 排在 dispatch 前面 → 把本该去建造的村民抢走（“拍了建筑不建”）；
        //   · 排到后面 → assign_tasks 已经先把正经任务派完，这里拿到的才是
        //     **真正剩下**的空闲村民，两边都不抢。
        //   代价：本帧新建的采集任务要等下一帧的 assign_tasks 才派出去（延迟 1 帧，无妨）。
        leaf("gather",   nullptr, [](BTContext &) { demand_gather();  return true; })
    });
    btRoot->btName = "root";
}

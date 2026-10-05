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
    bool granaryFarm = false; // 反攻新增农田，仅在谷仓周围选址
    int buildingType = -1;    // 建造/生产：建筑类型
    int blockDR = -1, blockUR = -1; // 建造位置
    int farmerSN = -1;        // 被分配的农民，-1 未分配
    int startFrame = 0;       // 分配帧号，用于超时
    int resendFrame = 0;      // 上次续建重发的帧号
    int progressFrame = -1, progressPercent = -1;
    int gatherProgressResource = -1;
    double progressDR = 0, progressUR = 0;
    std::unordered_map<int,int> badBuilders;
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
static std::set<int> granaryFarmSites; // (块DR << 12) | 块UR
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
static bool scoutSeenAlive = false;                  // 已见到活着的侦察兵
static bool scoutDeathPending = false;               // 阵亡位置等待反攻阶段使用

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
struct DefenseDiagOrder {
    int id, sn, target, frame;
    bool priest;
};
static std::vector<DefenseDiagOrder> defenseDiagOrders;
static std::unordered_map<int,int> defenseDiagBlood;
static int defenseDiagFrame = -1000000;

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
static bool assaultPriestFlee = false;
static int assaultPriestSafeFrame = 0;
static int assaultPriestBlood = -1;
static int attackConvertSN = -1;                     // 祭司在反攻阶段正在转化的目标 SN

// ---- 第三阶段反攻状态机 ----
static int    assaultState = 0;           // 1 集结，2 诱敌，3 拆塔，4 转化工程厂
static int    assaultStageFrame = 0;      // 进入当前状态的帧
static int    enemyClearSinceFrame = 0;

static std::unordered_map<int,int>    enemyLedgerSeenFrame;   // SN → 最后见到的帧
static std::unordered_map<int,double> enemyLedgerDR;         // SN → 最后见到的位置
static std::unordered_map<int,double> enemyLedgerUR;
static std::unordered_map<int,char>   enemyLedgerDead;       // SN → 1 = 已确认消灭

static int    baitScoutSN = -1;                     // 当前突击者 SN（-1 = 没有）
static double baitScoutHomeDR = 0, baitScoutHomeUR = 0;   // 突击者的初始位置
static int    baitScoutFrame = 0;                   // 上次给它下移动指令的帧（节流）
static int    baitScoutBlood = -1;                  // 上一帧血量（掉血 = 被攻击）
static bool   baitScoutReturning = false;
// 摘标志后的冷却帧：这段时间不选新突击者（防“刚摘就又抓同一个”）。
static int    baitScoutSwitchFrame = 0;
static const char *baitScoutReason = "待选";
static bool   baitScoutBack = false;
static double slowPushDR = 0, slowPushUR = 0;       // 队伍推进点（“主动进攻”逐步前压的目标）
static bool   slowPushValid = false;
static double lastAdvanceDR = -1, lastAdvanceUR = -1;
static bool   enemyFarFound = false;      // 是否已记下"100 格外的敌方目标"
static double enemyFarDR = 0, enemyFarUR = 0;

// ---- 前线集结区（7x7）的缓存：家 → 敌营 连线上那块空地 ----
static int    rallyBX = -1, rallyBY = -1;    // 集合点中心（块）；-1 = 还没找到
static int    rallyHalf = 0;                 // 铺开半径（rally_point 里按 RALLY_HALF 赋值）
static int    rallyFrame = -1000000;         // 上次重算的帧
static double rallyAnchorDR = 0, rallyAnchorUR = 0;  // 上次算法用的敌营位置
static int    assaultLogFrame = 0;        // 反攻状态日志的上次输出帧
static int factoryConvertFrame = -1000000;
static int factoryConvertId = -1;
static int factoryConvertTarget = -1;
static int assaultFocusTower = -1;
static int siegePositionFrame = -1000000;
struct SiegePositionSample { double dr, ur; int blood; };
static std::unordered_map<int, SiegePositionSample> siegePositionSamples;
static std::unordered_map<int, std::string> siegeOrderReason;
static std::unordered_map<int,int> siegeLastBlood, siegeFleeUntil, siegeAttackId;
struct SiegeDiagOrder { int id, sn, target, frame; };
static std::vector<SiegeDiagOrder> siegeDiagOrders;
static std::unordered_map<int, std::string> siegeDiagContext;

static int    siegePriestStuckFrame = 0;  // 转化阶段的祭司卡住检测
static double siegePriestStuckDR = 0, siegePriestStuckUR = 0;
static int    weakKillFrame = 0;          // 自裁弱兵的上次执行帧
static int    weakKillSN = -1;
static std::unordered_map<int,int> unitStuckKey;    // 单位 SN → 上次采样的位置（打包）
static std::unordered_map<int,int> unitStuckFrame;  // 单位 SN → 上次采样的帧号
static std::unordered_map<int,int> unitNeedsRecovery;
static std::unordered_map<int,int> unitStepFrame;
static std::unordered_map<int,int> unitFireFrame;
// 单位 SN 非 0 = 这个弓箭手正在“脱离接触”（拉扯的滞回状态，见 kite_archer_step）。
//   只按“距离 < 5 就退一步”判会来回抖：退一步后距离回到 9，下一帧立刻重下攻击指令，
//   内核又把人走回来 —— 净位移 0、还每次 suspendRelation 清路径，最后被追上。
static std::unordered_map<int,char> kiteRetreating;
// 单位 SN → 上次“家里迎战”下令帧（combat_tactic 第 3 节的节流）。
static std::unordered_map<int,int> defenseOrderFrame;
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
static std::unordered_map<long long,int> gatherBlockedUntil;
static std::unordered_map<int,int> bowTowerMoveTarget;
static std::unordered_map<int,int> escapeFrame;       // 村民危险撤离的上次下令帧

static int MAP[505][505] = {{0}};              // 建造占位图（>0 = 占用）
static std::unordered_map<int,int> badBuildSite;  // 被内核驳回过的建造位置（拉黑表）

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
static void update_enemy_ledger();
static int  enemy_remains();
static int  enemy_ledger_total();
static bool center_free();
static void demand_build();
static double nearest_dropoff_dist(int resType, double dr, double ur);
static bool hunt_dropoff_ready();
static void demand_produce();
static void demand_repair();
static void demand_gather();
static int resource_type_by_sn(int sn);
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

// 敌方第三波发动帧（约 14 分钟，默认 25fps → 21000 帧），之后转入反攻
static const int ATTACK_START_FRAME = 21000;

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
static const int SCOUT_RING_MAX = 40;     // 祭司探图的环半径上限（格）

static const int SCOUT_RING_START = 10;
static const int SCOUT_RING_STEP = 8;

static const int SCOUT_ARC_SPACING = 8;

// ---------------- 侦察骑兵：后期"找敌军大本营" ----------------
static const int SCOUT_DFS_RANGE = 40;      // 找前沿的搜索半径（格）
static const int SCOUT_DFS_MIN   = 2;       // 比这还近的前沿不选（避免原地抖）。
                                            // **别调大**：侦察兵贴在前沿边上时，
                                            // 正前方的新前沿就在 3~6 格外，调大就把
                                            // "继续往前扎"否掉了，变成只能沿边走。
static const double SCOUT_DFS_HEAD_W = 2.0; // "继续朝当前方向"权重
static const double SCOUT_DFS_FAR_W  = 1.0; // "越远越好"权重
static const double SCOUT_DFS_BACK_W = 4.0;
static const int SCOUT_BAD_MS = 30000;      // 走不到的目标拉黑多久（毫秒）
static const double SCOUT_BACK_HOME_PENALTY = 0.5;  // 朝家方向降权

static const int SCOUT_GOAL_NEAR = 30;

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

static const int BOWMAN_PRE_TECH_MAX = 2;

static const int TECH_FOOD_RESERVE = BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_FOOD + 50;
static const int TECH_WOOD_RESERVE = 100;

// ============ 第三阶段反攻：集结 → 诱杀野战军 → 齐射拆箭塔 → 祭司转化 ============
static const int ASSAULT_BOWMAN_MIN = 16;
static const int ASSAULT_FORCE_MIN = 23;
// 勾引线：踩进警戒半径 1 格。守军必须**自己走出来**才能打到我们 —— 一走就离开塔的掩护。
// ---- 拉锯的两段时长（毫秒）----
static const int RALLY_DIST = 14;
// ---- 前线集合点（村庄外 RALLY_AWAY_DIST 格）----
static const int RALLY_AWAY_DIST    = 40;
static const int RALLY_CLEAR_HALF   = 3;
static const int RALLY_HALF         = 2;
static const int RALLY_ENEMY_MARGIN = 24;

static const int RALLY_SIDE_MAX      = 10;
static const int RALLY_OPEN_GOOD     = 20;
static const int RALLY_OPEN_OK       = 12;
// ---- 第 1/2 阶段（第三波之前）军队的村外待命点（见 army_standby）----
static const int ARMY_STANDBY_DIST = 20;
// 待命区的铺开半径（7x7 → 3），一个格子一个兵，由 rally_slot_offset 从中心往外排。
static const int ARMY_STANDBY_HALF = 3;
static const int ARMY_STANDBY_SEARCH_R = 12;
static const int ASSAULT_STAGE_GUARD_DIST = 7;  // 集结期唯一允许还手的距离（格）
static const int ASSAULT_BACKSTEP = 4;          // 集结点落在塔射程里时每次后退几格
static const double TOWER_SAFE_MARGIN = 3.0;

static const int ENEMY_DIS_ADD_TOWER   = 3;
static const int ASSAULT_TOWER_RADIUS = 24;     // 敌营这个范围内的箭塔要拆掉
static const int ASSAULT_ENGAGE_DIST = 20;      // 单位主动交战的半径
static const int ASSAULT_ROLLBACK_MIN     = 3;
static const int ASSAULT_TOWER_COMMIT_MS  = 15000;
static const int SIEGE_BACK_DIST          = 2;
static const int ASSAULT_SPREAD_HALF = 2;
static const int SPREAD_EXTRA_RINGS = 3;        // 最多往外多扩 3 圈（2+3=5 → 11x11）
static const int ASSAULT_PROTECT_DIST = 8;      // 转化阶段部队停在敌营外几格（护祭司但不挡路）
static const int ASSAULT_HUNTER_WAIT_MS = 60000;
static const int ASSAULT_CLEAR_CONFIRM_MS = 8000;
// 敌方兵力账本的两个参数（定义与用法见文件头部 enemyLedger* 那一段）
static const int ENEMY_DEAD_CONFIRM_MS = 6000;   // 连续多久没再见到才考虑“已消灭”
static const int ENEMY_DEAD_SIGHT_DIST = 12;     // 我方要有人站到它最后位置这么近才算“看着那儿”
// ---- 诱敌突击者的参数（见文件头部 baitScout*/slowPush* 的说明）----
static const int BAIT_SCOUT_STEP         = 1;    // 突击者每次朝敌营挪几格
static const int BAIT_SCOUT_GAP          = 20;   // 两次移动指令的最小帧间隔（防内核反复清路径）
static const int SLOW_PUSH_STEP          = 2;    // 突击者每挪一步，其余部队推进几格
static const int BAIT_SCOUT_SWITCH_MS    = 5000; // 摘掉突击者标志后多久才能再选一个
static const int WEAK_KILL_INTERVAL_MS = 3000;  // 自裁弱兵的节流（毫秒）

// ---- 远程兵的行动节流参数（旧的“控距开火” 已整体删除）----
//   威胁是敌方弓兵 → 撤到 它射程+5+2 格；威胁是箭塔 → 撤到 17 格外。

static const int    RANGED_STEP_GAP       = 20;   // 同一单位两次战斗位移的最小帧间隔
static const int    RANGED_FIRE_GAP       = 25;   // 同一单位重复下攻击指令的最小帧间隔

static const int KITE_FLEE_DIST    = 5;   // 离最近敌人小于这个距离就进入“脱离”（格）
static const int KITE_RETREAT_STEP = 4;   // 每次脱离朝家退几格

// 让箭塔先拉仇恨、再让祭司转化：塔开火后等这么久（毫秒）祭司才动手。
static const int CONVERT_AGGRO_DELAY = 1500;

static const int PRIEST_ENGAGE_RADIUS = 30;
// 家里士兵迎战的下令节流（毫秒）。防“每帧重下”把攻击蓄力打断，见 combat_tactic 第 3 节。
static const int DEFENSE_ORDER_MS = 1000;

// 祭司优先转化的距离（块）：这个范围内的敌人能立刻上手，
// 超出后要给大额惩罚，免得祭司丢下脚边的敌人去追远处的（路上还会被反杀）。
static const int PRIEST_CONVERT_RADIUS = 12;
static const int PRIEST_HUNTER_FLEE_DIST = 10;
static const int PRIEST_HUNTER_RESUME_DIST = 13;

static const double PRIEST_CONVERT_SIEGE_BONUS = 150.0;
static const double PRIEST_CONVERT_PHALANX_BONUS = 180.0;

static const int PRIEST_HEAL_UNTIL_MIN = 13;   // 分钟

// 治疗的追击上限：只治离祭司这个距离以内的伤兵，别为了回血跑遍全图。
static const int HEAL_MAX_DIST = 25;           // 块

static const int HEAL_MAX_HOME_DIST = 20;      // 块

static const int SCOUT_HOME_CALL_RADIUS = 30;

// 祭司靠到防御锚点（箭塔/市中心）这个距离以内就算"已到位"，不再下移动指令。
// 不能用"距落脚点 <4 格"判定：塔边常挤满村民，祭司到不了那个精确格子，
// 会反复换点、在塔边来回徘徊。
static const int HOME_STAY_RADIUS = 6;

// ============ 开局经济：6 人采浆果 + 其余全砍树 + 先砍树后打猎 ============
static const int FOOD_GATHERERS = 6;

static const int HUNT_START_POP  = 8;
static const int HUNT_START_WOOD = 150;

// 开闸后打猎人数占村民总数的百分比（农田优先，剩下的名额才给打猎）。
static const int HUNT_PERCENT = 30;

static const int HUNT_WOOD_SURPLUS   = 150;   // 自由木头超过这个数就算富余
static const int HUNT_SURPLUS_EXTRA  = 2;     // 富余时额外多派几个人去打猎
static const int HUNT_EARLY = 2;
static const int HUNT_MAX_GATHERERS  = 6;     // 打猎人数上限
static const int HUNT_EARLY_TRIM    = 2;

static const int WOOD_MAX_GATHERERS = 6;
static const int WOOD_MAX_GATHERERS_EARLY = 8;

// ============ 农田：后期食物主力 ============
static const int FARM_PER_POP = 3;   // 每多少个村民配 1 块农田
static const int FARM_MAX = 11;
static const int FARM_TARGET_LATE = 7;
static const int GRANARY_FARM_EXTRA = 4;
static const int GRANARY_FARM_PITCH = 5;
static const int FARM_CAP_TOOL   = 2;   // 工具时代上限
static const int FARM_CAP_BRONZE = 4;   // 铜器时代~14:00 上限

// ============ 后期分工 ============
// 木材保底人数：后期食物为主，但房屋 / 补仓库 / 农田本身 / 科技都还要木头。
// 名额不够时按"打猎 → 农田"的顺序往回缩。
static const int WOOD_MIN_GATHERERS = 4;

static const int CENTER_BUILD_START_R = 8;

static const int BUILD_GRID_PITCH = 4;

static const int HUNT_DROP_RADIUS = 10;

// "食物断供"的例外：浆果吃光、又没有农田时只能靠打猎续命，
// 此时不必等人口/木头门槛；但开局这段帧内不启用，保证"开局不杀瞪羚"。
static const int HUNT_STARVE_MIN_FRAME = 750;   // ≈30 秒（默认 25fps）

static const int BUSH_NEAR_RADIUS = 22;

static const int TOWER_TARGET_EARLY  = 1;
static const int TOWER_TARGET_BRONZE = 2;
static const int TOWER_TARGET_LATE   = 3;
static const int TOWER_LATE_MIN      = 8;   // 第几分钟开始补最后那座（二三波之间）
static const int STONE_KEEP_MAX      = 400;
static const int STONE_STOP_MIN = 15;     // 游戏分钟：到这之后禁止采石
static const int STONE_SWEEP_MS = 2000;   // 已经在矿上的人，多久扫一遍把他们调走（毫秒）
static const int GOLD_KEEP_MAX       = 300;

// ---- 箭塔的三波时间线（enemyai.cpp：FAT=6000(4:00) / SAT=13500(9:00) / TAT=21000(14:00)）----
static const int TOWER_DEADLINE_MIN       = 14;   // 第三波到达时刻（= ATTACK_START_FRAME）
static const int TOWER_DEADLINE_GUARD_MIN = 3;    // 提前几分钟开始强行催塔（14-3 = 11:00）
static const int REPAIR_FROM_MIN          = 11;   // 第几分钟起允许修塔（第二波 9:00 + 余量）
static const int REPAIR_ORDER_INTERVAL_MS = 2000;  // 修塔指令节流（重下会 suspendRelation、修理从头开始）

static const int RANGE_MAX = 3;

static const int PHASE3_POP_TARGET = 50;

static const int POP_TARGET_EARLY = 28;

static const int SCOUT_UNITS = 1;

static const int SCOUT_BUILD_MIN = 12;

static const int GATHER_PER_RESOURCE_MAX = 3;

static const int GATHER_MAX_DIST = 35;

static const int GATHER_MAX_DIST_ORE = 35;

static const int GATHER_DANGER_RADIUS = 14;
static const int GATHER_DANGER_RADIUS_ORE = 6;

static const int STONE_WINDOW_FROM_MIN = 10;   // 分钟（含）
static const int STONE_WINDOW_TO_MIN   = 13;   // 分钟（不含）
static const int STONE_WINDOW_WORKERS  = 3;    // 窗口内固定几个采石工

static bool in_stone_window()
{
    const int tpf = (TimePerFrame > 0) ? TimePerFrame : 40;
    const long long ms = (long long)info.GameFrame * tpf;
    return ms >= (long long)STONE_WINDOW_FROM_MIN * 60000
        && ms <  (long long)STONE_WINDOW_TO_MIN   * 60000;
}

void UsrAI::processData()
{
    g_ai = this;   // 唯一实例：供下面那些自由函数转发基类调用（HumanMove / calDistance / DebugText ...）

    info = getInfo();

    // 首次进入时构建行为树
    if (!btRoot) build_behavior_tree();

    btCtx.info = &info;

    btRoot->tick(btCtx);

    auto repairActionSubject = [&](instruction &cur) {
        if (cur.type != INS_HUMANACTION || cur.self != nullptr) return;
        if (cur.SN < 0) return;
        // 仅构造取主体，不提交移动，也不改变攻击目标和指令编号。
        const instruction probe(INS_HUMANMOVE, cur.SN, Double::Zero(), Double::Zero());
        cur.self = probe.self;
        DebugText(std::string("单位指令修复: SN=") + std::to_string(cur.SN)
            + " id=" + std::to_string(cur.id) + " 目标=" + std::to_string(cur.obSN)
            + " 空主体已补=" + std::to_string(cur.self != nullptr));
    };
    for (instruction &cur : InsPerFrame) repairActionSubject(cur);
    UsrIns.lock.lock();
    const size_t pendingCount = UsrIns.instructions.size();
    for (size_t i = 0; i < pendingCount; ++i) {
        instruction cur = UsrIns.instructions.front(); UsrIns.instructions.pop();
        repairActionSubject(cur); UsrIns.instructions.push(cur);
    }
    UsrIns.lock.unlock();

    // 同时检查直接入队与基类暂存两种提交路径，兼容 OJ 的接口实现。
    if (!siegeDiagOrders.empty() && siegeDiagOrders.back().frame == info.GameFrame) {
        UsrIns.lock.lock();
        auto pending = UsrIns.instructions;
        UsrIns.lock.unlock();
        const int sharedQueueSize = (int)pending.size();
        const int bufferedQueueSize = (int)InsPerFrame.size();
        for (const instruction &cur : InsPerFrame) pending.push(cur);
        std::unordered_map<int, instruction> last;
        std::unordered_map<int, int> count;
        std::set<int> presentIds, objects;
        const int queueSize = (int)pending.size();
        while (!pending.empty()) {
            const instruction cur = pending.front(); pending.pop();
            presentIds.insert(cur.id);
            if (cur.self != nullptr) objects.insert(cur.SN);
            for (const tagArmy &a : info.armies) {
                if (a.Sort != AT_STONE_THROWER || a.SN != cur.SN) continue;
                last[cur.SN] = cur; ++count[cur.SN]; break;
            }
        }
        for (const SiegeDiagOrder &order : siegeDiagOrders) {
            if (order.frame != info.GameFrame) continue;
            const auto found = last.find(order.sn);
            std::string details = " 无同单位指令";
            if (found != last.end()) {
                const instruction &cur = found->second;
                const bool attacking = cur.type == INS_HUMANACTION;
                const int rank = (int)std::distance(objects.begin(), objects.lower_bound(cur.SN)) + 1;
                details = " 同单位条数=" + std::to_string(count[cur.SN])
                    + " 最后id=" + std::to_string(cur.id) + " 最后类型=" + std::to_string(cur.type)
                    + " 最后目标=" + (attacking ? std::to_string(cur.obSN) : "非攻击")
                    + " 主体非空=" + std::to_string(cur.self != nullptr)
                    + " 主体匹配=" + std::to_string(cur.self == g_Object[cur.SN])
                    + " 目标非空=" + (attacking ? std::to_string(cur.obj != nullptr) : "不适用")
                    + " 目标匹配=" + (attacking ? std::to_string(cur.obSN >= 0 && cur.obj == g_Object[cur.obSN]) : "不适用")
                    + " 去重后序位=" + std::to_string(rank);
            }
            DebugText(std::string("投石车队列核对: SN=") + std::to_string(order.sn)
                + " 请求id=" + std::to_string(order.id)
                + " 仍在队列=" + std::to_string(presentIds.count(order.id))
                + " 共享队列=" + std::to_string(sharedQueueSize)
                + " 基类暂存=" + std::to_string(bufferedQueueSize)
                + " 队列总数=" + std::to_string(queueSize)
                + " 有效主体数=" + std::to_string(objects.size()) + details);
        }
    }
}

// 建筑建造模块

bool find_block(int x,int y,int dx,int dy){
    if (info.theMap == nullptr) return 0;
    int w = (int)info.theMap->size();
    if (w == 0) return 0;
    int ht = (int)(*info.theMap)[0].size();
    if (x < 0 || y < 0 || x + dx > w || y + dy > ht) return 0;
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
    static int lastSeenGameFrame = 0;
    if (info.GameFrame < lastSeenGameFrame) {
        factoryConvertFrame = -1000000; factoryConvertId = -1; factoryConvertTarget = -1;
        assaultFocusTower = -1; bowTowerMoveTarget.clear(); gatherBlockedUntil.clear();
        assaultPriestFlee = false; assaultPriestSafeFrame = 0; assaultPriestBlood = -1;
        unitNeedsRecovery.clear();
        siegePositionFrame = -1000000; siegePositionSamples.clear(); siegeOrderReason.clear(); siegeLastBlood.clear(); siegeFleeUntil.clear(); siegeAttackId.clear(); siegeDiagOrders.clear(); siegeDiagContext.clear();
        defenseDiagOrders.clear(); defenseDiagBlood.clear();
        defenseDiagFrame = -1000000;
        assaultState = 0; assaultStageFrame = 0;
        enemyClearSinceFrame = 0;
        baitScoutSN = -1; baitScoutReturning = false; baitScoutSwitchFrame = 0;
        baitScoutBlood = -1; baitScoutBack = false; slowPushValid = false;
        lastAdvanceDR = -1; lastAdvanceUR = -1;
        granaryFarmSites.clear();
        scoutSeenAlive = false; scoutDeathPending = false;
        scoutLastDR = 0; scoutLastUR = 0;
        enemyFarFound = false; enemyFarDR = 0; enemyFarUR = 0;
        enemySiegeSN = -1; enemySiegeDR = -1; enemySiegeUR = -1;
        enemyLedgerSeenFrame.clear(); enemyLedgerDR.clear();
        enemyLedgerUR.clear(); enemyLedgerDead.clear();
        baitScoutReason = "待选";
    }
    lastSeenGameFrame = info.GameFrame;

    for (auto it = siegeDiagOrders.begin(); it != siegeDiagOrders.end();) {
        const auto ret = info.ins_ret.find(it->id);
        const bool timeout = (info.GameFrame - it->frame) * std::max(1, TimePerFrame) >= 2000;
        if (ret == info.ins_ret.end() && !timeout) { ++it; continue; }
        DebugText(std::string("投石车回执: SN=") + std::to_string(it->sn)
            + " id=" + std::to_string(it->id) + " 目标=" + std::to_string(it->target)
            + " ret=" + (ret == info.ins_ret.end() ? "未收到(2s)" : std::to_string(ret->second))
            + " 延迟ms=" + std::to_string((info.GameFrame - it->frame) * std::max(1, TimePerFrame)));
        it = siegeDiagOrders.erase(it);
    }

    for (auto it = defenseDiagOrders.begin(); it != defenseDiagOrders.end();) {
        const auto ret = info.ins_ret.find(it->id);
        const bool timeout = info.GameFrame - it->frame >= 2000 / TimePerFrame;
        if (ret == info.ins_ret.end() && !timeout) { ++it; continue; }
        DebugText(std::string("守家诊断回执: 类型=") + (it->priest ? "转化" : "塔攻击")
            + " id=" + std::to_string(it->id) + " SN=" + std::to_string(it->sn)
            + " 目标=" + std::to_string(it->target)
            + " ret=" + (ret == info.ins_ret.end() ? std::string("未收到(2s)") : std::to_string(ret->second))
            + " 延迟帧=" + std::to_string(info.GameFrame - it->frame));
        it = defenseDiagOrders.erase(it);
    }

    if (info.civilizationStage < CIVILIZATION_BRONZEAGE) {
        phase = 1;   // 开局即工具时代，直接冲铜器
    } else if (info.GameFrame >= ATTACK_START_FRAME) {
        phase = 3;   // 第三波之后：组织反攻
    } else {
        phase = 2;   // 铜器时代：发展军事、防守三波
    }

    // 全程跟踪侦察兵；敌营定位只在反攻阶段使用。
    record_enemy_positions();
    update_enemy_ledger();

    const int towerLateFrame = (int)(TOWER_LATE_MIN * 60 * 1000.0 / TimePerFrame);
    if (phase >= 2) arrowTowerTarget = TOWER_TARGET_BRONZE;
    else            arrowTowerTarget = TOWER_TARGET_EARLY;
    if (phase >= 2 && info.GameFrame >= towerLateFrame)
        arrowTowerTarget = TOWER_TARGET_LATE;
    if (phase >= 3) arrowTowerTarget = 0;

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
        if (phase >= 3) {
            farmTarget = FARM_TARGET_LATE + GRANARY_FARM_EXTRA;
        } else {
            const int cap = (phase >= 2) ? FARM_CAP_BRONZE : FARM_CAP_TOOL;
            if (farmTarget > cap) farmTarget = cap;
        }
        if (farmTarget > FARM_MAX) farmTarget = FARM_MAX;
    }

    // 农田绑定表：清掉"田没了（采完被内核删）"或"人没了"的条目
    prune_farm_holders();

    for (Task &t : taskQueue)
        if (t.startFrame == 0) t.startFrame = info.GameFrame;

    recycle_tasks();
}

// 清理 farmHolder：田不存在/已采完，或农民已阵亡，就解除绑定。
// 必须做——否则被删掉的田会永远占着一条绑定，后续永远匹配不上。
void prune_farm_holders()
{
    if (farmHolder.empty()) return;

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

int wood_gather_limit()
{
    int cap = wood_capacity();
    if (cap <= 0) return WOOD_MIN_GATHERERS;

    int lim = (phase >= 3) ? WOOD_MAX_GATHERERS : WOOD_MAX_GATHERERS_EARLY;
    if (cap < lim) lim = cap;          // 物理上限：全图树林站不下这么多人
    return lim;
}

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

static int edge_distance(int blockDR, int blockUR)
{
    if (blockDR < 0 || blockUR < 0 || blockDR >= MAP_L || blockUR >= MAP_U) return 0;
    int d = blockDR;                                          // 到 DR=0 那条边
    if (MAP_L - 1 - blockDR < d) d = MAP_L - 1 - blockDR;
    if (blockUR < d) d = blockUR;                              // 到 UR=0 那条边
    if (MAP_U - 1 - blockUR < d) d = MAP_U - 1 - blockUR;
    return d;
}

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

bool res_too_far(int type, int blockDR, int blockUR)
{
    int limit = (type == RESOURCE_STONE || type == RESOURCE_GOLD)
                ? GATHER_MAX_DIST_ORE : GATHER_MAX_DIST;
    if (type == RESOURCE_STONE && in_stone_window()) return false;
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
    int woodBudget = info.Wood;
    if (compositeBowUrgent()) {
        woodBudget -= TECH_WOOD_RESERVE;
        if (woodBudget < 0) woodBudget = 0;
    }

    const int rangeReserve =
        ((phase >= 3
          && count_done(BUILDING_RANGE) + active_build(BUILDING_RANGE) < RANGE_MAX)
         ? BUILD_RANGE_WOOD : 0);
    const int optionalWood = (woodBudget > rangeReserve) ? (woodBudget - rangeReserve) : 0;
    // ---- 房屋：按"目标人口"提前补，别让人口上限卡住村民生产与造兵 ----
    {
        const int targetPop = (phase >= 3) ? PHASE3_POP_TARGET : POP_TARGET_EARLY;
        int homeNeed = (targetPop - info.Human_MaxNum + HOUSE_HUMAN_NUM - 1)
                       / HOUSE_HUMAN_NUM;      // 还差几座房
        // 注意用 optionalWood（扣掉新增靶场的预留）：房屋是 priority 1、又排在本
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
                t.priority = 1;
                t.buildingType = n.type;
                taskQueue.push_back(t);
                break;
            }
        }
    } else {
        // 铜器时代：补齐马厩（骑兵前置，也是升铜器三选一之一）
        //   用 optionalWood：新增靶场比马厩重要（第三阶段不造骑兵了）。
        if (count_done(BUILDING_STABLE) == 0 && active_build(BUILDING_STABLE) == 0
            && optionalWood >= pending_build_wood() + BUILD_STABLE_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 2;
            t.buildingType = BUILDING_STABLE;
            taskQueue.push_back(t);
        }
        else if (phase >= 3
            && count_done(BUILDING_RANGE) + active_build(BUILDING_RANGE) < RANGE_MAX
            && woodBudget >= pending_build_wood() + BUILD_RANGE_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 1;
            t.buildingType = BUILDING_RANGE;
            taskQueue.push_back(t);
        }
    }

    int extraFarms = 0, extraPending = 0;
    for (const tagBuilding &b : info.buildings)
        if (b.Type == BUILDING_FARM && b.Percent >= 100
            && granaryFarmSites.count((b.BlockDR << 12) | b.BlockUR)) ++extraFarms;
    for (const Task &t : taskQueue)
        if (t.type == TASK_BUILD && t.buildingType == BUILDING_FARM && t.granaryFarm
            && t.state != TASK_DONE && t.state != TASK_FAILED) ++extraPending;
    const int baseTarget = (phase >= 3) ? FARM_TARGET_LATE : farmTarget;
    const int baseFarms = count_done(BUILDING_FARM) - extraFarms;
    const int basePending = active_build(BUILDING_FARM) - extraPending;
    if (baseFarms + basePending < baseTarget
        && optionalWood >= pending_build_wood() + BUILD_FARM_WOOD + 50) {
        Task t;
        t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 2;
        t.buildingType = BUILDING_FARM;
        taskQueue.push_back(t);
    }
    if (phase >= 3 && farmTarget > 0 && count_done(BUILDING_GRANARY) > 0
        && extraFarms + extraPending < GRANARY_FARM_EXTRA
        && optionalWood >= pending_build_wood() + BUILD_FARM_WOOD + 50) {
        Task t;
        t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 2;
        t.buildingType = BUILDING_FARM; t.granaryFarm = true;
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

        int towerNum = count_done(BUILDING_ARROWTOWER);
        if (towerNum > towerPeak) towerPeak = towerNum;
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
        return;
    }
}

// ==================== 石头的总需求 ====================
static int stone_demand_left()
{
    int need = 0;

    // ① 还要建的箭塔（在造的也算：demand_build 的判据是
    //    `towerNum + active_build(ARROWTOWER) < towerGoal`，这里必须对齐）。
    //    goal 取 “目标 与 历史峰值(打坏几个补几个)” 的较大者。
    if (phase < 3) {
        const int towerNum = count_done(BUILDING_ARROWTOWER);
        int goal = arrowTowerTarget;
        if (towerPeak > goal) goal = towerPeak;
        const int left = goal - towerNum - active_build(BUILDING_ARROWTOWER);
        if (left > 0) need += left * BUILD_ARROWTOWER_STONE;
    }

    if (phase < 3) {
        const double rr = REPAIR_COST_RATIO;
        for (tagBuilding &b : info.buildings) {
            if (b.Type != BUILDING_ARROWTOWER) continue;
            if (b.Percent < 100 || b.MaxBlood <= 0 || b.Blood >= b.MaxBlood) continue;
            const double lost = 1.0 - (double)b.Blood / (double)b.MaxBlood;
            need += (int)(rr * lost * (double)BUILD_ARROWTOWER_STONE);
        }
    }

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
    if (in_stone_window()) return info.Stone < STONE_KEEP_MAX;
    return info.Stone < stone_demand_left();
}

// ---------- 采集需求 ----------
void demand_gather()
{
    const bool foodFarmOnly = (phase >= 3);

    int farmerNum = 0;
    for (tagFarmer &f : info.farmers)
        if (f.FarmerSort == FARMERTYPE_FARMER) farmerNum++;

    // ---- 采集需求（食物 / 木头 / 石头 / 黄金按需分配）----
    int total = farmerNum > 0 ? farmerNum : 1;

    bool needStone = stone_needed();

    int wantStone = 0;
    if (needStone) {
        if (in_stone_window()) {
            wantStone = STONE_WINDOW_WORKERS;
        } else {
            const int need = stone_demand_left();
            wantStone = (need > BUILD_ARROWTOWER_STONE)             ? 3
                      : (need > BUILD_ARROWTOWER_STONE / 2)         ? 2 : 1;
            const int stoneCap = total / 3;
            if (stoneCap >= 1 && wantStone > stoneCap) wantStone = stoneCap;
            if (wantStone < 1) wantStone = 1;
        }
    }

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
    if (huntStarted && has_resource(RESOURCE_GAZELLE)) {
        int freeWood = info.Wood - pending_build_wood();
        if (freeWood >= HUNT_WOOD_SURPLUS) {
            wantHunt += HUNT_SURPLUS_EXTRA;
        }
    }

    // ---- 农田：一个村民对应一格农田 ----
    int wantFarm = farmTarget;
    if (wantFarm > farmCnt) wantFarm = farmCnt;

    if (huntStarted && has_resource(RESOURCE_GAZELLE)) {
        int restNow = total - wantStone - wantGold - wantBush - wantHunt - wantFarm;
        // 上限用 wood_gather_limit()（已含“第三波前 8 / 之后 6”与物理容量），
        // 而不是写死的 WOOD_MAX_GATHERERS —— 否则“分给木头”会被那个旧值封掉。
        int move = restNow - wood_gather_limit();
        if (move > 0) wantHunt += move;
        if (wantHunt > HUNT_MAX_GATHERERS) wantHunt = HUNT_MAX_GATHERERS;
    }

    if (foodFarmOnly) {
        wantBush = 0;
        wantHunt = 0;
    }

    // ---- 木材保底 ----
    while (!foodFarmOnly
           && total - wantStone - wantGold - wantBush - wantHunt - wantFarm
              < WOOD_MIN_GATHERERS) {
        if (wantHunt > 0) wantHunt--;
        else if (wantFarm > 0) wantFarm--;
        else break;
    }

    // ---- 其余劳动力全部伐木 ----
    int rest = total - wantStone - wantGold - wantBush - wantHunt - wantFarm;
    if (rest < 1) rest = 1;
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
            if (fallbackTypes[k] == RESOURCE_GAZELLE && (!huntStarted || foodFarmOnly))
                continue;   // 开局不杀瞪羚；14:00 后野生食物全停（食物只走农田）
            if (fallbackTypes[k] == RESOURCE_BUSH && (!berryPhase || foodFarmOnly))
                continue;   // 浆果阶段已结束 / 14:00 后不再采浆果
            if (fallbackTypes[k] == RESOURCE_STONE && !needStone) continue;
            if (fallbackTypes[k] == RESOURCE_GOLD  && wantGold == 0) continue;
            if (!has_resource(fallbackTypes[k])) continue;
            int allow = spare;
            if (fallbackTypes[k] == RESOURCE_TREE) {
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
    {
        std::unordered_map<int,int> sentNow;   // 本帧兜底已经派到每个资源点的人数
        int woodLeft = wood_gather_limit() - active_gather(RESOURCE_TREE);
        if (woodLeft < 0) woodLeft = 0;
        ensure_cutter_at_tree();
        double homeDR = 0, homeUR = 0;
        const bool haveHome = home_center(homeDR, homeUR);
        for (tagFarmer &f : info.farmers) {
            if (f.FarmerSort != FARMERTYPE_FARMER) continue;
            // 正负责建造的村民即使这一帧看着空闲（建造关系被内核断了）也不能拉走：
            // 拉走就烂尾了，recycle_tasks 会把它们叫回工地。
            if (on_build_task(f.SN)) continue;
            if (!farmer_available(f)) continue;

            if (haveHome && gather_spot_dangerous(f.DR, f.UR)) {
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
            for (int pass = 0; pass < 3 && pick == -1; pass++) {
                int treeSN = -1, anySN = -1;
                int treeUsed = 0x7fffffff, anyUsed = 0x7fffffff;   // 主判据：点上已派几人
                double treeD = 1e18, anyD = 1e18;          // 平手用：村民到资源的距离
                double treeHaul = 1e18, anyHaul = 1e18;    // 次判据：资源到最近存放建筑的距离
                for (tagResource &r : info.resources) {
                    if (r.Cnt <= 0 && r.Blood <= 0) continue;
                    if (r.Type != RESOURCE_TREE && r.Type != RESOURCE_STONE
                        && r.Type != RESOURCE_GOLD && r.Type != RESOURCE_BUSH
                        && r.Type != RESOURCE_GAZELLE)
                        continue;
                    if (r.Type == RESOURCE_TREE && pass < 2 && woodLeft <= 0) continue;
                    if (r.Type == RESOURCE_GAZELLE && (!huntStarted || foodFarmOnly))
                        continue;   // 开局不杀瞪羚；14:00 后野生食物全停
                    if (r.Type == RESOURCE_BUSH && (!berryPhase || foodFarmOnly))
                        continue;   // 浆果阶段结束 / 14:00 后不再采浆果
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
                    if (pass == 2) {
                        if (r.Type != RESOURCE_TREE) continue;   // 石/金/浆果/猎物前两趟已试过
                        if (cutterAtTree[r.SN] + sentNow[r.SN] >= spots) continue;
                    }
                    double haul = nearest_dropoff_dist(r.Type, r.DR, r.UR);
                    double d = calDistance(f.DR, f.UR, r.DR, r.UR);
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
    const int toFrames = (TimePerFrame > 0) ? TimePerFrame : 40;   // 帧 ↔ 毫秒换算
    if (info.GameFrame - gatherLogFrame >= 5000 / toFrames) {
        gatherLogFrame = info.GameFrame;
        // 待派建造任务数：如果这个数一直 > 0 而建筑又不动工，就是“没人/没地方建”
        int nBuildWait = 0;
        for (Task &t : taskQueue)
            if (t.type == TASK_BUILD && t.state == TASK_WAITING) nBuildWait++;
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
void demand_army()
{
    int bowman = 0, composite = 0, scout = 0, totalArmy = 0;
    bool weakKillAlive = false;            // weakKillSN 还在吗（见它的说明）
    for (tagArmy &a : info.armies) {
        if (a.SN == weakKillSN) weakKillAlive = true;
        if (a.Sort == AT_PRIEST) continue;
        if (a.Sort == AT_SCOUT) { scout++; continue; }
        totalArmy++;
        if (a.Sort == AT_COMPOSITE_BOWMAN) composite++;
        if (a.Sort == AT_BOWMAN || a.Sort == AT_COMPOSITE_BOWMAN
            || a.Sort == AT_SLINGER) bowman++;
    }

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

    if (compositeBowUrgent()) return;

    const bool rushComposite = (phase >= 3) && compositeBowReady()
                               && composite < ASSAULT_BOWMAN_MIN;
    int target = (phase >= 3) ? armyTarget + 8 : armyTarget;
    if (!rushComposite && totalArmy >= target) return;

    int bowmanForTarget = compositeBowReady() ? composite : bowman;
    int bowmanCap = compositeBowReady() ? (target + 1) / 2 : BOWMAN_PRE_TECH_MAX;
    // 推图至少要 ASSAULT_BOWMAN_MIN 个复合弓兵：别被兵力公式算小了卡住名额
    if (compositeBowReady() && bowmanCap < ASSAULT_BOWMAN_MIN)
        bowmanCap = ASSAULT_BOWMAN_MIN;
    tagBuilding *rg = free_building(BUILDING_RANGE);
    if (rg && !rangeReservedForResearch() && bowmanForTarget < bowmanCap) {
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
    for (ResearchState &r : researches)
        if (r.action == BUILDING_MARKET_WOOD_UPGRADE) { r.minPhase = 1; break; }
    add("驯养动物", BUILDING_MARKET, BUILDING_MARKET_FARM_UPGRADE, 1,
        BUILDING_MARKET_FARM_UPGRADE_FOOD, BUILDING_MARKET_FARM_UPGRADE_WOOD, 0, 0,
        0, 0, 0, 0);
    add("金矿开采", BUILDING_MARKET, BUILDING_MARKET_GOLD_UPGRADE, 1,
        BUILDING_MARKET_GOLD_UPGRADE_FOOD, BUILDING_MARKET_GOLD_UPGRADE_WOOD, 0, 0,
        0, 0, 0, 0);
    add("车轮",     BUILDING_MARKET, BUILDING_MARKET_WHEEL_UPGRADE, 1,
        BUILDING_MARKET_WHEEL_UPGRADE_FOOD, BUILDING_MARKET_WHEEL_UPGRADE_WOOD, 0, 0,
        0, 0, 0, 0);

    add("复合弓科技", BUILDING_RANGE, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW, 1,
        BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_FOOD,
        BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_WOOD, 0, 0, 0, 0, 0, 0);
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

    // 弓兵护甲：两级科技。
    add("弓兵护甲", BUILDING_STOCK, BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER, 2,
        BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER_FOOD, 0, 0, 0,
        BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER_2_FOOD, 0, 0,
        BUILDING_STOCK_UPGRADE_DEFENSE_ARCHER_2_GOLD);
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
            return (r.deadlineFrame > 0 && info.GameFrame >= r.urgentFromFrame)
                || phase >= 3;
        }
    return false;
}

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

bool rushing_composite_bowman()
{
    if (!compositeBowReady()) return false;
    return composite_bowman_count() < ASSAULT_BOWMAN_MIN;
}

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
        return r.deadlineFrame > 0 && r.level < r.maxLevel
            && (info.GameFrame >= r.urgentFromFrame || phase >= 3);
    };

    // ---- 下单顺序：用一张"档位表"明确写死，按档位从小到大依次下单 ----
    auto researchRank = [&](ResearchState &r) -> int {
        return isUrgent(r) ? 0 : 1;
    };
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

static bool army_is_siege(int sort) { return sort == AT_STONE_THROWER; }

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

int enemy_hunter_count()
{
    int n = 0;
    for (tagArmy &e : info.enemy_armies) {
        if (e.Sort == AT_CAVALRY || e.Sort == AT_CHARIOT
            || e.Sort == AT_CHARIOT_ARCHER) n++;
    }
    return n;
}

// ---------- 敌方兵力账本 ----------
static void update_enemy_ledger()
{
    const int tpf = (TimePerFrame > 0) ? TimePerFrame : 40;
    const double bsl2 = BLOCKSIDELENGTH;

    // 1) 看见的都刷新（出现即确认存活）
    for (tagArmy &e : info.enemy_armies) {
        enemyLedgerSeenFrame[e.SN] = info.GameFrame;
        enemyLedgerDR[e.SN] = e.DR;
        enemyLedgerUR[e.SN] = e.UR;
        std::unordered_map<int,char>::iterator itD = enemyLedgerDead.find(e.SN);
        if (itD != enemyLedgerDead.end()) itD->second = 0;   // 又看到了：撤销“已消灭”
    }

    // 2) 判“已消灭”：够久没见到 + 我方守着它最后出现的位置
    //    节流：这一步是 账本条数 × 我方单位数 的距离计算，不必每帧做。
    static int confirmFrame = 0;
    if (info.GameFrame - confirmFrame < 10) return;
    confirmFrame = info.GameFrame;
    const int confirmFrames = ENEMY_DEAD_CONFIRM_MS / tpf;
    const double sight = ENEMY_DEAD_SIGHT_DIST * bsl2;
    for (std::unordered_map<int,int>::iterator it = enemyLedgerSeenFrame.begin();
         it != enemyLedgerSeenFrame.end(); ++it) {
        const int sn = it->first;
        std::unordered_map<int,char>::iterator itD = enemyLedgerDead.find(sn);
        if (itD != enemyLedgerDead.end() && itD->second) continue;      // 已消灭
        if (info.GameFrame - it->second < confirmFrames) continue;      // 还没到确认时间
        const double dr = enemyLedgerDR[sn], ur = enemyLedgerUR[sn];
        bool watched = false;
        for (tagArmy &a : info.armies)
            if (calDistance(a.DR, a.UR, dr, ur) <= sight) { watched = true; break; }
        if (!watched)
            for (tagBuilding &b : info.buildings)
                if (calDistance(b.BlockDR * bsl2, b.BlockUR * bsl2, dr, ur) <= sight) {
                    watched = true;
                    break;
                }
        if (watched) enemyLedgerDead[sn] = 1;
    }
}

// 账本里“还没被确认消灭”的敌人数 = 估计的敌方剩余兵力
static int enemy_remains()
{
    int alive = 0;
    for (std::unordered_map<int,int>::iterator it = enemyLedgerSeenFrame.begin();
         it != enemyLedgerSeenFrame.end(); ++it) {
        std::unordered_map<int,char>::iterator itD = enemyLedgerDead.find(it->first);
        if (itD == enemyLedgerDead.end() || !itD->second) alive++;
    }
    return alive;
}

// 账本规模（见过的敌方单位总数）—— 用来判断“账本是否已经建立”
static int enemy_ledger_total() { return (int)enemyLedgerSeenFrame.size(); }

// ---------- 敌方位置记录 ----------
void record_enemy_positions()
{
    // 己方军队不受迷雾过滤；由出现后消失确认阵亡，保留最后可取得的位置。
    bool scoutAlive = false;
    for (const tagArmy &a : info.armies) {
        if (a.Sort != AT_SCOUT) continue;
        scoutAlive = true;
        scoutLastDR = a.DR;
        scoutLastUR = a.UR;
        break;
    }
    if (scoutAlive) {
        scoutSeenAlive = true;
    } else if (scoutSeenAlive) {
        scoutSeenAlive = false;
        scoutDeathPending = true;
    }
    if (phase < 3) return;

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
    for (tagArmy &e : info.enemy_armies) {
        double d = haveHome ? calDistance(e.DR, e.UR, homeDR, homeUR) : 0.0;
        if (d > recD) {
            recD = d;
            enemyFarDR = e.DR;
            enemyFarUR = e.UR;
            enemyFarFound = true;
        }
    }
    if (scoutDeathPending) {
        scoutDeathPending = false;
        if (enemySiegeSN == -1) {
            enemyFarDR = scoutLastDR;
            enemyFarUR = scoutLastUR;
            enemyFarFound = true;
            DebugText("侦察骑兵阵亡：以最后位置作为敌营搜索点");
        }
    }
}

// ---------- 前线集合点：村庄（市镇中心）外 RALLY_AWAY_DIST 格的一块空地 ----------
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

static bool prepare_unit_reach(const tagArmy &a);
static bool spread_cell_reachable(int bx, int by);

static bool kite_retreat_home(tagArmy &a)
{
    double hDR = 0, hUR = 0;
    if (!home_center(hDR, hUR)) return false;
    const double bsl = BLOCKSIDELENGTH;
    double dx = hDR - a.DR, dy = hUR - a.UR;
    const double len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6) return false;                 // 已经站在家中心：没地方退
    dx /= len; dy /= len;
    const int cx = a.BlockDR, cy = a.BlockUR;
    if (info.GameFrame - unitStepFrame[a.SN] < RANGED_STEP_GAP) return false;   // 位移节流

    const int tx = cx + (int)lround(dx * KITE_RETREAT_STEP);
    const int ty = cy + (int)lround(dy * KITE_RETREAT_STEP);
    if (!prepare_unit_reach(a)) return false;
    int destX = -1, destY = -1; double best = 1e18;
    for (int bx = tx - 6; bx <= tx + 6; ++bx) {
        for (int by = ty - 6; by <= ty + 6; ++by) {
            if (!landing_ok(bx, by, a.SN) || !spread_cell_reachable(bx, by)) continue;
            if (bx == cx && by == cy) continue;
            const double score = calDistance(bx, by, tx, ty);
            if (score < best) { best = score; destX = bx; destY = by; }
        }
    }
    if (destX < 0) return false;
    if (a.NowState == HUMAN_STATE_WALKING && !unitNeedsRecovery.count(a.SN)
        && calDistance(a.DR0, a.UR0, (destX + 0.5) * bsl, (destY + 0.5) * bsl) <= bsl)
        return true;
    unitNeedsRecovery.erase(a.SN);
    unitStepFrame[a.SN] = info.GameFrame;
    cellClaim[(destX << 12) | destY] = a.SN;
    HumanMove(a.SN, (destX + 0.5) * bsl, (destY + 0.5) * bsl);
    return true;
}

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
        if (wantSN < 0 || (a.WorkObjectSN == wantSN
            && a.NowState != HUMAN_STATE_IDLE && !unitNeedsRecovery.count(a.SN))) return;
        if (info.GameFrame - unitFireFrame[a.SN] < RANGED_FIRE_GAP) return;
        if (onlyIfInReach && td > reach) return;
        unitNeedsRecovery.erase(a.SN);
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
    if (a.NowState == HUMAN_STATE_WALKING && attackOrderSN.count(a.SN)
        && attackOrderSN[a.SN] == -9 && !unitNeedsRecovery.count(a.SN)
        && info.GameFrame - unitStepFrame[a.SN] < 3000 / TimePerFrame)
        return true;
    if (unitNeedsRecovery.count(a.SN) && prepare_unit_reach(a)) {
        double targetDR = 0, targetUR = 0; bool found = false;
        for (const tagArmy &e : info.enemy_armies)
            if (e.SN == wantSN) { targetDR = e.DR; targetUR = e.UR; found = true; break; }
        for (const tagBuilding &e : info.enemy_buildings)
            if (e.SN == wantSN) {
                targetDR = e.BlockDR * BLOCKSIDELENGTH;
                targetUR = e.BlockUR * BLOCKSIDELENGTH; found = true; break;
            }
        const double bsl = BLOCKSIDELENGTH;
        if (found && calDistance(a.DR, a.UR, targetDR, targetUR) > reach * bsl) {
            int bxBest = -1, byBest = -1; double best = 1e18;
            for (int dx = -4; dx <= 4; ++dx) {
                for (int dy = -4; dy <= 4; ++dy) {
                    const int bx = a.BlockDR + dx, by = a.BlockUR + dy;
                    if (dx * dx + dy * dy < 2) continue;
                    if (!landing_ok(bx, by, a.SN) || !spread_cell_reachable(bx, by)) continue;
                    const double score = calDistance((bx + 0.5) * bsl, (by + 0.5) * bsl,
                                                       targetDR, targetUR);
                    if (score < best) { best = score; bxBest = bx; byBest = by; }
                }
            }
            if (bxBest >= 0) {
                HumanMove(a.SN, (bxBest + 0.5) * bsl, (byBest + 0.5) * bsl);
                unitStepFrame[a.SN] = info.GameFrame;
                unitNeedsRecovery.erase(a.SN);
                attackOrderSN[a.SN] = -9;
                cellClaim[(bxBest << 12) | byBest] = a.SN;
                return true;
            }
        }
    }
    shoot(false);                            // 有目标：交给内核走过去开火
    return true;
}

// 连通性 BFS 的共享状态（**建造选址、行军走位都用这一套**）。
static int reachStamp[505][505];      // 访问时间戳（避免每轮清 255KB）
static int reachCur = 0;
static std::vector<int> reachQueue;   // BFS 队列（x*505+y）

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

static bool prepare_unit_reach(const tagArmy &a)
{
    static int frame = -1, stamp = -1;
    if (a.BlockDR < 0 || a.BlockUR < 0 || a.BlockDR >= 505 || a.BlockUR >= 505)
        return false;
    if (frame == info.GameFrame && stamp == reachCur
        && reachStamp[a.BlockDR][a.BlockUR] == reachCur) return true;
    const bool ready = reach_bfs(a.BlockDR, a.BlockUR);
    frame = info.GameFrame; stamp = reachCur;
    return ready;
}

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
    static int spreadReachStamp = -1;
    if (spreadReachFrame != info.GameFrame || spreadReachStamp != reachCur) {
        spreadReachFrame = info.GameFrame;
        spreadReachReady = reach_bfs(cx, cy);
        spreadReachStamp = reachCur;
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

static unsigned char rallyOkMap[505][505];
static int rallyOkFrame = -1000000;

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

static bool rally_patch_clear(int bx, int by, int half)
{
    if (!rally_ground_ok(bx, by)) return false;
    for (int di = -half; di <= half; di++)
        for (int dj = -half; dj <= half; dj++)
            if (!rally_ground_ok(bx + di, by + dj)) return false;
    return true;
}

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
        const int rectXMin = (hx < ebx) ? hx : ebx;
        const int rectXMax = (hx < ebx) ? ebx : hx;
        const int rectYMin = (hy < eby) ? hy : eby;
        const int rectYMax = (hy < eby) ? eby : hy;
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

    cellClaim.clear();

    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;  // 祭司守家、侦察兵探路
        if (a.NowState != HUMAN_STATE_IDLE) continue;             // 走路/交战都不打扰

        // 已经进到待命区里 → 不再下指令。
        //   每帧重下会被内核 suspendRelation + 清路径，单位就在原地抖
        //   （这个坑在集合点那儿已经踩过一次）。
        if (calDistance(a.DR, a.UR, areaDR, areaUR) <= half * bsl) continue;

        int mx = -1, my = -1;
        if (!spread_slot(bx, by, half, a.SN, mx, my)) continue;   // 分不到：下帧再试
        // 已经站在自己那一格（或紧挨着）→ 不再重下
        const int ddx = a.BlockDR - mx, ddy = a.BlockUR - my;
        if (ddx * ddx + ddy * ddy <= 1) continue;
        HumanMove(a.SN, (mx + 0.5) * bsl, (my + 0.5) * bsl);
        attackOrderSN[a.SN] = -7;      // -7 = 去军队待命点（与其他编码区分开）
    }
}

static int assault_height(int bx, int by)
{
    if (info.theMap == nullptr || bx < 0 || by < 0
        || bx >= (int)info.theMap->size() || by >= (int)(*info.theMap)[bx].size()) return 0;
    return (*info.theMap)[bx][by].height;
}

static bool bow_attack_home_side(tagArmy &a, int targetSN, double homeDR, double homeUR)
{
    const tagBuilding *tower = nullptr;
    for (const tagBuilding &b : info.enemy_buildings)
        if (b.SN == targetSN && b.Type == BUILDING_ARROWTOWER) { tower = &b; break; }
    if (!tower) return false;
    const double bsl = BLOCKSIDELENGTH;
    const double tx = (tower->BlockDR + building_size(tower->Type) * 0.5) * bsl;
    const double ty = (tower->BlockUR + building_size(tower->Type) * 0.5) * bsl;
    double ux = homeDR - tx, uy = homeUR - ty;
    const double len = sqrt(ux * ux + uy * uy);
    if (len < bsl) return false;
    ux /= len; uy /= len;
    auto firing = [&](double x, double y) {
        const double range = std::min(static_cast<double>(VISION_COMPOSITE_BOWMAN),
            own_attack_range(a.Sort) + std::max(0, assault_height((int)(x / bsl), (int)(y / bsl))
                - assault_height(tower->BlockDR, tower->BlockUR)));
        const double depth = (x - tx) * ux + (y - ty) * uy;
        const double lateral = fabs((x - tx) * uy - (y - ty) * ux);
        return depth >= bsl && depth >= lateral
            && std::max(fabs(x - tx), fabs(y - ty)) <= (range - 0.2) * bsl;
    };
    bool currentClear = true;
    for (const tagBuilding &b : info.enemy_buildings) {
        if (b.Type != BUILDING_ARROWTOWER || b.SN == targetSN || b.Percent < 100) continue;
        const double ox = (b.BlockDR + building_size(b.Type) * 0.5) * bsl;
        const double oy = (b.BlockUR + building_size(b.Type) * 0.5) * bsl;
        const double range = std::min(static_cast<double>(VISION_ARROWTOWER),
            static_cast<double>(DIS_ARROWTOWER) + ENEMY_DIS_ADD_TOWER
                + std::max(0, assault_height(b.BlockDR, b.BlockUR) - assault_height(a.BlockDR, a.BlockUR)));
        if (std::max(fabs(a.DR - ox), fabs(a.UR - oy)) <= (range + 0.25) * bsl) currentClear = false;
    }
    if (currentClear && firing(a.DR, a.UR) && !unitNeedsRecovery.count(a.SN)) {
        if (a.WorkObjectSN != targetSN || a.NowState == HUMAN_STATE_IDLE) {
            if (attackOrderSN[a.SN] != targetSN
                || info.GameFrame - unitFireFrame[a.SN] >= RANGED_FIRE_GAP) {
                HumanAction(a.SN, targetSN); attackOrderSN[a.SN] = targetSN;
                unitFireFrame[a.SN] = info.GameFrame;
            }
        }
        return true;
    }
    if (a.NowState == HUMAN_STATE_WALKING && bowTowerMoveTarget[a.SN] == targetSN
        && !unitNeedsRecovery.count(a.SN)
        && info.GameFrame - unitStepFrame[a.SN] < 3000 / TimePerFrame) return true;
    if (info.GameFrame - unitStepFrame[a.SN] < RANGED_STEP_GAP) return true;
    if (!prepare_unit_reach(a)) return true;
    int bxBest = -1, byBest = -1; double best = 1e18;
    for (int bx = tower->BlockDR - 12; bx <= tower->BlockDR + 13; ++bx) {
        for (int by = tower->BlockUR - 12; by <= tower->BlockUR + 13; ++by) {
            if (!landing_ok(bx, by, a.SN) || !spread_cell_reachable(bx, by)) continue;
            const double x = (bx + 0.5) * bsl, y = (by + 0.5) * bsl;
            if (!firing(x, y)) continue;
            double risk = 0;
            for (const tagBuilding &b : info.enemy_buildings) {
                if (b.Type != BUILDING_ARROWTOWER || b.SN == targetSN || b.Percent < 100) continue;
                const double ox = (b.BlockDR + building_size(b.Type) * 0.5) * bsl;
                const double oy = (b.BlockUR + building_size(b.Type) * 0.5) * bsl;
                const double range = std::min(static_cast<double>(VISION_ARROWTOWER),
                    static_cast<double>(DIS_ARROWTOWER) + ENEMY_DIS_ADD_TOWER
                    + std::max(0, assault_height(b.BlockDR, b.BlockUR) - assault_height(bx, by)));
                const double d = std::max(fabs(x - ox), fabs(y - oy)) / bsl;
                if (d <= range + 0.25) risk += 1000 + range + 0.25 - d;
            }
            const double depth = (x - tx) * ux + (y - ty) * uy;
            const double lateral = fabs((x - tx) * uy - (y - ty) * ux);
            const double score = risk * bsl + 2 * lateral - depth
                + 0.15 * calDistance(a.DR, a.UR, x, y);
            if (score < best) { best = score; bxBest = bx; byBest = by; }
        }
    }
    if (bxBest >= 0) {
        if (calDistance(a.DR, a.UR, (bxBest + 0.5) * bsl, (byBest + 0.5) * bsl) <= 0.75 * bsl
            && firing(a.DR, a.UR)) {
            if (a.WorkObjectSN != targetSN || a.NowState == HUMAN_STATE_IDLE) {
                if (attackOrderSN[a.SN] != targetSN
                    || info.GameFrame - unitFireFrame[a.SN] >= RANGED_FIRE_GAP) {
                    HumanAction(a.SN, targetSN); attackOrderSN[a.SN] = targetSN;
                    unitFireFrame[a.SN] = info.GameFrame;
                }
            }
            unitNeedsRecovery.erase(a.SN);
            return true;
        }
        HumanMove(a.SN, (bxBest + 0.5) * bsl, (byBest + 0.5) * bsl);
        attackOrderSN[a.SN] = -10; bowTowerMoveTarget[a.SN] = targetSN;
        unitStepFrame[a.SN] = info.GameFrame; unitNeedsRecovery.erase(a.SN);
        cellClaim[(bxBest << 12) | byBest] = a.SN;
    }
    return true;
}

static double siege_path_risk(double x, double y, int ignoredTower)
{
    const double bsl = BLOCKSIDELENGTH;
    double risk = 0;
    for (const tagBuilding &b : info.enemy_buildings) {
        if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100 || b.SN == ignoredTower) continue;
        const double tx = (b.BlockDR + building_size(b.Type) * 0.5) * bsl;
        const double ty = (b.BlockUR + building_size(b.Type) * 0.5) * bsl;
        const double range = std::min(static_cast<double>(VISION_ARROWTOWER),
            static_cast<double>(DIS_ARROWTOWER) + ENEMY_DIS_ADD_TOWER
                + std::max(0, assault_height(b.BlockDR, b.BlockUR)
                    - assault_height((int)(x / bsl), (int)(y / bsl))));
        risk += std::max(0.0, range + 0.5 - std::max(fabs(x - tx), fabs(y - ty)) / bsl);
    }
    for (const tagArmy &e : info.enemy_armies) {
        const double reach = std::max(4.0, own_attack_range(e.Sort) + 2);
        risk += 2 * std::max(0.0, reach - calDistance(x, y, e.DR, e.UR) / bsl);
    }
    return risk;
}

static int siege_diag_order(int id, int sn, int target, double dr, double ur)
{
    siegeDiagOrders.push_back({id, sn, target, info.GameFrame});
    DebugText(std::string("投石车下令: SN=") + std::to_string(sn)
        + " id=" + std::to_string(id) + " 类型=" + (target >= 0 ? "攻击" : "移动/停止")
        + " 目标=" + std::to_string(target)
        + " 落点=(" + std::to_string(dr / BLOCKSIDELENGTH) + "," + std::to_string(ur / BLOCKSIDELENGTH) + ")"
        + " 原因=" + siegeOrderReason[sn]);
    return id;
}

static bool siege_safe_step(tagArmy &a, double goalDR, double goalUR, int ignoredTower, bool fleeing)
{
    const double bsl = BLOCKSIDELENGTH;
    const int tpf = std::max(1, TimePerFrame);
    if (a.BlockDR < 0 || a.BlockUR < 0 || a.BlockDR >= 505 || a.BlockUR >= 505) return false;
    const double initialRisk = siege_path_risk(a.DR, a.UR, ignoredTower);
    if (a.NowState == HUMAN_STATE_WALKING && !unitNeedsRecovery.count(a.SN)
        && info.GameFrame - unitStepFrame[a.SN] < 800 / tpf
        && (!fleeing || siege_path_risk(a.DR0, a.UR0, ignoredTower) <= initialRisk + 0.01)) return true;
    if (info.GameFrame - unitStepFrame[a.SN] < RANGED_STEP_GAP) return false;
    const int origin = a.BlockDR * 505 + a.BlockUR;
    std::vector<int> queue(1, origin);
    std::unordered_map<int,int> parent;
    parent[origin] = origin;
    std::unordered_map<int,double> pathRisk;
    pathRisk[origin] = initialRisk;
    int bestKey = origin;
    double best = (fleeing ? initialRisk * 1000 * bsl : initialRisk * 0.25 * bsl)
        + calDistance(a.DR, a.UR, goalDR, goalUR);
    const int dx[4] = {1,-1,0,0}, dy[4] = {0,0,1,-1};
    for (size_t head = 0; head < queue.size() && head < 6000; ++head) {
        const int key = queue[head], bx = key / 505, by = key % 505;
        const double x = (bx + 0.5) * bsl, y = (by + 0.5) * bsl;
        const double risk = key == origin ? initialRisk : siege_path_risk(x, y, ignoredTower);
        if (key != origin && landing_ok(bx, by, a.SN)) {
            const double score = (fleeing ? risk * 1000 * bsl : pathRisk[key] * 0.25 * bsl)
                + calDistance(x, y, goalDR, goalUR);
            if (score < best - 0.1 * bsl) { best = score; bestKey = key; }
        }
        for (int k = 0; k < 4; ++k) {
            const int nx = bx + dx[k], ny = by + dy[k];
            if (!rally_ground_ok(nx, ny) || !landing_ok(nx, ny, a.SN)) continue;
            const int next = nx * 505 + ny;
            if (parent.count(next)) continue;
            const double nextRisk = siege_path_risk((nx + 0.5) * bsl, (ny + 0.5) * bsl, ignoredTower);
            if (fleeing && nextRisk > risk + 0.01) continue;
            pathRisk[next] = std::max(pathRisk[key], nextRisk);
            parent[next] = key; queue.push_back(next);
        }
    }
    if (bestKey == origin) return false;
    int waypoint = bestKey;
    while (parent[waypoint] != origin) waypoint = parent[waypoint];
    const int bx = waypoint / 505, by = waypoint % 505;
    siege_diag_order(HumanMove(a.SN, (bx + 0.5) * bsl, (by + 0.5) * bsl),
        a.SN, -1, (bx + 0.5) * bsl, (by + 0.5) * bsl);
    attackOrderSN[a.SN] = -12; unitStepFrame[a.SN] = info.GameFrame;
    unitNeedsRecovery.erase(a.SN); cellClaim[(bx << 12) | by] = a.SN;
    return true;
}

static bool reserve_wave3_siege(const tagArmy &enemy)
{
    if (enemy.Sort != AT_STONE_THROWER || info.GameFrame < ATTACK_START_FRAME
        || assaultState >= 2) return false;
    double homeDR = 0, homeUR = 0;
    return home_center(homeDR, homeUR)
        && calDistance(enemy.DR, enemy.UR, homeDR, homeUR)
            <= HOME_DEFEND_RADIUS * BLOCKSIDELENGTH;
}

static bool assault_priest_hunter(int sort)
{
    return sort == AT_CAVALRY || sort == AT_CHARIOT || sort == AT_CHARIOT_ARCHER;
}

static void retreat_assault_priest(tagArmy &p)
{
    const double bsl = BLOCKSIDELENGTH;
    const int gap = std::max(1, 500 / TimePerFrame);
    if (info.GameFrame - unitStepFrame[p.SN] < gap
        && p.NowState == HUMAN_STATE_WALKING) return;
    if (!prepare_unit_reach(p)) return;
    double homeDR = p.DR, homeUR = p.UR;
    home_center(homeDR, homeUR);
    int bestBX = -1, bestBY = -1; double best = -1e18;
    for (int dx = -6; dx <= 6; ++dx) {
        for (int dy = -6; dy <= 6; ++dy) {
            if (dx * dx + dy * dy < 4 || dx * dx + dy * dy > 36) continue;
            const int bx = p.BlockDR + dx, by = p.BlockUR + dy;
            if (!landing_ok(bx, by, p.SN) || !spread_cell_reachable(bx, by)) continue;
            const double gx = (bx + 0.5) * bsl, gy = (by + 0.5) * bsl;
            if (point_in_enemy_tower_range(gx, gy, TOWER_SAFE_MARGIN)) continue;
            double clearance = 100 * bsl;
            for (const tagArmy &e : info.enemy_armies) {
                const double threatRange = assault_priest_hunter(e.Sort)
                    ? PRIEST_HUNTER_FLEE_DIST : own_attack_range(e.Sort) + 2;
                clearance = std::min(clearance,
                    calDistance(gx, gy, e.DR, e.UR) - threatRange * bsl);
            }
            const double score = clearance - 0.05 * calDistance(gx, gy, homeDR, homeUR);
            if (score > best) { best = score; bestBX = bx; bestBY = by; }
        }
    }
    if (bestBX >= 0) {
        const double gx = (bestBX + 0.5) * bsl, gy = (bestBY + 0.5) * bsl;
        if (p.NowState != HUMAN_STATE_WALKING || calDistance(p.DR0, p.UR0, gx, gy) > bsl) {
            HumanMove(p.SN, gx, gy);
            unitStepFrame[p.SN] = info.GameFrame;
        }
        cellClaim[(bestBX << 12) | bestBY] = p.SN;
    } else {
        kite_retreat_home(p);
    }
}

void demand_attack()
{
    if (phase < 3) return;

    cellClaim.clear();

    const double bsl = BLOCKSIDELENGTH;               // 1 格 = 多少细节坐标
    // 敌方箭塔射程：DIS_ARROWTOWER(7) + 谷仓升级/木材加工/工艺(+3) = 10 格（敌方科技全满）
    const double towerRange = (double)(DIS_ARROWTOWER + ENEMY_DIS_ADD_TOWER);
    const int toFrames = (TimePerFrame > 0) ? TimePerFrame : 40;
    const bool priestMayJoin = (assaultState >= 2);
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

    const double gameMinNow = (double)info.GameFrame * toFrames / 60000.0;

    tagArmy *priest = nullptr;
    for (tagArmy &a : info.armies)
        if (a.Sort == AT_PRIEST) { priest = &a; break; }


    // 第三波家中还有待转化投石车时，保持守家，防止本帧切入反攻解除保留规则。
    for (const tagArmy &e : info.enemy_armies)
        if (reserve_wave3_siege(e)) return;

    // 集结/推图途中家里被打 → 先交给 combat_tactic 守家（守住了再出门）。
    // 但兵力已经攒够（ATTACK_FORCE）时不再拖：反攻是唯一取胜手段，硬上限 30:00。
    if (assaultState < 2 && bt_enemy_at_home() && totalArmy < ATTACK_FORCE) return;

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
        if (assaultState < 2) {
            assaultState = 2;
            assaultStageFrame = info.GameFrame;
        }
    }

    // ---- 4) 战场态势 ----
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
    if (assaultState == 2) {
        // 清兵计时与账本必须同时满足，失去视野不直接触发拆塔。
        if (info.enemy_armies.empty()) {
            if (enemyClearSinceFrame == 0)
                enemyClearSinceFrame = info.GameFrame;
        } else {
            enemyClearSinceFrame = 0;             // 还看得见敌人：重新计时
        }
        if (enemyClearSinceFrame != 0
            && info.GameFrame - enemyClearSinceFrame >= ASSAULT_CLEAR_CONFIRM_MS / toFrames
            && enemy_ledger_total() > 0
            && enemy_remains() == 0) {
            assaultState = 3;                     // 已知敌军账本清零且持续无可见敌军
            assaultStageFrame = info.GameFrame;
            enemyClearSinceFrame = 0;
        }
    } else if (assaultState == 3) {
        const bool committed = (info.GameFrame - assaultStageFrame
                                > ASSAULT_TOWER_COMMIT_MS / toFrames);
        if (fieldUnits >= ASSAULT_ROLLBACK_MIN && committed) {   // 真冒出一队 → 先打人
            assaultState = 2;
            assaultStageFrame = info.GameFrame;
        } else if (towerCnt <= 2 && haveBase && enemySiegeSN >= 0
                   && info.enemy_armies.empty() && enemy_remains() == 0) {
            const bool waited = (info.GameFrame - assaultStageFrame
                                 > ASSAULT_HUNTER_WAIT_MS / toFrames);
            if (enemyHunters == 0 || (towerCnt == 0 && waited)) {
                DebugText(std::string("提前转化: 敌兵清空，剩塔=") + std::to_string(towerCnt));
                assaultState = 4;              // 猎手清完（或等到放弃）→ 祭司进场
                assaultStageFrame = info.GameFrame;
            }
        }
    } else if (assaultState == 4 && towerCnt > 2) {
        assaultState = 3;                      // 又看到塔（新探索到的）→ 回去拆
        assaultStageFrame = info.GameFrame;
    }


    // ---- 6) 这一轮集火拆哪座箭塔（全队打同一座：拆得快、少挨打）----
    int focusTower = -1;
    if (towerCnt > 0) {
        const double originDR = haveHome ? homeDR : sx;
        const double originUR = haveHome ? homeUR : sy;
        double best = 1e18;
        bool previousAlive = false;
        for (const tagBuilding &b : info.enemy_buildings) {
            if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
            if (b.SN == assaultFocusTower) previousAlive = true;
            const double dr = (b.BlockDR + building_size(b.Type) * 0.5) * bsl;
            const double ur = (b.BlockUR + building_size(b.Type) * 0.5) * bsl;
            const double d = calDistance(originDR, originUR, dr, ur);
            if (d < best) { best = d; focusTower = b.SN; }
        }
        if (assaultState == 3) {
            if (previousAlive) focusTower = assaultFocusTower;
            else if (focusTower >= 0) {
                assaultFocusTower = focusTower;
                DebugText(std::string("拆塔目标: 优先距我方大本营最近的塔=") + std::to_string(focusTower));
            }
        }
    } else {
        assaultFocusTower = -1;
    }

    if (assaultState != 2) {
        baitScoutSN = -1; baitScoutBack = false; baitScoutReturning = false;
    }
    if (assaultState == 2 && haveHome) {
        if (!slowPushValid) {
            double sumDR = 0, sumUR = 0; int count = 0;
            for (const tagArmy &a : info.armies) {
                if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT || a.SN == weakKillSN) continue;
                sumDR += a.DR; sumUR += a.UR; ++count;
            }
            if (count > 0) {
                slowPushDR = sumDR / count; slowPushUR = sumUR / count;
                slowPushValid = true;
            }
        }
        double scDR = 0, scUR = 0; int scBlood = 0; bool scAlive = false;
        if (baitScoutSN >= 0) {
            for (tagArmy &a : info.armies)
                if (a.SN == baitScoutSN) {
                    scAlive = true; scDR = a.DR; scUR = a.UR; scBlood = a.Blood; break;
                }
            if (!scAlive) {                          // 阵亡 → 重选（带冷却）
                baitScoutSN = -1;
                baitScoutSwitchFrame = info.GameFrame + BAIT_SCOUT_SWITCH_MS / toFrames;
            }
        }
        // ---- 步骤 1：选突击者（刚摘过标志的 5 秒内不再选，避免同一个兵被反复抓）----
        if (baitScoutSN < 0 && info.GameFrame >= baitScoutSwitchFrame) {
            int bestRanged = -1, bestFast = -1;
            double brD = 1e18, bfD = 1e18;
            for (tagArmy &a : info.armies) {
                if (a.SN == weakKillSN) continue;
                if (a.Sort == AT_PRIEST || a.Sort == AT_SCOUT) continue;
                const double d = calDistance(a.DR, a.UR, tx, ty);
                if ((a.Sort == AT_COMPOSITE_BOWMAN || a.Sort == AT_BOWMAN) && d < brD) {
                    brD = d; bestRanged = a.SN;
                }
                if ((a.Sort == AT_CAVALRY || a.Sort == AT_CHARIOT) && d < bfD) {
                    bfD = d; bestFast = a.SN;
                }
            }
            baitScoutSN = (bestRanged != -1) ? bestRanged : bestFast;
            if (baitScoutSN >= 0) {
                for (tagArmy &a : info.armies)
                    if (a.SN == baitScoutSN) {
                        // 退回点在起点后方，避免刚选中就原地解除。
                        double rx = homeDR - a.DR, ry = homeUR - a.UR;
                        const double rl = sqrt(rx * rx + ry * ry);
                        baitScoutHomeDR = a.DR; baitScoutHomeUR = a.UR;
                        if (rl > 1e-6) {
                            baitScoutHomeDR += rx / rl * KITE_RETREAT_STEP * bsl;
                            baitScoutHomeUR += ry / rl * KITE_RETREAT_STEP * bsl;
                        }
                        scAlive = true; scDR = a.DR; scUR = a.UR; scBlood = a.Blood;
                        break;
                    }
                baitScoutBlood = -1;
                baitScoutFrame = 0;
                baitScoutReturning = false;
                baitScoutBack = false; baitScoutReason = "接近";
            }
        }
        // ---- 步骤 2 / 3 ----
        if (baitScoutSN >= 0 && scAlive) {
            double nd = 1e18, probeDR = tx, probeUR = ty;
            bool locked = false;
            for (tagArmy &e : info.enemy_armies) {
                const double d = calDistance(scDR, scUR, e.DR, e.UR);
                if (d < nd) { nd = d; probeDR = e.DR; probeUR = e.UR; }
                if (e.WorkObjectSN == baitScoutSN) locked = true;   // 有敌人正锁着它
            }
            const bool hurt = (baitScoutBlood >= 0 && scBlood < baitScoutBlood);  // 掉血 = 挨打
            const bool closeEnemy = nd < KITE_FLEE_DIST * bsl;
            const bool back = baitScoutReturning || closeEnemy || locked || hurt;
            baitScoutReason = locked ? "被锁定" : hurt ? "掉血"
                : closeEnemy ? "贴脸" : back ? "撤回" : "接近";
            baitScoutBack = back;                      // 供逐单位循环判断“现在该撤，别下攻击指令”
            if (back) baitScoutReturning = true;       // 记下“它退过”，才允许在初始位置摘标志
            const double homeDist = calDistance(scDR, scUR, baitScoutHomeDR, baitScoutHomeUR);
            if (baitScoutReturning && homeDist <= 2.0 * bsl) {
                // 步骤 3：回到初始位置（不管此刻身边有没有敌人）⇒ 摘标志 + 冷却
                baitScoutSN = -1; baitScoutReason = "冷却";
                baitScoutSwitchFrame = info.GameFrame + BAIT_SCOUT_SWITCH_MS / toFrames;
            } else if (info.GameFrame - baitScoutFrame >= BAIT_SCOUT_GAP) {
                bool canShoot = false;
                if (!back) {
                    for (tagArmy &a : info.armies) {
                        if (a.SN != baitScoutSN) continue;
                        if (a.Sort == AT_BOWMAN || a.Sort == AT_COMPOSITE_BOWMAN)
                            canShoot = nd <= own_attack_range(a.Sort) * bsl;
                        break;
                    }
                }
                double mx = baitScoutHomeDR, my = baitScoutHomeUR;
                if (!back && !canShoot) {
                    double dx = probeDR - scDR, dy = probeUR - scUR;
                    const double len = sqrt(dx * dx + dy * dy);
                    if (len > 1e-6) {
                        mx = scDR + dx / len * BAIT_SCOUT_STEP * bsl;
                        my = scUR + dy / len * BAIT_SCOUT_STEP * bsl;
                    }
                }
                int mbx = (int)(mx / bsl), mby = (int)(my / bsl);
                if (!landing_ok(mbx, mby, baitScoutSN)) {
                    // 目标格被占/站不住（水边、建筑旁）⇒ 在附近找一块能站的，找不到本帧不动
                    int fx = mbx, fy = mby;
                    if (find_free_spot_near(mbx, mby, 1, 6, fx, fy)
                        && landing_ok(fx, fy, baitScoutSN)) {
                        mbx = fx;
                        mby = fy;
                    }
                }
                if (!canShoot && !landing_ok(mbx, mby, baitScoutSN))
                    baitScoutReason = "无可用落点";
                if (!canShoot && landing_ok(mbx, mby, baitScoutSN)) {
                    HumanMove(baitScoutSN, (mbx + 0.5) * bsl, (mby + 0.5) * bsl);
                    baitScoutFrame = info.GameFrame;
                    // 突击者往前挪 ⇒ 其余部队跟着推进 1~2 格（步骤 2 后半句）
                    if (!back) {
                        // 随诱饵逐步推进，不设置敌营外的固定停留距离。
                        const double dx2 = probeDR - slowPushDR, dy2 = probeUR - slowPushUR;
                        const double len2 = sqrt(dx2 * dx2 + dy2 * dy2);
                        if (slowPushValid && len2 > 1e-6) {
                            const double step = std::min((double)SLOW_PUSH_STEP * bsl, len2);
                            slowPushDR += dx2 / len2 * step;
                            slowPushUR += dy2 / len2 * step;
                        }
                    }
                }
            }
            baitScoutBlood = scBlood;                  // 每帧更新，供下一帧比“掉血”
        }
    }

    if (assaultState >= 2 && info.GameFrame - siegePositionFrame >= std::max(1, 2000 / toFrames)) {
        siegePositionFrame = info.GameFrame;
        for (auto it = siegePositionSamples.begin(); it != siegePositionSamples.end();) {
            bool alive = false;
            for (const tagArmy &a : info.armies) if (a.SN == it->first) { alive = true; break; }
            if (alive) { ++it; continue; }
            DebugText(std::string("投石车消失: SN=") + std::to_string(it->first)
                + " 最后坐标=(" + std::to_string(it->second.dr / bsl) + "," + std::to_string(it->second.ur / bsl) + ")"
                + " 最后血=" + std::to_string(it->second.blood)
                + " 最后原因=" + siegeOrderReason[it->first] + " 最后判定=" + siegeDiagContext[it->first]);
            it = siegePositionSamples.erase(it);
        }
        for (const tagArmy &a : info.armies) {
            if (!army_is_siege(a.Sort)) continue;
            const auto old = siegePositionSamples.find(a.SN);
            const double moved = old == siegePositionSamples.end() ? 0
                : calDistance(a.DR, a.UR, old->second.dr, old->second.ur) / bsl;
            const int hpDelta = old == siegePositionSamples.end() ? 0 : a.Blood - old->second.blood;
            DebugText(std::string("投石车位置: SN=") + std::to_string(a.SN)
                + " 实际=(" + std::to_string(a.DR / bsl) + "," + std::to_string(a.UR / bsl)
                + ") 2s位移=" + std::to_string(moved)
                + " 终点=(" + std::to_string(a.DR0 / bsl) + "," + std::to_string(a.UR0 / bsl)
                + ") 距终点=" + std::to_string(calDistance(a.DR, a.UR, a.DR0, a.UR0) / bsl)
                + " 血=" + std::to_string(a.Blood) + " 血变化=" + std::to_string(hpDelta)
                + " 状态=" + std::to_string(a.NowState) + " 目标=" + std::to_string(a.WorkObjectSN)
                + " 阶段=" + std::to_string(assaultState) + " 集火塔=" + std::to_string(assaultFocusTower)
                + " 上帧判定=" + siegeDiagContext[a.SN]
                + " 原因=" + (siegeOrderReason.count(a.SN) ? siegeOrderReason[a.SN] : "尚未下令")
                + " 指令目标=" + (attackOrderSN.count(a.SN) ? std::to_string(attackOrderSN[a.SN]) : "无")
                + " 移动距今ms=" + std::to_string((info.GameFrame - unitStepFrame[a.SN]) * toFrames)
                + " 攻击距今ms=" + (unitFireFrame.count(a.SN)
                    ? std::to_string((info.GameFrame - unitFireFrame[a.SN]) * toFrames) : "无记录"));
            siegePositionSamples[a.SN] = {a.DR, a.UR, a.Blood};
        }
    }

    // ---- 7) 逐单位下令 ----
    const int stuckSampleInterval = (3000 / toFrames) < 1 ? 1 : (3000 / toFrames);

    int towerPositionRejected = 0, noTargetUnits = 0;
    // 选敌人：检查我方预计开火位置，而非敌人与塔/工程厂的距离。
    // siegeFirst：状态 2/3 里把投石车排到最前
    const bool siegeFirst = (assaultState >= 2 && assaultState <= 3);
    auto pickEnemy = [&](tagArmy &a, bool forbidRange, bool protectPriest,
                         int maxDist, bool nearestOnly = false) -> int {
        int bestSN = -1;
        double best = 1e18;
        for (tagArmy &e : info.enemy_armies) {
            if (reserve_wave3_siege(e)) continue;
            double d = calDistance(a.DR, a.UR, e.DR, e.UR);
            bool inReach = (d <= maxDist * bsl);
            if (!inReach && protectPriest && priestMayJoin && priest != nullptr)
                inReach = (calDistance(priest->DR, priest->UR, e.DR, e.UR)
                           <= ASSAULT_ENGAGE_DIST * bsl);
            if (!inReach) continue;
            if (forbidRange && d > own_attack_range(a.Sort) * bsl) {
                const double reach = own_attack_range(a.Sort) * bsl;
                const double fireDR = e.DR + (a.DR - e.DR) * reach / d;
                const double fireUR = e.UR + (a.UR - e.UR) * reach / d;
                if (point_in_enemy_tower_range(fireDR, fireUR, TOWER_SAFE_MARGIN)) {
                    ++towerPositionRejected;
                    continue;
                }
            }
            // 投石车优先：5 秒一发 50 点，而我们复合弓兵 45 血 ⇒ 挨一发就死。
            //   加成取 100 格（远大于 ASSAULT_ENGAGE_DIST）⇒ 在交战半径内就优先，
            //   但不影响“半径外不打”。只在状态 2/3 生效（状态 4 不能让队伍被拉走）。
            double sc = d;
            if (priestMayJoin && priest != nullptr && e.WorkObjectSN == priest->SN)
                sc -= 200.0 * bsl;
            if (!nearestOnly && siegeFirst && e.Sort == AT_STONE_THROWER) sc -= 100.0 * bsl;
            if (sc < best) { best = sc; bestSN = e.SN; }
        }
        return bestSN;
    };

    const bool advance = (assaultState >= 2);
    double advDR = tx, advUR = ty;
    if (assaultState >= 4 && haveHome) {
        double dx = homeDR - tx;
        double dy = homeUR - ty;
        double len = sqrt(dx * dx + dy * dy);
        if (len < 1e-6) { dx = -1.0; dy = 0.0; len = 1.0; }
        advDR = tx + dx / len * (ASSAULT_PROTECT_DIST * bsl);
        advUR = ty + dy / len * (ASSAULT_PROTECT_DIST * bsl);
    } else if (assaultState == 2 && slowPushValid) {
        advDR = slowPushDR; advUR = slowPushUR;
    }
    // 移动目标编码：-1 = 回集结点，-2 = 冲敌营，-4 = 回保护位，
    //                -6 = 压上勾引，-8 = 拉锯后撤（退回诱杀线）
    // （必须分开编号：否则状态切换时编码相同、内核“目标没变就不重下”会卡住）
    const int advCode = (assaultState >= 4) ? -4
                      : (assaultState == 3) ? -2
                      : -6;

    // 阵位中心改变时，旧移动编号不能代表目标坐标未变。
    if (advance && (lastAdvanceDR < 0
        || calDistance(lastAdvanceDR, lastAdvanceUR, advDR, advUR) >= bsl)) {
        for (auto it = attackOrderSN.begin(); it != attackOrderSN.end(); ) {
            if (it->second < 0) it = attackOrderSN.erase(it);
            else ++it;
        }
        lastAdvanceDR = advDR; lastAdvanceUR = advUR;
    }

    // 撤退优先于转化，避免同一帧的后续命令覆盖撤退。
    bool priestRetreating = false;
    if (priest != nullptr && priestMayJoin) {
        bool priestMayMove = true;
        bool danger = totalArmy < 3, safe = totalArmy >= 3;
        for (const tagArmy &e : info.enemy_armies) {
            const double d = calDistance(priest->DR, priest->UR, e.DR, e.UR) / bsl;
            const bool hunter = assault_priest_hunter(e.Sort);
            const bool locked = e.WorkObjectSN == priest->SN;
            if (locked || d < (hunter ? PRIEST_HUNTER_FLEE_DIST : own_attack_range(e.Sort) + 2))
                danger = true;
            if (locked || d < (hunter ? PRIEST_HUNTER_RESUME_DIST : own_attack_range(e.Sort) + 4))
                safe = false;
        }
        const bool forcingFactory = assaultState == 4 && towerCnt <= 2;
        if (!forcingFactory && assaultPriestBlood >= 0 && priest->Blood < assaultPriestBlood) danger = true;
        assaultPriestBlood = priest->Blood;
        if (!forcingFactory && point_in_enemy_tower_range(priest->DR, priest->UR, TOWER_SAFE_MARGIN)) {
            danger = true; safe = false;
        }
        if (forcingFactory && !danger) assaultPriestFlee = false;
        if (danger) { assaultPriestFlee = true; assaultPriestSafeFrame = 0; }
        if (assaultPriestFlee) {
            if (!safe) assaultPriestSafeFrame = 0;
            else if (assaultPriestSafeFrame == 0) assaultPriestSafeFrame = info.GameFrame;
            if (assaultPriestSafeFrame > 0
                && info.GameFrame - assaultPriestSafeFrame >= 3000 / toFrames)
                assaultPriestFlee = false;
        }
        bool chasing = false;
        if (priest->NowState == HUMAN_STATE_ATTACKING && attackConvertSN >= 0) {
            for (const tagArmy &e : info.enemy_armies)
                if (e.SN == attackConvertSN
                    && calDistance(priest->DR, priest->UR, e.DR, e.UR)
                        > static_cast<double>(DIS_PRIEST) * bsl) chasing = true;
        }
        if (assaultPriestFlee || chasing) {
            retreat_assault_priest(*priest);
            factoryConvertTarget = -1; factoryConvertId = -1;
            attackConvertSN = -1;
            priestMayMove = false;
            priestRetreating = true;
        } else if (enemy_near(homeDR, homeUR, 55.0 * bsl)) {
            priestMayMove = false;
        }
        // 最靠前的我方战斗兵（诱饵/自裁兵不算：诱饵本来就主动前压）
        double lineFrontDist = 1e18;
        for (tagArmy &o : info.armies) {
            if (o.Sort == AT_PRIEST || o.Sort == AT_SCOUT) continue;
            if (o.SN == baitScoutSN || o.SN == weakKillSN) continue;
            const double d = calDistance(o.DR, o.UR, tx, ty);
            if (d < lineFrontDist) lineFrontDist = d;
        }
        if (lineFrontDist > 1e17) priestMayMove = false;   // 我军已打光：祭司原地不动

        if (!advance) {
            if (priestMayMove) recall_priest_home(priest);
        } else if (priestMayMove && assaultState < 4 && (priest->NowState == HUMAN_STATE_IDLE
                   || priest->NowState == HUMAN_STATE_WALKING)) {
            // 跟随实际弓兵后排；已过时的回城移动也在此纠正。
            double bowDR = 0, bowUR = 0, bowRange = 0; int bowCount = 0;
            double frontDist = 1e18;
            for (const tagArmy &a : info.armies) {
                if (a.SN == baitScoutSN || a.SN == weakKillSN) continue;
                if (a.Sort != AT_BOWMAN && a.Sort != AT_COMPOSITE_BOWMAN) continue;
                frontDist = std::min(frontDist, calDistance(a.DR, a.UR, tx, ty));
            }
            for (const tagArmy &a : info.armies) {
                if (a.SN == baitScoutSN || a.SN == weakKillSN) continue;
                if (a.Sort != AT_BOWMAN && a.Sort != AT_COMPOSITE_BOWMAN) continue;
                if (calDistance(a.DR, a.UR, tx, ty) > frontDist + 2 * bsl) continue;
                bowDR += a.DR; bowUR += a.UR; bowRange += own_attack_range(a.Sort); ++bowCount;
            }
            if (bowCount > 0) { bowDR /= bowCount; bowUR /= bowCount; bowRange /= bowCount; }
            else { bowDR = priest->DR; bowUR = priest->UR; bowRange = 7; }
            const double backGap = std::max(2.0, std::min(5.0,
                static_cast<double>(DIS_PRIEST) - bowRange - 1.0));
            double ux = bowDR - tx, uy = bowUR - ty;
            const double ul = sqrt(ux * ux + uy * uy);
            if (ul > 1e-6) { bowDR += ux / ul * backGap * bsl; bowUR += uy / ul * backGap * bsl; }
            int pcx = (int)(bowDR / bsl), pcy = (int)(bowUR / bsl);
            const int phalf = 1;
            int pbx = -1, pby = -1;
            if (spread_slot(pcx, pcy, phalf, priest->SN, pbx, pby)
                && prepare_unit_reach(*priest) && spread_cell_reachable(pbx, pby)) {
                const double gx = (pbx + 0.5) * bsl, gy = (pby + 0.5) * bsl;
                bool deepOk = true;              // 闸③
                if (nearest_enemy_tower_dist(gx, gy) <= towerRange + TOWER_SAFE_MARGIN)
                    deepOk = false;              //   落点在敌方箭塔射程内
                if (calDistance(gx, gy, tx, ty) < lineFrontDist)
                    deepOk = false;              //   落点比最靠前的战斗兵还靠前
                if (enemy_near(gx, gy, 8.0 * bsl))
                    deepOk = false;              //   落点附近有敌人
                for (const tagArmy &e : info.enemy_armies)
                    if (assault_priest_hunter(e.Sort)
                        && calDistance(gx, gy, e.DR, e.UR) < PRIEST_HUNTER_FLEE_DIST * bsl)
                        deepOk = false;
                if (deepOk) {
                    const int mdx = priest->BlockDR - pbx, mdy = priest->BlockUR - pby;
                    const double dArea = calDistance(priest->DR, priest->UR,
                                                     (pcx + 0.5) * bsl, (pcy + 0.5) * bsl);
                    const bool oldDestination = priest->NowState == HUMAN_STATE_WALKING
                        && calDistance(priest->DR0, priest->UR0, gx, gy) > 3 * bsl
                        && dArea > 2 * bsl;
                    if ((mdx * mdx + mdy * mdy > 1 && dArea > 2 * bsl
                         && priest->NowState != HUMAN_STATE_WALKING)
                        || oldDestination) {
                        const int gap = 1000 / toFrames;      // 每 1 秒最多重下一次
                        if (priestOrderFrame == 0
                            || info.GameFrame - priestOrderFrame >= gap) {
                            HumanMove(priest->SN, gx, gy);
                            priestOrderFrame = info.GameFrame;
                        }
                    }
                }
            }
        }
    }

    for (tagArmy &a : info.armies) {
        // 祭司的格位已在上面 7.0) 分配（特殊兵种：格位中心往家退 4 格）
        if (a.Sort == AT_PRIEST) continue;
        if (a.Sort == AT_SCOUT) continue;   // 侦察骑兵只探路，不参加反攻
        if (a.SN == weakKillSN) continue;
        if (a.SN == baitScoutSN) {
            if (!baitScoutBack) {
                int target = -1;
                double nearest = own_attack_range(a.Sort) * bsl;
                for (tagArmy &e : info.enemy_armies) {
                    if (reserve_wave3_siege(e)) continue;
                    const double d = calDistance(a.DR, a.UR, e.DR, e.UR);
                    if (d <= nearest) { nearest = d; target = e.SN; }
                }
                if (target >= 0 && (a.WorkObjectSN != target || a.NowState == HUMAN_STATE_IDLE)) {
                    HumanAction(a.SN, target);
                    attackOrderSN[a.SN] = target;
                }
            }
            continue;
        }

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
                    bool clearOrder = (code == -1 || code == -2 || code == -4
                                       || code == -6 || code == -7 || code == -9 || code == -10 || code == -12);
                    if (code >= 0) {
                        double d = -1.0, targetHeight = assault_height(a.BlockDR, a.BlockUR);
                        for (const tagArmy &e : info.enemy_armies)
                            if (e.SN == code) {
                                d = std::max(fabs(a.DR - e.DR), fabs(a.UR - e.UR));
                                targetHeight = assault_height(e.BlockDR, e.BlockUR); break;
                            }
                        if (d < 0)
                            for (const tagBuilding &e : info.enemy_buildings)
                                if (e.SN == code) {
                                    const double x = (e.BlockDR + building_size(e.Type) * 0.5) * bsl;
                                    const double y = (e.BlockUR + building_size(e.Type) * 0.5) * bsl;
                                    d = std::max(fabs(a.DR - x), fabs(a.UR - y));
                                    targetHeight = assault_height(e.BlockDR, e.BlockUR); break;
                                }
                        const double range = own_attack_range(a.Sort)
                            + std::max(0.0, assault_height(a.BlockDR, a.BlockUR) - targetHeight);
                        if (d < 0 || d > (range + 0.5) * bsl) clearOrder = true;
                    }
                    if (clearOrder) {
                        attackOrderSN.erase(a.SN);
                        unitNeedsRecovery[a.SN] = 1;
                    }   // 下一帧重新下令（=重新寻路）
                }
                unitStuckKey[a.SN] = posKey;
                unitStuckFrame[a.SN] = info.GameFrame;
            }
        }

        const bool archer = (a.Sort == AT_BOWMAN || a.Sort == AT_COMPOSITE_BOWMAN);

        if (a.NowState == HUMAN_STATE_ATTACKING && !archer && !army_is_siege(a.Sort)) continue;

        int wantSN = -1;      // >=0 = 攻击目标；-1 = 没有可打的目标
        if (assaultState == 1) {
            wantSN = pickEnemy(a, true, false, ASSAULT_STAGE_GUARD_DIST, archer);
        } else if (assaultState == 2) {
            wantSN = pickEnemy(a, false, false, ASSAULT_ENGAGE_DIST, archer);
        } else if (assaultState == 3 || assaultState == 4) {
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

        if ((assaultState == 3 || assaultState == 4) && archer && wantSN == focusTower && haveHome
            && bow_attack_home_side(a, focusTower, homeDR, homeUR)) continue;

        if (army_is_siege(a.Sort)) {
            double targetDR = tx, targetUR = ty;
            bool targetVisible = false;
            for (const tagArmy &e : info.enemy_armies) {
                if (e.SN != wantSN) continue;
                targetDR = e.DR; targetUR = e.UR; targetVisible = true; break;
            }
            for (const tagBuilding &e : info.enemy_buildings) {
                if (e.SN != wantSN) continue;
                targetDR = (e.BlockDR + building_size(e.Type) * 0.5) * bsl;
                targetUR = (e.BlockUR + building_size(e.Type) * 0.5) * bsl;
                targetVisible = true; break;
            }
            const double range = std::min(static_cast<double>(VISION_STONE_THROWER),
                own_attack_range(a.Sort) + std::max(0,
                    assault_height(a.BlockDR, a.BlockUR)
                    - assault_height((int)(targetDR / bsl), (int)(targetUR / bsl)))) * bsl;
            const double minRange = static_cast<double>(DIS_MIN_STONE_THROWER) * bsl;
            const double targetDist = std::max(fabs(a.DR - targetDR), fabs(a.UR - targetUR));
            const double targetMinDist = calDistance(a.DR, a.UR, targetDR, targetUR);
            siegeDiagContext[a.SN] = "目标=" + std::to_string(wantSN)
                + " 可见=" + std::to_string(targetVisible)
                + " 坐标=(" + std::to_string(targetDR / bsl) + "," + std::to_string(targetUR / bsl) + ")"
                + " 距离=" + std::to_string(targetDist / bsl)
                + " 最小距=" + std::to_string(targetMinDist / bsl)
                + " 射程=[" + std::to_string(minRange / bsl) + "," + std::to_string(range / bsl) + "]"
                + " 恢复=" + std::to_string(unitNeedsRecovery.count(a.SN))
                + " 风险=" + std::to_string(siege_path_risk(a.DR, a.UR, -1));
            const auto previousBlood = siegeLastBlood.find(a.SN);
            const bool hurt = previousBlood != siegeLastBlood.end() && a.Blood < previousBlood->second;
            siegeLastBlood[a.SN] = a.Blood;
            const bool closeEnemy = enemy_near(a.DR, a.UR, 4 * bsl);
            siegeDiagContext[a.SN] += " 掉血=" + std::to_string(hurt) + " 近敌=" + std::to_string(closeEnemy);
            if (hurt || closeEnemy) siegeFleeUntil[a.SN] = info.GameFrame + 4000 / std::max(1, TimePerFrame);
            if (siegeFleeUntil.count(a.SN) && info.GameFrame < siegeFleeUntil[a.SN]) {
                siegeOrderReason[a.SN] = "受伤或近敌，沿低威胁路径撤离";
                const double escapeDR = haveHome ? homeDR : a.DR + (a.DR - targetDR);
                const double escapeUR = haveHome ? homeUR : a.UR + (a.UR - targetUR);
                if (!siege_safe_step(a, escapeDR, escapeUR, -1, true)
                    && a.NowState != HUMAN_STATE_IDLE
                    && info.GameFrame - unitStepFrame[a.SN] >= RANGED_STEP_GAP) {
                    siege_diag_order(HumanMove(a.SN, a.DR, a.UR), a.SN, -1, a.DR, a.UR); attackOrderSN[a.SN] = -12;
                    unitStepFrame[a.SN] = info.GameFrame;
                    siegeOrderReason[a.SN] = "撤离暂时无路，停止追击等待";
                }
                continue;
            }
            bool attackRejected = false;
            const auto pendingAttack = siegeAttackId.find(a.SN);
            if (pendingAttack != siegeAttackId.end()) {
                const auto receipt = info.ins_ret.find(pendingAttack->second);
                if (receipt != info.ins_ret.end()) {
                    attackRejected = receipt->second != ACTION_SUCCESS;
                    siegeAttackId.erase(pendingAttack);
                    if (attackRejected) unitNeedsRecovery[a.SN] = info.GameFrame;
                }
            }
            const int attackGap = std::max(RANGED_FIRE_GAP, 1000 / std::max(1, TimePerFrame));
            auto fireSiege = [&]() {
                if (unitFireFrame.count(a.SN)
                    && info.GameFrame - unitFireFrame[a.SN] < attackGap) return;
                siegeAttackId[a.SN] = siege_diag_order(HumanAction(a.SN, wantSN),
                    a.SN, wantSN, targetDR, targetUR);
                unitFireFrame[a.SN] = info.GameFrame;
                attackOrderSN[a.SN] = wantSN;
                unitNeedsRecovery.erase(a.SN);
            };
            if (targetVisible && a.WorkObjectSN == wantSN
                && targetMinDist >= minRange && targetDist <= range
                && a.NowState != HUMAN_STATE_IDLE) {
                siegeOrderReason[a.SN] = "保留有效射击位，不追随远端补兵";
                if (unitNeedsRecovery.count(a.SN)) fireSiege();
                continue;
            }
            double bowDR = 0, bowUR = 0; int bowCount = 0;
            double nearestBow = 1e18;
            for (const tagArmy &o : info.armies) {
                if (o.SN == baitScoutSN || o.SN == weakKillSN) continue;
                if (o.Sort != AT_BOWMAN && o.Sort != AT_COMPOSITE_BOWMAN) continue;
                nearestBow = std::min(nearestBow, calDistance(o.DR, o.UR, targetDR, targetUR));
            }
            for (const tagArmy &o : info.armies) {
                if (o.SN == baitScoutSN || o.SN == weakKillSN) continue;
                if (o.Sort != AT_BOWMAN && o.Sort != AT_COMPOSITE_BOWMAN) continue;
                if (calDistance(o.DR, o.UR, targetDR, targetUR) > nearestBow + 3 * bsl) continue;
                bowDR += o.DR; bowUR += o.UR; ++bowCount;
            }
            if (bowCount > 0) {
                bowDR /= bowCount; bowUR /= bowCount;
                double ux = targetDR - bowDR, uy = targetUR - bowUR;
                const double ul = sqrt(ux * ux + uy * uy);
                if (ul > 1e-6) {
                    ux /= ul; uy /= ul;
                    const double behind = (bowDR - a.DR) * ux + (bowUR - a.UR) * uy;
                    siegeDiagContext[a.SN] += " 前排人数=" + std::to_string(bowCount)
                        + " 前排=(" + std::to_string(bowDR / bsl) + "," + std::to_string(bowUR / bsl) + ")"
                        + " 落后格=" + std::to_string(behind / bsl);
                    const bool firing = a.NowState == HUMAN_STATE_ATTACKING
                        && a.WorkObjectSN == wantSN;
                    // 开火至少落后弓兵 2 格；正在攻击时保留 1 格余量，减少边界抖动。
                    const double fireBack = (firing ? 1.0 : (double)SIEGE_BACK_DIST) * bsl;
                    if (behind >= fireBack && targetVisible
                        && targetMinDist >= minRange && targetDist <= range) {
                        siegeOrderReason[a.SN] = "位于弓兵后排，保持攻击";
                        if (a.WorkObjectSN != wantSN || a.NowState == HUMAN_STATE_IDLE
                            || unitNeedsRecovery.count(a.SN)) {
                            fireSiege();
                        }
                        continue;
                    }
                    const double rearDR = bowDR - ux * SIEGE_BACK_DIST * bsl;
                    const double rearUR = bowUR - uy * SIEGE_BACK_DIST * bsl;
                    if (info.GameFrame - unitStepFrame[a.SN] < RANGED_STEP_GAP) continue;
                    if (!prepare_unit_reach(a)) {
                        siegeOrderReason[a.SN] = "起点连通搜索失败"; continue;
                    }
                    const int cx = (int)(rearDR / bsl), cy = (int)(rearUR / bsl);
                    int mbx = -1, mby = -1; double best = 1e18;
                    for (int dx = -6; dx <= 6; ++dx) {
                        for (int dy = -6; dy <= 6; ++dy) {
                            const int bx = cx + dx, by = cy + dy;
                            const double gx = (bx + 0.5) * bsl, gy = (by + 0.5) * bsl;
                            const double behind = (bowDR - gx) * ux + (bowUR - gy) * uy;
                            if (behind < SIEGE_BACK_DIST * bsl) continue;
                            if (!landing_ok(bx, by, a.SN) || !spread_cell_reachable(bx, by)) continue;
                            const double d = std::max(fabs(gx - targetDR), fabs(gy - targetUR));
                            double score = calDistance(gx, gy, rearDR, rearUR);
                            if (targetVisible) {
                                if (calDistance(gx, gy, targetDR, targetUR) < minRange) continue;
                                const double candidateRange = std::min(static_cast<double>(VISION_STONE_THROWER),
                                    own_attack_range(a.Sort) + std::max(0,
                                        assault_height(bx, by)
                                        - assault_height((int)(targetDR / bsl), (int)(targetUR / bsl)))) * bsl;
                                if (d > candidateRange) score += 20 * bsl + 4 * (d - candidateRange);
                            }
                            if (score < best) { best = score; mbx = bx; mby = by; }
                        }
                    }
                    siegeOrderReason[a.SN] = "跟随后排阵位";
                    if (mbx < 0) {
                        siegeOrderReason[a.SN] = "远端后排不可达，局部接近";
                        double localBest = calDistance(a.DR, a.UR, rearDR, rearUR);
                        for (int dx = -8; dx <= 8; ++dx) {
                            for (int dy = -8; dy <= 8; ++dy) {
                                const int bx = a.BlockDR + dx, by = a.BlockUR + dy;
                                if (!landing_ok(bx, by, a.SN) || !spread_cell_reachable(bx, by)) continue;
                                const double gx = (bx + 0.5) * bsl, gy = (by + 0.5) * bsl;
                                if ((bowDR - gx) * ux + (bowUR - gy) * uy < SIEGE_BACK_DIST * bsl) continue;
                                if ((assaultState != 3 && assaultState != 4 && point_in_enemy_tower_range(gx, gy, TOWER_SAFE_MARGIN))
                                    || enemy_near(gx, gy, 4 * bsl)) continue;
                                const double score = calDistance(gx, gy, rearDR, rearUR);
                                if (score < localBest - 0.25 * bsl) {
                                    localBest = score; mbx = bx; mby = by;
                                }
                            }
                        }
                        if (mbx < 0) siegeOrderReason[a.SN] = "远端及局部后排均无可用落点";
                    }
                    siegeDiagContext[a.SN] += " 后排落点=(" + std::to_string(mbx) + "," + std::to_string(mby) + ")";
                    if (mbx >= 0) {
                        const double gx = (mbx + 0.5) * bsl, gy = (mby + 0.5) * bsl;
                        const bool sameMove = a.NowState == HUMAN_STATE_WALKING
                            && calDistance(a.DR0, a.UR0, gx, gy) <= bsl;
                        if ((!sameMove || unitNeedsRecovery.count(a.SN))
                            && (a.NowState == HUMAN_STATE_ATTACKING
                                || calDistance(a.DR, a.UR, gx, gy) > 0.5 * bsl)) {
                            const int ignoredTower = (assaultState == 3 || assaultState == 4) && wantSN == focusTower ? focusTower : -1;
                            if (!siege_safe_step(a, gx, gy, ignoredTower, false))
                                siegeOrderReason[a.SN] = "后排寻路未找到有效推进点，保持位置";
                        }
                        cellClaim[(mbx << 12) | mby] = a.SN;
                    }
                    continue;
                }
            }
            siegeOrderReason[a.SN] = "无有效弓兵前排，保持位置";
            if (a.NowState != HUMAN_STATE_IDLE && attackOrderSN[a.SN] != -11) {
                siege_diag_order(HumanMove(a.SN, a.DR, a.UR), a.SN, -1, a.DR, a.UR);
                attackOrderSN[a.SN] = -11; unitStepFrame[a.SN] = info.GameFrame;
            }
            continue;
        }

        if (wantSN < 0) ++noTargetUnits;
        // 诱饵已独立处理，其余弓兵统一拉扯。
        if (archer && kite_archer_step(a, wantSN)) continue;

        // 没有攻击目标 → advance 时直接冲向敌营，否则回集结点（在塔射程之外）待命。
        // 只有状态 1 使用初始集结点，状态 2 随诱饵推进。
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
            int cx, cy, half;
            if (advance)       { cx = (int)(advDR / bsl); cy = (int)(advUR / bsl); half = ASSAULT_SPREAD_HALF; }
            else if (useRally) { cx = rallyBX;            cy = rallyBY;            half = rallyHalf; }
            else               { cx = (int)(sx / bsl);    cy = (int)(sy / bsl);    half = ASSAULT_SPREAD_HALF; }

            if (army_is_siege(a.Sort) && haveHome) {
                double ux = homeDR - tx, uy = homeUR - ty;   // 敌营 → 家
                const double ul = sqrt(ux * ux + uy * uy);
                if (ul > 1e-6) {
                    cx += (int)lround(ux / ul * SIEGE_BACK_DIST);
                    cy += (int)lround(uy / ul * SIEGE_BACK_DIST);
                }
            }

            int mbx = -1, mby = -1;
            if (!spread_slot(cx, cy, half, a.SN, mbx, mby)) continue;
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
            std::unordered_map<int,int>::iterator itStep = unitStepFrame.find(a.SN);
            if (itStep != unitStepFrame.end() && itStep->second == info.GameFrame) nMoved++;
            if (calDistance(a.DR, a.UR, advDR, advUR)
                <= 3.0 * bsl) nStaged++;
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
                  + " 敌剩=" + std::to_string(enemy_remains())
                  + "/" + std::to_string(enemy_ledger_total())
                  + " 突击=" + (baitScoutSN < 0 ? std::string("-") : std::to_string(baitScoutSN))
                  + " 推进=" + (slowPushValid
                        ? ("(" + std::to_string((int)(slowPushDR / bsl)) + ","
                              + std::to_string((int)(slowPushUR / bsl)) + ")")
                        : std::string("-"))
                  + " 无敌人=" + std::to_string(enemyClearSinceFrame == 0 ? 0
                        : (info.GameFrame - enemyClearSinceFrame) * toFrames / 1000)
                  + "s"
                  + " 箭塔=" + std::to_string(towerCnt)
                  + " 猎手=" + std::to_string(enemyHunters)
                  + " 到位=" + std::to_string(nStaged)
                  + "/" + std::to_string(nTotal)
                  + " 部队: 停=" + std::to_string(nIdle)
                  + " 走=" + std::to_string(nWalk)
                  + " 打=" + std::to_string(nAtk)
                  + " 忙=" + std::to_string(nWork)
                  + " 位移=" + std::to_string(nMoved)
                  + " 无目标兵=" + std::to_string(noTargetUnits)
                  + " 塔位过滤=" + std::to_string(towerPositionRejected)
                  + " 诱饵状态=" + baitScoutReason
                  + " 祭司=" + (priestRetreating ? std::string("撤退") :
                      (priestMayJoin ? std::string("有限参与") : std::string("待命"))));
    }

    if (priest == nullptr || !priestMayJoin || priestRetreating) return;
    if (bt_enemy_at_home()) return;   // 家里被打：祭司交给 combat_tactic

    const bool convertReady = (priest->ConvertCooldown <= 0);   // 冷却中就别下令（白费一次）

    // ① 选转化目标
    int convSN = -1;
    double convScore = -1e18;
    for (tagArmy &e : info.enemy_armies) {
        const double d = calDistance(priest->DR, priest->UR, e.DR, e.UR) / bsl;
        if (d > std::min((double)PRIEST_CONVERT_RADIUS, static_cast<double>(DIS_PRIEST)))
            continue; // 只转化当前射程内目标，不追击。
        double score = -d;                                // 越近越好
        if (e.Sort == AT_CAVALRY || e.Sort == AT_CHARIOT
            || e.Sort == AT_CHARIOT_ARCHER)      score += 1000.0;   // ① 祭司猎手
        else if (e.Sort == AT_HOPLITE)           score += 800.0;    // ② 学院兵（方阵兵）
        else if (e.Sort == AT_STONE_THROWER)     score += 1200.0;    // ③ 投石车
        if (score > convScore) { convScore = score; convSN = e.SN; }
    }

    if (assaultState >= 4 && haveBase && enemySiegeSN >= 0
        && (convSN < 0 || factoryConvertTarget == enemySiegeSN)) {
        const tagBuilding *factory = nullptr;
        for (const tagBuilding &b : info.enemy_buildings)
            if (b.SN == enemySiegeSN) { factory = &b; break; }
        if (factory == nullptr || factory->Percent < 100) return;
        const int size = building_size(factory->Type);
        const double centerDR = (factory->BlockDR + size * 0.5) * bsl;
        const double centerUR = (factory->BlockUR + size * 0.5) * bsl;
        const double workDist = size * 0.5 * bsl + 2 * static_cast<double>(CRASHBOX_SINGLEOB);
        const bool adjacent = fabs(priest->DR - centerDR) <= workDist
            && fabs(priest->UR - centerUR) <= workDist;
        if (adjacent) {
            siegePriestStuckFrame = 0;
            const bool latched = factoryConvertTarget == enemySiegeSN;
            const bool grace = latched && info.GameFrame - factoryConvertFrame < 8000 / toFrames;
            const auto receipt = info.ins_ret.find(factoryConvertId);
            const bool rejected = receipt != info.ins_ret.end() && receipt->second != ACTION_SUCCESS;
            const bool stillActive = priest->NowState == HUMAN_STATE_ATTACKING
                && (priest->WorkObjectSN == enemySiegeSN || attackConvertSN == enemySiegeSN);
            if (grace || (latched && !rejected && stillActive)) return;
            if (convertReady) {
                factoryConvertId = HumanAction(priest->SN, enemySiegeSN);
                factoryConvertTarget = enemySiegeSN;
                factoryConvertFrame = info.GameFrame;
                attackConvertSN = enemySiegeSN;
                DebugText("武器厂转化: 已贴邻，开始转化(2~6s)，保持指令");
            }
            return;
        }
        // 先走到明确满足内核工作距离的厂边位置，接近过程不能当作施法。
        bool stuck = false;
        if (siegePriestStuckFrame == 0) {
            siegePriestStuckFrame = info.GameFrame;
            siegePriestStuckDR = priest->DR; siegePriestStuckUR = priest->UR;
        } else if (info.GameFrame - siegePriestStuckFrame >= 3000 / toFrames) {
            stuck = calDistance(priest->DR, priest->UR, siegePriestStuckDR, siegePriestStuckUR) < 0.5 * bsl;
            siegePriestStuckFrame = info.GameFrame;
            siegePriestStuckDR = priest->DR; siegePriestStuckUR = priest->UR;
        }
        if (!prepare_unit_reach(*priest)) return;
        double best = 1e18, gxBest = 0, gyBest = 0; int bxBest = -1, byBest = -1;
        for (int bx = factory->BlockDR - 1; bx <= factory->BlockDR + size; ++bx) {
            for (int by = factory->BlockUR - 1; by <= factory->BlockUR + size; ++by) {
                if (!landing_ok(bx, by, priest->SN) || !spread_cell_reachable(bx, by)) continue;
                const double gx = std::max(centerDR - workDist + 1.0,
                    std::min(centerDR + workDist - 1.0, (bx + 0.5) * bsl));
                const double gy = std::max(centerUR - workDist + 1.0,
                    std::min(centerUR + workDist - 1.0, (by + 0.5) * bsl));
                if ((int)(gx / bsl) != bx || (int)(gy / bsl) != by) continue;
                if ((towerCnt > 2 && point_in_enemy_tower_range(gx, gy, TOWER_SAFE_MARGIN))
                    || enemy_near(gx, gy, 8 * bsl)) continue;
                bool hunterNear = false;
                for (const tagArmy &e : info.enemy_armies)
                    if (assault_priest_hunter(e.Sort)
                        && calDistance(gx, gy, e.DR, e.UR) < PRIEST_HUNTER_FLEE_DIST * bsl)
                        hunterNear = true;
                if (hunterNear) continue;
                double score = calDistance(priest->DR, priest->UR, gx, gy);
                if (stuck && calDistance(priest->DR0, priest->UR0, gx, gy) < bsl) score += 20 * bsl;
                if (score < best) { best = score; gxBest = gx; gyBest = gy; bxBest = bx; byBest = by; }
            }
        }
        if (bxBest >= 0 && (stuck || priest->NowState != HUMAN_STATE_WALKING
            || calDistance(priest->DR0, priest->UR0, gxBest, gyBest) > bsl)) {
            HumanMove(priest->SN, gxBest, gyBest);
            factoryConvertTarget = -1; factoryConvertId = -1;
            attackConvertSN = -1;
            cellClaim[(bxBest << 12) | byBest] = priest->SN;
            DebugText(std::string("武器厂转化: 接近厂边 ") + std::to_string(bxBest) + ","
                + std::to_string(byBest) + (stuck ? " 卡住换位" : ""));
        }
        return;
    }

    if (priest->NowState == HUMAN_STATE_ATTACKING && attackConvertSN >= 0) {
        for (tagArmy &e : info.enemy_armies)
            if (e.SN == attackConvertSN) return;
        if (assaultState >= 4 && attackConvertSN == enemySiegeSN) return;
    }

    // ② 有得转化就先转化（转化要时间，尽量别被移动指令打断）
    if (convertReady && convSN != -1
        && (attackConvertSN != convSN || priest->NowState == HUMAN_STATE_IDLE)) {
        attackConvertSN = convSN;
        HumanAction(priest->SN, convSN);
        return;
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
            if (haveHead && dot < 0.0) score -= SCOUT_DFS_BACK_W;

            if (score > bestScore) {
                bestScore = score;
                bestX = i;
                bestY = j;
            }
        }
    }

    if (bestX < 0) return false;      // 附近已经没有可去的前沿格了

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

    double healHomeDR = 0, healHomeUR = 0;
    const bool healHomeOk = home_center(healHomeDR, healHomeUR);
    const double maxHomeDist = HEAL_MAX_HOME_DIST * bsl;
    auto nearHomeOk = [&](double dr, double ur) -> bool {
        return !healHomeOk
               || calDistance(healHomeDR, healHomeUR, dr, ur) <= maxHomeDist;
    };

    // ---- 选目标：**离祭司最近的伤兵**，选中就一路治到满血 ----
    int target = -1;
    if (healTargetSN >= 0) {
        for (tagArmy &a : info.armies) {
            if (a.SN != healTargetSN) continue;
            if (a.Sort != AT_PRIEST && a.Sort != AT_SCOUT
                && a.MaxBlood > 0 && a.Blood < a.MaxBlood
                && calDistance(priest->DR, priest->UR, a.DR, a.UR) <= maxDist
                && nearHomeOk(a.DR, a.UR))          // ★ 跑出“家门口”就放弃它
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
            if (!nearHomeOk(a.DR, a.UR)) continue;                    // ★ 不在家门口
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

    const bool priestHealing = (phase < 3) && (priest != nullptr) && scoutIsUnit && priest_heal(priest);

    if (scoutIsUnit && phase < 3) {
        recall_priest_home(scout);
        return;
    }

    // （敌方位置 / 武器工程厂的记录已统一挪到 record_enemy_positions()：由 bt_sync 每帧调用，
    //   但**只在第三阶段（侦察骑兵出门探图之后）真正记录**，看到建筑/部队都算。）

    if (phase >= 3 && !scoutIsUnit) return;

    // ---- 第三阶段：侦察骑兵的活干完了 —— **不躲避，直接扎进敌军** ----
    if (phase >= 3 && scoutIsUnit && enemySiegeSN != -1) {
        HumanAction(scout->SN, scout->SN);
        return;
    }

    // 探路者不是祭司时，把祭司收回村待命（治疗/防守都在家附近做）。
    // 两个保护：只在它手里没活干时下令；有敌袭时不下（否则会覆盖掉
    // combat_tactic 同一帧刚下的转化指令，这个坑之前踩过）。
    if (phase < 3 && scoutIsUnit && priest != nullptr && !priestHealing
        && !bt_enemy_at_home() && priest->NowState == HUMAN_STATE_IDLE) {
        recall_priest_home(priest);
    }

    // ---- 敌方正在打我方的家：祭司交给 combat_tactic（箭塔拉仇恨 + 转化）----
    // 这一段必须放在"时间到回村"之前，否则回村指令会把同一帧刚下的转化指令覆盖掉。
    // 只有祭司还在外面很远时才叫它回村，已经在塔/中心附近就让它专心转化。
    if (bt_enemy_at_home()) {
        // 祭司必须回村防守（转化），不管它是不是探路者
        if (phase < 3 && priest != nullptr) {
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
    if (!scoutIsUnit && !atHome
        && enemy_near(scout->DR, scout->UR, SCOUT_THREAT_RADIUS * BLOCKSIDELENGTH)) {
        scout_retreat(scout);
        return;
    }

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

bool build_margin_clear(int x, int y, int size)
{
    for (int i = x - 1; i <= x + size; i++) {
        for (int j = y - 1; j <= y + size; j++) {
            if (i >= x && i < x + size && j >= y && j < y + size) continue;  // 本体跳过
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

static bool gather_target_reachable(const tagFarmer &f, int sn, int bx, int by, int size)
{
    const long long key = ((long long)f.SN << 32) | (unsigned int)sn;
    const auto blocked = gatherBlockedUntil.find(key);
    if (blocked != gatherBlockedUntil.end() && blocked->second > info.GameFrame) return false;
    tagArmy walker;
    walker.BlockDR = f.BlockDR; walker.BlockUR = f.BlockUR;
    if (!prepare_unit_reach(walker)) return false;
    for (int x = bx - 1; x <= bx + size; ++x)
        for (int y = by - 1; y <= by + size; ++y)
            if ((x < bx || x >= bx + size || y < by || y >= by + size)
                && rally_ground_ok(x, y) && spread_cell_reachable(x, y)) return true;
    return false;
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

    auto find_idle = [&](const Task *buildTask = nullptr) -> tagFarmer* {
        for (tagFarmer &f : info.farmers) {
            if (f.FarmerSort != FARMERTYPE_FARMER) continue;
            if (assignedThisFrame.count(f.SN)) continue;
            // 正负责一个还没建完的建造任务：绝不能被派去干别的。
            // 一旦被拉走，那栋楼就永远烂尾（同位置不能再下建造单，见 on_build_task 注释）。
            if (on_build_task(f.SN)) continue;
            if (!farmer_available(f)) continue;
            if (buildTask != nullptr) {
                const auto bad = buildTask->badBuilders.find(f.SN);
                if (bad != buildTask->badBuilders.end() && bad->second > info.GameFrame) continue;
            }
            return &f;
        }
        return nullptr;
    };

    for (Task &t : taskQueue) {
        if (t.state != TASK_WAITING) continue;

        if (t.type == TASK_GATHER) {
            // ---- 农田特殊处理：只派"这块田的主人"，不随便挑空闲村民 ----
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
                    if (!gather_target_reachable(*owner, b.SN, b.BlockDR, b.BlockUR, building_size(b.Type))) {
                        farmHolder.erase(b.SN); continue;
                    }
                    farmSN = b.SN;
                    farmerSN = owner->SN;
                    break;
                }

                if (farmSN == -1) {
                    tagFarmer *f = find_idle();
                    if (f == nullptr) continue;
                    double best = 1e18;
                    for (tagBuilding &b : info.buildings) {
                        if (b.Type != BUILDING_FARM) continue;
                        if (b.Percent < 100 || b.Cnt <= 0) continue;
                        if (lockedRes.count(b.SN)) continue;
                        if (farmHolder.find(b.SN) != farmHolder.end()) continue;
                        if (!gather_target_reachable(*f, b.SN, b.BlockDR, b.BlockUR, building_size(b.Type))) continue;
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
                for (int pass = 0; pass < 2 && resSN == -1; pass++) {
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
                        if (!gather_target_reachable(*f, r.SN, r.BlockDR, r.BlockUR, 1)) continue;
                        const int used = gatherers_on(r.SN);
                        if (pass == 0) {
                            if (used >= GATHER_PER_RESOURCE_MAX) continue;
                            if (used >= spots) continue;
                        }
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
            tagFarmer *f = find_idle(&t);
            if (f == nullptr) continue;

            if (t.blockDR != -1) {
                for (tagBuilding &b : info.buildings) {
                    if (b.Type != t.buildingType) continue;
                    if (b.BlockDR != t.blockDR || b.BlockUR != t.blockUR) continue;
                    if (b.Percent >= 100) { t.state = TASK_DONE; break; }  // 已经完工了
                    // 续建者必须能走到工地外围，避免换人后又挑中隔水的工人。
                    tagArmy reachWorker;
                    reachWorker.BlockDR = f->BlockDR; reachWorker.BlockUR = f->BlockUR;
                    bool canReach = false;
                    if (prepare_unit_reach(reachWorker)) {
                        const int size = building_size(b.Type);
                        for (int bx = b.BlockDR - 1; bx <= b.BlockDR + size; ++bx)
                            for (int by = b.BlockUR - 1; by <= b.BlockUR + size; ++by)
                                if ((bx < b.BlockDR || bx >= b.BlockDR + size
                                    || by < b.BlockUR || by >= b.BlockUR + size)
                                    && rally_ground_ok(bx, by) && spread_cell_reachable(bx, by))
                                    canReach = true;
                    }
                    if (!canReach) {
                        t.badBuilders[f->SN] = info.GameFrame + 60000 / TimePerFrame;
                        break;
                    }
                    t.targetSN = HumanAction(f->SN, b.SN);   // 续建
                    t.farmerSN = f->SN;
                    t.state = TASK_ASSIGNED;
                    t.startFrame = info.GameFrame;
                    t.resendFrame = 0;
                    t.resendCount = 0;
                    t.progressFrame = -1;
                    assignedThisFrame.insert(f->SN);
                    break;
                }
                // 已完工（等 recycle_tasks 清掉）或已派去续建 → 都不要另选新址
                if (t.state == TASK_DONE || t.state == TASK_ASSIGNED) continue;
                bool foundationExists = false;
                for (const tagBuilding &b : info.buildings)
                    if (b.Type == t.buildingType && b.BlockDR == t.blockDR && b.BlockUR == t.blockUR)
                        foundationExists = true;
                if (foundationExists) continue;
                // 工地上什么都没有（指令丢了/被拆了）→ 按下面的正常流程重选位置
            }

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

            int ax = -1, ay = -1;
            bool houseOutward = false;     // 房屋：是否只要"比锚点更贴地图边"的落点
            int  houseEdge = 0;            // 锚点到最近地图边的距离（房屋外扩判定用）

            // 市中心块坐标：房屋"往外建"和箭塔"聚群"都要拿它当参考
            int cx = -1, cy = -1;
            for (tagBuilding &b : info.buildings) {
                if (b.Type == BUILDING_CENTER) { cx = b.BlockDR; cy = b.BlockUR; break; }
            }

            if (t.granaryFarm) {
                double nearest = 1e18;
                for (const tagBuilding &b : info.buildings) {
                    if (b.Type != BUILDING_GRANARY || b.Percent < 100) continue;
                    const double d = calDistance(f->DR, f->UR,
                        b.BlockDR * BLOCKSIDELENGTH, b.BlockUR * BLOCKSIDELENGTH);
                    if (d < nearest) { nearest = d; ax = b.BlockDR; ay = b.BlockUR; }
                }
                if (ax < 0) continue;
            }
            if (t.buildingType == BUILDING_HOME) {
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
            int xBak = -1, yBak = -1;
            auto trySite = [&](int bx, int by, bool strictMargin) {
                if (x != -1) return;
                if (t.granaryFarm) {
                    // 谷仓和市中心的横纵通道各留三格，新增田不得占用。
                    auto blocksPassage = [&](int px, int py) {
                        return (bx <= px + 2 && bx + size > px)
                            || (by <= py + 2 && by + size > py);
                    };
                    if (blocksPassage(ax, ay)
                        || (cx >= 0 && blocksPassage(cx, cy))) return;
                    // 保留市中心原七块田及空缺出入口所在的整圈区域。
                    if (cx >= 0 && bx < cx + 8 && bx + size > cx - 5
                        && by < cy + 8 && by + size > cy - 5) return;
                }
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
                if (t.granaryFarm && !reachUsable) return;
                if (reachUsable && !site_reachable(bx, by, size)) {
                    if (xBak == -1) { xBak = bx; yBak = by; }
                    return;
                }
                x = bx;
                y = by;
            };

            if (t.granaryFarm) {
                // 四个象限优先；无合法位置就等待，不退化为远处随意建田。
                for (int ring = 1; ring <= 2 && x == -1; ++ring)
                    for (int gi = -ring; gi <= ring && x == -1; ++gi)
                        for (int gj = -ring; gj <= ring && x == -1; ++gj) {
                            if (gi == 0 || gj == 0) continue;
                            if (abs(gi) != ring && abs(gj) != ring) continue;
                            trySite(ax + gi * GRANARY_FARM_PITCH,
                                    ay + gj * GRANARY_FARM_PITCH, true);
                        }
            }
            if (!t.granaryFarm && cx >= 0 && cy >= 0 && ax == cx && ay == cy) {
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
            for (int pass = 0; !t.granaryFarm && pass < 3 && x == -1; ++pass) {
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
            if (!t.granaryFarm && x == -1 && xBak != -1) { x = xBak; y = yBak; }
            if (x == -1) continue;   // 找不到空地，保持等待

            if (t.granaryFarm) granaryFarmSites.insert((x << 12) | y);
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

    for (int sn : assignedThisFrame) mark_farmer_order(sn);
}

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
        mark_farmer_order(farmerSN);
        return;
    }
    double hx = 0, hy = 0;
    if (home_center(hx, hy)) {
        HumanMove(farmerSN, hx, hy);
        mark_farmer_order(farmerSN);
    }
}

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

static int foodFarmSweepFrame = -1000000;
static void recall_food_gatherers_to_farm()
{
    const int tpf = (TimePerFrame > 0) ? TimePerFrame : 40;
    if (info.GameFrame - foodFarmSweepFrame < STONE_SWEEP_MS / tpf) return;
    foodFarmSweepFrame = info.GameFrame;

    std::set<int> occupied;
    for (const auto &holder : farmHolder) occupied.insert(holder.first);
    for (const Task &t : taskQueue)
        if (t.type == TASK_GATHER && t.resourceType == GATHER_FARM
            && t.state == TASK_ASSIGNED && t.targetSN >= 0) occupied.insert(t.targetSN);
    for (const tagFarmer &f : info.farmers)
        if (f.WorkObjectSN >= 0) occupied.insert(f.WorkObjectSN);

    int woodWorkers = 0, goldWorkers = 0;
    for (const tagFarmer &f : info.farmers) {
        const int type = resource_type_by_sn(f.WorkObjectSN);
        if (type == RESOURCE_TREE) ++woodWorkers;
        if (type == RESOURCE_GOLD) ++goldWorkers;
    }
    std::set<int> moved;
    for (const tagBuilding &b : info.buildings) {
        if (b.Type != BUILDING_FARM || b.Percent < 100 || b.Cnt <= 0
            || occupied.count(b.SN)) continue;
        int worker = -1, workerType = -1;
        for (int pass = 0; pass < 3 && worker < 0; ++pass)
            for (tagFarmer &f : info.farmers) {
                if (f.FarmerSort != FARMERTYPE_FARMER || moved.count(f.SN)
                    || on_build_task(f.SN) || farmer_just_ordered(f.SN)) continue;
                const int type = resource_type_by_sn(f.WorkObjectSN);
                const bool idle = f.NowState == HUMAN_STATE_IDLE;
                const bool wild = type == RESOURCE_BUSH || type == RESOURCE_GAZELLE;
                const bool wood = type == RESOURCE_TREE && woodWorkers > WOOD_MIN_GATHERERS;
                const bool gold = type == RESOURCE_GOLD && (goldWorkers > 2 || !gold_needed());
                if ((pass == 0 && (idle || wild)) || (pass == 1 && wood) || (pass == 2 && gold)) {
                    worker = f.SN; workerType = type; break;
                }
            }
        if (worker < 0) break;
        for (Task &t : taskQueue)
            if (t.type == TASK_GATHER && t.farmerSN == worker
                && t.state != TASK_DONE && t.state != TASK_FAILED) t.state = TASK_DONE;
        HumanAction(worker, b.SN);
        mark_farmer_order(worker);
        farmHolder[b.SN] = worker;
        Task t;
        t.id = nextTaskId++; t.type = TASK_GATHER; t.priority = 3;
        t.resourceType = GATHER_FARM; t.targetSN = b.SN; t.farmerSN = worker;
        t.state = TASK_ASSIGNED; t.startFrame = info.GameFrame;
        taskQueue.push_back(t);
        moved.insert(worker); occupied.insert(b.SN);
        if (workerType == RESOURCE_TREE) --woodWorkers;
        if (workerType == RESOURCE_GOLD) --goldWorkers;
    }
}

// 按内核真值（WorkObjectSN）查一个资源 SN 是什么类型；查不到返回 -1。
// 用于“这个村民现在到底在干什么”（打猎 / 伐木 / 采金 / 采石）。
static int resource_type_by_sn(int sn)
{
    for (tagResource &r : info.resources)
        if (r.SN == sn) return r.Type;
    return -1;
}

static int stonePullFrame = -1000000;

static void pull_workers_to_stone()
{
    const int tpf = (TimePerFrame > 0) ? TimePerFrame : 40;
    if (info.GameFrame - stonePullFrame < STONE_SWEEP_MS / tpf) return;   // 节流（2 秒）
    if (!in_stone_window()) return;                    // 只在 10:00~13:00
    if (!stone_needed()) return;                       // 石够用 / 15:00 后：不抽人

    // 已在石矿上的人（内核真值，含“正走过去”的）
    std::unordered_map<int,char> stoneSN;
    for (tagResource &r : info.resources)
        if (r.Type == RESOURCE_STONE) stoneSN[r.SN] = 1;
    if (stoneSN.empty()) return;

    int onStone = 0;
    for (tagFarmer &f : info.farmers)
        if (f.FarmerSort == FARMERTYPE_FARMER && f.WorkObjectSN != -1
            && stoneSN.count(f.WorkObjectSN)) onStone++;
    if (onStone >= STONE_WINDOW_WORKERS) return;       // 已经够 3 个了

    stonePullFrame = info.GameFrame;

    // 三个来源，各抽 1 个（顺序：打猎 → 伐木 → 采金）
    const int srcTypes[STONE_WINDOW_WORKERS] = { RESOURCE_GAZELLE, RESOURCE_TREE,
                                                 RESOURCE_GOLD };
    for (int k = 0; k < STONE_WINDOW_WORKERS && onStone < STONE_WINDOW_WORKERS; ++k) {
        for (tagFarmer &f : info.farmers) {
            if (f.FarmerSort != FARMERTYPE_FARMER) continue;
            if (f.WorkObjectSN == -1) continue;                 // 没在手干活
            if (on_build_task(f.SN)) continue;                  // 工地上的不动
            if (resource_type_by_sn(f.WorkObjectSN) != srcTypes[k]) continue;

            // 离他最近的可用石矿
            int resSN = -1;
            double bestD = 1e18;
            for (tagResource &r : info.resources) {
                if (r.Type != RESOURCE_STONE || r.Cnt <= 0) continue;
                if (res_too_far(r.Type, r.BlockDR, r.BlockUR)) continue;
                if (gather_spot_dangerous(r.DR, r.UR,
                                          gather_danger_radius(r.Type))) continue;
                if (res_stand_spots(r.SN) <= 0) continue;       // 够不到：不派
                const double d = calDistance(f.DR, f.UR, r.DR, r.UR);
                if (d < bestD) { bestD = d; resSN = r.SN; }
            }
            if (resSN == -1) break;        // 没矿可去：这一档放弃（下一轮再试）

            HumanAction(f.SN, resSN);
            mark_farmer_order(f.SN);

            // ① 归还他手上那条旧采集任务（否则它一直挂 ASSIGNED、占着名额）
            for (Task &t : taskQueue) {
                if (t.type != TASK_GATHER || t.state != TASK_ASSIGNED) continue;
                if (t.farmerSN != f.SN) continue;
                t.state = TASK_DONE;
                break;
            }
            // ② 有现成的采石任务就登记成“他 + 这座矿”
            for (Task &t : taskQueue) {
                if (t.type != TASK_GATHER || t.resourceType != RESOURCE_STONE) continue;
                if (t.state != TASK_WAITING) continue;
                t.state = TASK_ASSIGNED;
                t.farmerSN = f.SN;
                t.targetSN = resSN;
                t.startFrame = info.GameFrame;
                break;
            }
            ++onStone;
            break;
        }
    }
}

void recycle_tasks()
{
    if (stone_forbidden_now()) recall_stone_miners();
    if (phase >= 3) recall_food_gatherers_to_farm();

    int woodKept = 0;
    const int woodQuota = wood_gather_limit();

    for (Task &t : taskQueue) {
        if (t.state == TASK_DONE || t.state == TASK_FAILED) continue;

        if (t.type == TASK_GATHER) {
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
                        if (t.state == TASK_ASSIGNED) {
                            if (t.progressFrame < 0 || f.Resource != t.gatherProgressResource
                                || calDistance(f.DR, f.UR, t.progressDR, t.progressUR) >= 0.25 * BLOCKSIDELENGTH) {
                                t.progressFrame = info.GameFrame; t.progressDR = f.DR; t.progressUR = f.UR;
                                t.gatherProgressResource = f.Resource;
                            } else if (info.GameFrame - t.progressFrame >= 8000 / std::max(1, TimePerFrame)
                                && !farmer_just_ordered(f.SN) && !on_build_task(f.SN)) {
                                const long long key = ((long long)f.SN << 32) | (unsigned int)t.targetSN;
                                gatherBlockedUntil[key] = info.GameFrame + 60000 / std::max(1, TimePerFrame);
                                const auto holder = farmHolder.find(t.targetSN);
                                if (holder != farmHolder.end() && holder->second == f.SN) farmHolder.erase(holder);
                                HumanMove(f.SN, f.DR, f.UR); mark_farmer_order(f.SN);
                                DebugText(std::string("采集卡住: 村民=") + std::to_string(f.SN)
                                    + " 目标=" + std::to_string(t.targetSN) + " 8s无位移或采集进展，释放任务");
                                t.state = TASK_DONE;
                            }
                        }
                        // 判“他闲下来了”必须排除“我刚派过他、内核还没反应”的窗口，
                        // 否则刚派出去的任务会被当成已完成收掉（“来回折腾”的来源之一）
                        farmerIdle = (f.NowState == HUMAN_STATE_IDLE)
                                     && !farmer_just_ordered(f.SN);
                        break;
                    }
            }
            if (t.state == TASK_DONE) continue;
            if (targetGone || farmerGone) {
                t.state = TASK_DONE;          // 目标没了 / 人阵亡 → 任务作废
            }
            else if (farmerIdle) {
                t.state = TASK_WAITING;
                t.farmerSN = -1;
                t.targetSN = -1;
                t.progressFrame = -1;
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
                t.state = TASK_DONE;
                send_gatherer_to_wood(t.farmerSN);
                continue;
            }
            else if (t.resourceType == RESOURCE_GOLD && !gold_needed()) {
                t.state = TASK_DONE;
                send_gatherer_to_wood(t.farmerSN);
                continue;
            }
        }
        else if (t.type == TASK_BUILD) {
            int builtSN = -1;   // 建成后的建筑 SN（农田要用）

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

                int percent = -1;
                for (const tagBuilding &b : info.buildings)
                    if (b.SN == siteSN) { percent = b.Percent; break; }
                if (f != nullptr) {
                    if (t.progressFrame < 0 || percent != t.progressPercent
                        || calDistance(f->DR, f->UR, t.progressDR, t.progressUR) > 0.5 * BLOCKSIDELENGTH) {
                        t.progressFrame = info.GameFrame; t.progressPercent = percent;
                        t.progressDR = f->DR; t.progressUR = f->UR;
                    } else if (info.GameFrame - t.progressFrame >= 8000 / TimePerFrame) {
                        const int oldWorker = t.farmerSN;
                        t.badBuilders[oldWorker] = info.GameFrame + 60000 / TimePerFrame;
                        t.state = TASK_WAITING; t.farmerSN = -1; t.targetSN = -1;
                        t.progressFrame = -1; t.startFrame = info.GameFrame;
                        t.resendFrame = 0; t.resendCount = 0;
                        send_gatherer_to_wood(oldWorker);
                        DebugText(std::string("建造卡住换人: 任务=") + std::to_string(t.id)
                            + " 原工人=" + std::to_string(oldWorker) + " 地基=" + std::to_string(siteSN));
                        continue;
                    }
                }

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
                                t.badBuilders[t.farmerSN] = info.GameFrame + 60000 / TimePerFrame;
                                t.state = TASK_WAITING; t.farmerSN = -1; t.targetSN = -1;
                                t.progressFrame = -1; t.startFrame = info.GameFrame;
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

            if (t.blockDR != -1
                && (t.state == TASK_FAILED
                    || (t.state == TASK_DONE && builtSN != -1))) {
                int size = building_size(t.buildingType);
                for (int i = t.blockDR; i < t.blockDR + size; i++)
                    for (int j = t.blockUR; j < t.blockUR + size; j++)
                        MAP[i][j] = 0;   // 释放占位
            }

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

    for (const tagBuilding &b : info.buildings) {
        if (b.Percent >= 100) continue;
        bool tracked = false;
        for (const Task &t : taskQueue)
            if (t.type == TASK_BUILD && t.buildingType == b.Type
                && t.blockDR == b.BlockDR && t.blockUR == b.BlockUR) { tracked = true; break; }
        if (tracked) continue;
        Task t;
        t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 1;
        t.buildingType = b.Type; t.blockDR = b.BlockDR; t.blockUR = b.BlockUR;
        t.startFrame = info.GameFrame;
        t.granaryFarm = granaryFarmSites.count((b.BlockDR << 12) | b.BlockUR) != 0;
        taskQueue.push_back(t);
        DebugText(std::string("恢复未完工建筑: SN=") + std::to_string(b.SN));
    }
    pull_workers_to_stone();
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
            const bool focusChanged = towerFocusSN != lastTowerFocusSN;
            if (towerFocusSN != lastTowerFocusSN)
                towerAggroFrame = info.GameFrame;   // 换了新集火目标：重新计拉仇恨时间
            lastTowerFocusSN = towerFocusSN;
            towerOrderFrame = info.GameFrame;
            for (tagBuilding &tower : info.buildings) {
                if (tower.Type != BUILDING_ARROWTOWER) continue;
                if (tower.Percent < 100) continue;
                towerTargetSN[tower.SN] = towerFocusSN;
                const int diagId = HumanAction(tower.SN, towerFocusSN);
                defenseDiagOrders.push_back({diagId, tower.SN, towerFocusSN, info.GameFrame, false});
                DebugText(std::string("守家诊断下令: 塔=") + std::to_string(tower.SN)
                    + " 目标=" + std::to_string(towerFocusSN) + " id=" + std::to_string(diagId)
                    + " 快照Project=" + std::to_string(tower.Project)
                    + " 原因=" + (focusChanged ? "换目标" : "2s刷新"));
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

    if (info.GameFrame - defenseDiagFrame >= 5000 / TimePerFrame) {
        defenseDiagFrame = info.GameFrame;
        DebugText(std::string("守家诊断快照: 帧=") + std::to_string(info.GameFrame)
            + " 阶段=" + std::to_string(phase) + " 时代=" + std::to_string(info.civilizationStage)
            + " 石库存=" + std::to_string((int)info.Stone)
            + " 塔科技=" + std::to_string(arrowTowerResearched)
            + " 塔目标数=" + std::to_string(arrowTowerTarget)
            + " 待建塔=" + std::to_string(active_build(BUILDING_ARROWTOWER))
            + " 集火=" + std::to_string(towerFocusSN)
            + " 有塔=" + std::to_string(hasTower) + " 仇恨就绪=" + std::to_string(aggroReady)
            + " 祭司到位=" + std::to_string(priestEngaged));
        if (priest != nullptr)
            DebugText(std::string("守家诊断祭司: SN=") + std::to_string(priest->SN)
                + " 血=" + std::to_string(priest->Blood) + " 状态=" + std::to_string(priest->NowState)
                + " 冷却=" + std::to_string(priest->ConvertCooldown)
                + " Work=" + std::to_string(priest->WorkObjectSN)
                + " 转化记录=" + std::to_string(convertTargetSN)
                + " 位置=" + std::to_string(priest->BlockDR) + "," + std::to_string(priest->BlockUR));
        for (const tagBuilding &tower : info.buildings) {
            if (tower.Type != BUILDING_ARROWTOWER) continue;
            DebugText(std::string("守家诊断塔: SN=") + std::to_string(tower.SN)
                + " 血=" + std::to_string(tower.Blood) + " 完成=" + std::to_string(tower.Percent)
                + " Project=" + std::to_string(tower.Project)
                + " 位置=" + std::to_string(tower.BlockDR) + "," + std::to_string(tower.BlockUR));
        }
        std::unordered_map<int,int> nextBlood;
        for (const tagArmy &e : info.enemy_armies) {
            double towerDist = -1;
            for (const tagBuilding &tower : info.buildings) {
                if (tower.Type != BUILDING_ARROWTOWER || tower.Percent < 100) continue;
                const double d = calDistance(e.DR, e.UR, tower.BlockDR * BLOCKSIDELENGTH,
                                              tower.BlockUR * BLOCKSIDELENGTH) / BLOCKSIDELENGTH;
                if (towerDist < 0 || d < towerDist) towerDist = d;
            }
            const auto old = defenseDiagBlood.find(e.SN);
            DebugText(std::string("守家诊断敌兵: SN=") + std::to_string(e.SN)
                + " 兵种=" + std::to_string(e.Sort) + " 血=" + std::to_string(e.Blood)
                + " 掉血=" + (old == defenseDiagBlood.end() ? std::string("首次") : std::to_string(old->second - e.Blood))
                + " 状态=" + std::to_string(e.NowState) + " Work=" + std::to_string(e.WorkObjectSN)
                + " 最近塔距=" + std::to_string(towerDist)
                + " 祭司距=" + std::to_string(priest == nullptr ? -1.0 :
                    calDistance(e.DR, e.UR, priest->DR, priest->UR) / BLOCKSIDELENGTH));
            nextBlood[e.SN] = e.Blood;
        }
        defenseDiagBlood.swap(nextBlood);
    }

    if (priest != nullptr && priest->ConvertCooldown == 0 && priestEngaged && aggroReady) {
        // 目标失效（转化成功/死亡/离开视野）→ 重新选
        if (convertTargetSN != -1) {
            bool stillEnemy = false;
            for (int sn : enemies)
                if (sn == convertTargetSN) { stillEnemy = true; break; }
            if (!stillEnemy) convertTargetSN = -1;
        }

        int target = -1;
        double best = 1e18;
        const double bsl = BLOCKSIDELENGTH;      // 1 格 = 多少细节坐标
        const double nearMax = PRIEST_CONVERT_RADIUS * bsl;
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
        double homeDR = 0, homeUR = 0;
        const bool haveHome = home_center(homeDR, homeUR);
        double siegeBest = 1e18;
        for (const tagArmy &e : info.enemy_armies) {
            if (e.Sort != AT_STONE_THROWER || !haveHome
                || calDistance(e.DR, e.UR, homeDR, homeUR) > HOME_DEFEND_RADIUS * bsl) continue;
            const double d = calDistance(priest->DR, priest->UR, e.DR, e.UR);
            if (e.SN == convertTargetSN && priest->WorkObjectSN == e.SN
                && priest->NowState == HUMAN_STATE_ATTACKING) {
                target = e.SN; break;
            }
            if (d < siegeBest) { siegeBest = d; target = e.SN; }
        }
        if (target < 0)
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
            const int diagId = HumanAction(priest->SN, target);
            defenseDiagOrders.push_back({diagId, priest->SN, target, info.GameFrame, true});
            DebugText(std::string("守家诊断下令: 祭司=") + std::to_string(priest->SN)
                + " 目标=" + std::to_string(target) + " id=" + std::to_string(diagId));
        }
    }

    // ---- 3) 我方士兵主动迎战 ----
    double hDR = 0, hUR = 0;
    const bool haveHome = home_center(hDR, hUR);
    int defenseGap = DEFENSE_ORDER_MS / TimePerFrame;
    if (defenseGap < 1) defenseGap = 1;
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) continue;
        if (a.Sort == AT_SCOUT) continue;
        bool reservedTarget = false;
        for (const tagArmy &e : info.enemy_armies)
            if (e.SN == a.WorkObjectSN && reserve_wave3_siege(e)) { reservedTarget = true; break; }
        if (a.NowState == HUMAN_STATE_ATTACKING && !reservedTarget) continue;

        // ① 离家太远的兵不归这里管（它们是反攻部队）
        if (!haveHome
            || calDistance(a.DR, a.UR, hDR, hUR)
               > HOME_DEFEND_RADIUS * BLOCKSIDELENGTH) continue;
        // ② 节流
        if (!reservedTarget && info.GameFrame - defenseOrderFrame[a.SN] < defenseGap) continue;

        int target = -1;
        double best = 1e18;
        for (tagArmy &e : info.enemy_armies) {
            if (reserve_wave3_siege(e)) continue;
            double d = calDistance(a.DR, a.UR, e.DR, e.UR);
            if (d < best) { best = d; target = e.SN; }
        }
        if (target != -1) {
            HumanAction(a.SN, target);
            defenseOrderFrame[a.SN] = info.GameFrame;
        } else if (reservedTarget) {
            kite_retreat_home(a);
            // 即使短退找不到落点，也取消旧的攻击关系，避免继续击杀待转化投石车。
            if (unitStepFrame[a.SN] != info.GameFrame)
                HumanMove(a.SN, a.DR, a.UR);
            attackOrderSN.erase(a.SN);
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
        leaf("standby",  nullptr, [](BTContext &) { army_standby();   return true; }),
        leaf("produce",  nullptr, [](BTContext &) { demand_produce(); return true; }),
        leaf("army",     nullptr, [](BTContext &) { demand_army();    return true; }),
        leaf("research", nullptr, [](BTContext &) { demand_research();return true; }),
        leaf("scout",    nullptr, [](BTContext &) { demand_scout();   return true; }),
        leaf("attack",   nullptr, [](BTContext &) { demand_attack();  return true; }),
        leaf("repair",   nullptr, [](BTContext &) { demand_repair();  return true; }),
        leaf("dispatch", nullptr, [](BTContext &) { bt_dispatch();    return true; }),
        leaf("gather",   nullptr, [](BTContext &) { demand_gather();  return true; })
    });
    btRoot->btName = "root";
}

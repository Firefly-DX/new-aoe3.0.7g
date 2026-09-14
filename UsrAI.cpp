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

using namespace std;

tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/
tagInfo info;

// 建筑占地尺寸（块）：房屋/箭塔 2x2，其余 3x3
static int building_size(int type) {
    return (type == BUILDING_HOME || type == BUILDING_ARROWTOWER) ? 2 : 3;
}

// 该建造位置是否堵住了"市中心的村民通道"。
// 背景：引擎里**所有建筑都是障碍物**（Map::loadBarrierMap 里没有任何例外，
// 农田也一样），而农田和市中心都是 3x3——只要市中心东南西北各落一块农田，
// 四边邻格就被填满，市中心被彻底围死，村民进不去也交不了货。
// 所以固定预留一条通道：市中心正南方向、宽 HOME_CORRIDOR_WIDTH 格的竖向地带，
// 从市中心南邻行一直向南延伸。任何以市中心为锚点的建筑都不能压住它。
static const int HOME_CORRIDOR_WIDTH = 2;   // 通道宽度（格）

static bool blocks_home_corridor(int cx, int cy, int bx, int by, int size)
{
    // 市中心占 (cx..cx+2, cy..cy+2)，通道取西侧对齐的 cx..cx+WIDTH-1 列，y>=cy+3
    if (bx + size - 1 < cx)                          return false;   // 完全在通道左侧（以西）
    if (bx > cx + HOME_CORRIDOR_WIDTH - 1)           return false;   // 完全在通道右侧（以东）
    if (by + size - 1 < cy + 3)                      return false;   // 完全在市中心本体 / 通道以北
    return true;                                                      // 与通道相交
}

// 敌方第三波发动帧（约 14 分钟，默认 25fps → 21000 帧），之后转入反攻
static const int ATTACK_START_FRAME = 21000;

// 采集任务的哨兵资源类型：表示"采集农田"（农田是建筑而非 tagResource，
// 且 BUILDING_FARM 与 RESOURCE_GAZELLE 数值都是 4，故用独立哨兵值区分）
static const int GATHER_FARM = 100;

// 探图步长（块）：目标选"距离祭司最接近这个值"的前沿格。
// 默认取祭司视野半径（12），这样每前进一步刚好揭开一圈新区域，既不会重叠浪费、
// 也不会跳过大片未知区域。
//  调大 → 每步跨得更远（偏深度，可能跳过一些兜区）；
//  调小 → 贴着前沿细扫（偏广度，步数更多）。
static const int SCOUT_STRIDE = 12;

// 防守触发半径（块）：只有可见敌军进入我方市镇中心这个半径内，才启用
// 「箭塔拉仇恨 + 祭司转化」的防守战术。这样祭司探图途中扫到远处零散兵群
// 或远处的来袭波次时，不会被误触发去防守，探图与防守不再互相打扰。
static const int HOME_DEFEND_RADIUS = 35;

// 探图时遇敌的撤离参数：
//   SCOUT_THREAT_RADIUS —— 敌人进入祭司这个半径内就撤离；
//   SCOUT_FLEE_STRIDE   —— 每次撤离目标取背离方向这么多格。
static const int SCOUT_THREAT_RADIUS = 12;
static const int SCOUT_FLEE_STRIDE = 15;

// 选目标时避开敌点的半径，以及敌点记录的"新鲜度"：
// 只按最近 SPOT_AVOID_FRESH 秒内看到过的敌人来避让（陈年记录不再影响选点），
// 否则一张地图上四处都留下过敌点，会到处都"不能去"。
static const int SPOT_AVOID_RADIUS = 15;
static const int SPOT_AVOID_FRESH_FRAMES = 750;   // ≈30 秒（默认 25fps）

// 选点打分的两个修正项：
//   SCOUT_BACK_PENALTY —— 目标落在当前行进方向后方时的惩罚（越大越少走回头路）；
//   SCOUT_EDGE_MARGIN / SCOUT_EDGE_PENALTY —— 离地图边界越近权重越低，
//   避免祭司老往地图角落跑。单位："等效格数"（会乘 BLOCKSIDELENGTH）。
static const double SCOUT_BACK_PENALTY = 1.2;
static const int SCOUT_EDGE_MARGIN = 10;
static const double SCOUT_EDGE_PENALTY = 1.5;

// 新探路（以营地为圆心的环形广度优先）参数：
//   从营地外 SCOUT_RING_START 格开始，每圈按弧长均匀布点
//   （间距 ≈SCOUT_ARC_SPACING），一圈扫完半径 +SCOUT_RING_STEP。
//   STEP 调小 → 扫得更细（更慢）；调大 → 更粗更快。
static const int SCOUT_RING_START = 10;
static const int SCOUT_RING_STEP = 8;

// 每圈路点的目标间距（弧长，格）。点上个数按弧长算：n = 2πr / 间距。
// 必须按弧长算——营地几乎总在地图角落，"以营地为圆心的整圆"有大半在图外；
// 若每圈固定取 16 个点，图内那段弧上只剩 3~4 个点、间距二十多格，
// 看起来就成了"沿直线往外扩、只扫 1/4 圈"。
static const int SCOUT_ARC_SPACING = 8;

// 让箭塔先拉仇恨、再让祭司转化：塔开火后等这么久（毫秒）祭司才动手。
static const int CONVERT_AGGRO_DELAY = 1500;

// 祭司"在位"判定半径（块）：以防御锚点（箭塔，无塔则市中心）为圆心。
// 不能用市中心判定——祭司是被 recall_priest_home 派到**箭塔**下待命的，
// 箭塔建得离市中心远时，用市中心算距离会把已到位的祭司误判成"没到位"，
// 结果它站在塔下一动不动、永远不转化。
// 必须与 SCOUT_HOME_CALL_RADIUS 保持同一个值（两边职责相反，半径要一致）。
static const int PRIEST_ENGAGE_RADIUS = 30;

// 祭司优先转化的距离（块）：这个范围内的敌人能立刻上手，
// 超出后要给大额惩罚，免得祭司丢下脚边的敌人去追远处的（路上还会被反杀）。
static const int PRIEST_CONVERT_RADIUS = 12;

// 祭司"空余时间治疗"：这个时间点之前（分钟），只要家里没敌袭，
// 祭司回村待命时就顺便给伤兵回血（内核里祭司对友军执行 HumanAction 就是治疗）。
// 之后的战事密集，祭司交给 combat_tactic / demand_attack，不再单独治疗。
static const int PRIEST_HEAL_UNTIL_MIN = 12;   // 分钟

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

// 采集/捕猎点到"最近的可用存放建筑"超过这个距离（块），就就近补建谷仓/仓库，
// 减少村民来回跑路的时间。超过 10 格就补——来回一趟的时间差不多能再采一组了。
static const int DROP_DIST_MAX = 10;

// 冲铜器阶段需要采石时，采石人数占村民总数的百分比。
// 开局自带 1 座箭塔、石头 150（够再建 1 座），所以前期通常算出来是 0 人。
static const int PRE_BRONZE_STONE_PCT = 10;

// ============ 开局经济：6 人采浆果 + 其余全砍树 + 先砍树后打猎 ============
// 食物固定人数：只采浆果，不派人打猎（浆果就在城边，打猎要跑远、来回搬肉）。
// 这里用"绝对人数"而不是百分比：开局只有 8 个村民，任何百分比算出来都是
// 2~3 人，永远攒不起冲铜器建筑链要的 545 木材。
static const int FOOD_GATHERERS = 6;

// 打猎（瞪羚）开闸条件：人口约 HUNT_START_POP、木头约 HUNT_START_WOOD 时开始。
// 开局一律不杀瞪羚，多余劳动力先全部投入伐木；开到闸之后锁存常开，
// 不随木头存量上下波动来回切（否则打猎人数会反复增减、村民来回跑）。
static const int HUNT_START_POP  = 12;
static const int HUNT_START_WOOD = 300;

// 开闸后打猎人数占村民总数的百分比（农田优先，剩下的名额才给打猎）。
static const int HUNT_PERCENT = 20;

// ============ 农田：后期食物主力 ============
// 内核里一块农田是"一次性资源建筑"（CNT_BUILD_FARM = 250 食物，成本 75 木），
// 而且只允许**一个**采集者（Building_Resource::isGathererAsLandlord 的地主判定），
// 采完会被内核自动删除（非 surplus 的资源建筑直接移除）。
// 所以规则是"一个村民对应一格农田"：想派 N 个人种田就得有 N 块农田，
// 由 demand_build 按人口持续补建。
static const int FARM_PER_POP = 5;   // 每多少个村民配 1 块农田
static const int FARM_MAX     = 6;   // 农田数量上限（地面和木头都要省着用）

// ============ 后期分工 ============
// 木材保底人数：后期食物为主，但房屋 / 补仓库 / 农田本身 / 科技都还要木头。
// 名额不够时按"打猎 → 农田"的顺序往回缩。
static const int WOOD_MIN_GATHERERS = 4;

// 以市中心为锚点的建筑（市场/兵营/靶场/马厩/学院/箭塔…）的搜索起始环半径（块）。
// 农田要环绕市中心（采完走一格就能上交），所以这些建筑往外扩，把内圈让出来。
static const int CENTER_BUILD_START_R = 6;

// 补建存放建筑的数量上限（含开局自带的 1 谷仓 + 1 仓库）。
// 每种资源最多补建一座；上限给够，否则树会把名额占满，
// 金矿/石矿离得再远也永远轮不到拍仓库。
static const int DROPOFF_GRANARY_MAX = 3;
static const int DROPOFF_STOCK_MAX   = 5;

// 打猎前置条件：瞪羚附近这个半径（块）内必须先有可用存放建筑
// （仓库/谷仓/市中心都算），否则村民大半时间都花在搬肉的路上。
// 不满足就先"在瞪羚旁边拍仓库"，拍好再派村民杀瞪羚（见 demand_dropoff）。
static const int HUNT_DROP_RADIUS = 10;

// "食物断供"的例外：浆果吃光、又没有农田时只能靠打猎续命，
// 此时不必等人口/木头门槛；但开局这段帧内不启用，保证"开局不杀瞪羚"。
static const int HUNT_STARVE_MIN_FRAME = 750;   // ≈30 秒（默认 25fps）

// 浆果只在前期采：只采城边 BUSH_NEAR_RADIUS 格内那几丛（实测 4 张图在市中心
// 22 格内都正好有 6 丛），采完就结束浆果阶段——不跑远去追别的浆果丛。
// 空出来的名额转给农田 / 打猎 / 伐木（具体由下面的配额逻辑决定）。
static const int BUSH_NEAR_RADIUS = 22;

// 箭塔数量目标：前期 1 座就够挡第一波，多建是浪费石头（150 石/座）；
// 进铜器后再补到 2 座。
static const int TOWER_TARGET_EARLY  = 1;
static const int TOWER_TARGET_BRONZE = 2;

// 采集点分散：同一个资源点最多同时挂 GATHER_PER_RESOURCE_MAX 个村民。
// 树 / 矿石都只占一格，周围站不下太多人（碰撞会把后到的人挤开），
// 全挤在离卸货点最近的那棵树上，结果就是谁也采不踏实。
// 资源点够多时按上限分散；一个符合条件的都没有时（资源太少）
// 会自动放开限制选最近的，不让村民干等。
static const int GATHER_PER_RESOURCE_MAX = 3;

void UsrAI::processData()
{
    info = getInfo();

    // 首次进入时构建行为树
    if (!btRoot) build_behavior_tree();

    btCtx.info = &info;
    btCtx.ai = this;
    btCtx.traceOn = btTraceEnabled;
    btCtx.trace.clear();

    btRoot->tick(btCtx);

    // 调试：输出本帧 tick 路径
    if (btTraceEnabled) {
        std::string s = "BT:";
        for (const char *n : btCtx.trace) { s += " "; s += n; }
        DebugText(s);
    }
}

// 建筑建造模块

bool UsrAI::find_block(int x,int y,int dx,int dy){
    if (info.theMap == nullptr) return 0;
    int w = (int)info.theMap->size();
    if (w == 0) return 0;
    int ht = (int)(*info.theMap)[0].size();
    if (x < 0 || y < 0 || x + dx > w || y + dy > ht) return 0;
    int h[dx * dy] = {0};
    for (int i = 0;i < dx;i ++){
        for (int j = 0;j < dy;j ++){
            tagTerrain field = (*info.theMap)[x + i][y + j];
            if (field.type != MAPPATTERN_GRASS){
                return 0;
            }
            if (MAP[x + i][y + j] != 0){
                return 0;
            }
            h[dx * i + j] = field.height; 
        }
    }
    for (int i = 1;i < dx * dy;i ++){
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
void UsrAI::bt_sync()
{
    if (info.civilizationStage < CIVILIZATION_BRONZEAGE) {
        phase = 1;   // 开局即工具时代，直接冲铜器
    } else if (info.GameFrame >= ATTACK_START_FRAME) {
        phase = 3;   // 第三波之后：组织反攻
    } else {
        phase = 2;   // 铜器时代：发展军事、防守三波
    }

    // 箭塔数量目标：前期 1 座即可，进铜器后再补
    arrowTowerTarget = (phase >= 2) ? TOWER_TARGET_BRONZE : TOWER_TARGET_EARLY;

    // ---- 目标农田数 = 想派去种田的村民数（一个村民对应一格农田）----
    // 内核一块农田只认一个采集者，而且采完会自动消失，所以要按人口持续补建。
    // 前置：市场（Development 里农田的 buildCon 挂了市场的 precondition）。
    farmTarget = 0;
    if (count_done(BUILDING_MARKET) > 0) {
        int farmerNum = 0;
        for (tagFarmer &f : info.farmers)
            if (f.FarmerSort == FARMERTYPE_FARMER) farmerNum++;
        farmTarget = farmerNum / FARM_PER_POP;
        if (farmTarget > FARM_MAX) farmTarget = FARM_MAX;
    }

    recycle_tasks();
}

// ---------- 统计辅助 ----------
int UsrAI::count_done(int type)
{
    int c = 0;
    for (tagBuilding &b : info.buildings)
        if (b.Type == type && b.Percent >= 100) c++;
    return c;
}

int UsrAI::active_build(int btype)
{
    int c = 0;
    for (Task &t : taskQueue)
        if (t.type == TASK_BUILD && t.buildingType == btype
            && t.state != TASK_DONE && t.state != TASK_FAILED) c++;
    return c;
}

int UsrAI::active_gather(int rtype)
{
    int c = 0;
    for (Task &t : taskQueue)
        if (t.type == TASK_GATHER && t.resourceType == rtype
            && t.state != TASK_DONE && t.state != TASK_FAILED) c++;
    return c;
}

int UsrAI::active_action(int btype, int action)
{
    int c = 0;
    for (Task &t : taskQueue)
        if ((t.type == TASK_PRODUCE || t.type == TASK_UPGRADE)
            && t.buildingType == btype && t.targetSN == action
            && t.state != TASK_DONE && t.state != TASK_FAILED) c++;
    return c;
}

// 该资源点上已经派了几个采集村民（还没做完的任务）
int UsrAI::gatherers_on(int resSN)
{
    int c = 0;
    for (Task &t : taskQueue)
        if (t.type == TASK_GATHER && t.targetSN == resSN
            && t.state != TASK_DONE && t.state != TASK_FAILED) c++;
    return c;
}

bool UsrAI::has_resource(int rtype)
{
    // 普通资源看 Cnt；活动物 Cnt=0 但 Blood>0，也算"有资源"（可打猎）
    bool isAnimal = (rtype == RESOURCE_GAZELLE || rtype == RESOURCE_ELEPHANT
                     || rtype == RESOURCE_LION);
    for (tagResource &r : info.resources) {
        if (r.Type != rtype) continue;
        if (r.Cnt > 0) return true;
        if (isAnimal && r.Blood > 0) return true;
    }
    return false;
}

bool UsrAI::center_free()
{
    for (tagBuilding &b : info.buildings)
        if (b.Type == BUILDING_CENTER && b.Percent >= 100 && b.Project == 0)
            return true;
    return false;
}

// ---------- 建造需求 ----------
void UsrAI::demand_build()
{
    // ---- 房屋：按"目标人口"提前补，别让人口上限卡住村民生产与造兵 ----
    {
        int targetPop = 20 + armyTarget + 4;   // 村民上限 + 军队目标 + 余量
        int homeNeed = (targetPop - info.Human_MaxNum + HOUSE_HUMAN_NUM - 1)
                       / HOUSE_HUMAN_NUM;      // 还差几座房
        if (homeNeed > 0
            && info.Wood >= BUILD_HOUSE_WOOD
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
            if (count_done(n.type) == 0 && active_build(n.type) == 0
                && info.Wood >= n.wood) {
                Task t;
                t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 2;
                t.buildingType = n.type;
                taskQueue.push_back(t);
                break;
            }
        }
    } else {
        // 铜器时代：补齐马厩（骑兵前置）与学院（方阵兵）
        if (count_done(BUILDING_STABLE) == 0 && active_build(BUILDING_STABLE) == 0
            && info.Wood >= BUILD_STABLE_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 2;
            t.buildingType = BUILDING_STABLE;
            taskQueue.push_back(t);
        } else if (count_done(BUILDING_COLLAGE) == 0 && active_build(BUILDING_COLLAGE) == 0
            && info.Wood >= BUILD_COLLAGE_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 2;
            t.buildingType = BUILDING_COLLAGE;
            taskQueue.push_back(t);
        }
    }

    // ---- 农田：环绕市中心建（采完走一格就能上交），数量 = 目标农田数 ----
    // "一个村民对应一格农田"：内核一块农田只允许一个采集者，而且采完会被
    // 自动删除，所以这里要持续补建，直到达到 farmTarget（见 bt_sync）。
    // 优先级必须低于采集（3/4）—— demand_gather 会创建恰好等于村民总数的采集任务，
    // 若农田排在采集之后，轮到时已经没有空闲村民，任务会永远卡在 WAITING。
    // 用 2 与建筑链同级；同级按 id 排序，而建筑链在本函数更前面创建，仍会先建。
    if (farmTarget > 0
        && count_done(BUILDING_FARM) + active_build(BUILDING_FARM) < farmTarget
        && info.Wood >= BUILD_FARM_WOOD + 50) {
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
            if (!arrowTowerResearched && arrowTowerResearchId == -1
                && info.Meat >= BUILDING_GRANARY_ARROWTOWER_FOOD) {
                for (tagBuilding &b : info.buildings) {
                    if (b.Type == BUILDING_GRANARY && b.Percent >= 100 && b.Project == 0) {
                        arrowTowerResearchId = BuildingAction(b.SN, BUILDING_GRANARY_ARROWTOWER);
                        break;
                    }
                }
            }
        }

        // 建箭塔
        int towerNum = 0;
        for (tagBuilding &b : info.buildings)
            if (b.Type == BUILDING_ARROWTOWER) towerNum++;
        if (arrowTowerResearched && towerNum + active_build(BUILDING_ARROWTOWER) < arrowTowerTarget
            && info.Stone >= BUILD_ARROWTOWER_STONE) {
            Task t;
            t.id = nextTaskId++;
            t.type = TASK_BUILD;
            t.priority = 1;
            t.buildingType = BUILDING_ARROWTOWER;
            taskQueue.push_back(t);
        }
    }

    // ---- 资源点太远 → 就近补建谷仓/仓库 ----
    demand_dropoff();
}

// ---------- 就近补建存放建筑（谷仓 / 仓库）----------
// 村民采满一组后会去最新的存放建筑上交，太远会大量浪费时间在路上。
// 存放规则：浆果食物 → 谷仓；木/石/金/狩猎食物 → 仓库；市中心什么都能存。
// 若某资源点到"最近的可用存放建筑"超过 DROP_DIST_MAX 格，就在该资源点旁补建。
void UsrAI::demand_dropoff()
{
    int farmerNum = 0;
    for (tagFarmer &f : info.farmers)
        if (f.FarmerSort == FARMERTYPE_FARMER) farmerNum++;

    // ---- 打猎前置：先在瞪羚旁边拍仓库，拍好才派人杀瞪羚 ----
    // 只有"看得见的瞪羚全都离存放建筑太远"（hunt_dropoff_ready 为假）时才拍，
    // 并且挑"离存放建筑最近的那只瞪羚"当锚点：它一到位打猎就能开工，
    // 不必给每只远处的瞪羚都配一座仓库（120 木/座，会把经济拖垮）。
    // 人口快到 HUNT_START_POP 时就提前备料；木头也要先到 HUNT_START_WOOD，
    // 免得这座仓库把冲刺中的木材花掉。
    // 这条不能等进铜器：否则"没仓库 → 打猎不开闸"互相等死。
    if (!huntStarted && !hunt_dropoff_ready()
        && farmerNum >= HUNT_START_POP - 2
        && info.Wood >= HUNT_START_WOOD
        && count_done(BUILDING_STOCK) + active_build(BUILDING_STOCK) < 3) {
        bool queued = false;
        for (Task &t : taskQueue)
            if (t.type == TASK_BUILD && t.resourceType == RESOURCE_GAZELLE) {
                queued = true; break;
            }
        if (!queued) {
            int bestSN = -1;
            double best = 1e18;
            for (tagResource &r : info.resources) {
                if (r.Type != RESOURCE_GAZELLE) continue;
                if (r.Cnt <= 0 && r.Blood <= 0) continue;
                double d = nearest_dropoff_dist(RESOURCE_GAZELLE, r.DR, r.UR);
                if (d < best) { best = d; bestSN = r.SN; }
            }
            if (bestSN != -1) {
                Task t;
                t.id = nextTaskId++;
                t.type = TASK_BUILD;
                t.priority = 2;
                t.buildingType = BUILDING_STOCK;
                t.resourceType = RESOURCE_GAZELLE;   // 标记：这是"打猎用仓库"
                t.targetSN = bestSN;                 // 锚点：最近的瞪羚
                taskQueue.push_back(t);
            }
        }
    }

    // 其余"资源点太远就补建仓库"只在铜器之后做：
    // 冲铜器阶段 545 木材的建筑链优先，任何额外建造都会拖慢升级；
    // 开局自带 1 谷仓 + 1 仓库 + 市中心，采浆果、砍树完全够用。
    if (phase < 2) return;

    // 瞪羚（打猎）的仓库由上面的"打猎前置"专门处理，这里不再重复
    struct Need { int resType; int btype; };
    const Need needs[] = {
        { RESOURCE_BUSH,     BUILDING_GRANARY },
        { RESOURCE_TREE,     BUILDING_STOCK   },
        { RESOURCE_STONE,    BUILDING_STOCK   },
        { RESOURCE_GOLD,     BUILDING_STOCK   },
        { RESOURCE_ELEPHANT, BUILDING_STOCK   },
    };

    for (const Need &n : needs) {
        int wood = (n.btype == BUILDING_GRANARY) ? BUILD_GRANARY_WOOD : BUILD_STOCK_WOOD;
        if (info.Wood < wood) continue;

        // ① 同一种资源已经补建过（建成或在建）就不再重复。
        //    这就是"每种资源最多一座"的限制：建好后该资源点就在附近了，
        //    距离判定自然不会再触发。
        bool queued = false;
        for (Task &t : taskQueue)
            if (t.type == TASK_BUILD && t.resourceType == n.resType) {
                queued = true; break;
            }
        if (queued) continue;

        // ② 全局上限（谷仓 / 仓库各自算）。以前这里是"各最多 2 座"，
        //    而开局已经自带 1 谷仓 + 1 仓库，于是只剩下一个名额、
        //    被树抢走后，金矿石矿离得再远也永远轮不到拍仓库。
        int cnt = count_done(n.btype) + active_build(n.btype);
        if (n.btype == BUILDING_GRANARY) {
            if (cnt >= DROPOFF_GRANARY_MAX) continue;
        } else {
            if (cnt >= DROPOFF_STOCK_MAX) continue;
        }

        // 找该类型中"离存放建筑最远"的资源点，作为新建的锚点
        int bestSN = -1;
        double worst = DROP_DIST_MAX * BLOCKSIDELENGTH;
        for (tagResource &r : info.resources) {
            if (r.Type != n.resType) continue;
            if (r.Cnt <= 0 && r.Blood <= 0) continue;
            double d = nearest_dropoff_dist(n.resType, r.DR, r.UR);
            if (d > worst) { worst = d; bestSN = r.SN; }
        }
        if (bestSN == -1) continue;

        Task t;
        t.id = nextTaskId++;
        t.type = TASK_BUILD;
        t.priority = 2;
        t.buildingType = n.btype;
        t.resourceType = n.resType;   // 标记：这是"选址在资源旁的存放建筑"
        t.targetSN = bestSN;          // 锚点资源 SN
        taskQueue.push_back(t);
    }
}

// 资源点 (dr,ur) 到"最近的可用存放建筑"的距离。
double UsrAI::nearest_dropoff_dist(int resType, double dr, double ur)
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
bool UsrAI::hunt_dropoff_ready()
{
    double best = 1e18;
    for (tagResource &r : info.resources) {
        if (r.Type != RESOURCE_GAZELLE) continue;
        if (r.Cnt <= 0 && r.Blood <= 0) continue;
        double d = nearest_dropoff_dist(RESOURCE_GAZELLE, r.DR, r.UR);
        if (d < best) best = d;
    }
    return best <= HUNT_DROP_RADIUS * BLOCKSIDELENGTH;
}

// ---------- 生产需求：村民 ----------
void UsrAI::demand_produce()
{
    int farmerNum = 0;
    for (tagFarmer &f : info.farmers)
        if (f.FarmerSort == FARMERTYPE_FARMER) farmerNum++;

    // ---- 造村民（全程持续，直到 20 人）----
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

// ---------- 采集需求 ----------
void UsrAI::demand_gather()
{
    int farmerNum = 0;
    for (tagFarmer &f : info.farmers)
        if (f.FarmerSort == FARMERTYPE_FARMER) farmerNum++;

    // ---- 采集需求（食物 / 木头 / 石头 / 黄金按需分配）----
    int total = farmerNum > 0 ? farmerNum : 1;

    // 冲铜器阶段（未进铜器）：不采金，采石只在箭塔缺料时才安排。
    bool preBronze = (phase < 2);

    // 箭塔未建够且石头不足时才安排采石
    int towerCnt = 0;
    for (tagBuilding &b : info.buildings)
        if (b.Type == BUILDING_ARROWTOWER) towerCnt++;
    bool needStone = (towerCnt < arrowTowerTarget) && (info.Stone < BUILD_ARROWTOWER_STONE);

    int wantStone = 0;
    if (needStone) {
        wantStone = preBronze ? (total * PRE_BRONZE_STONE_PCT / 100)
                              : (total / 6);
        if (wantStone < 1) wantStone = 1;
    }

    // 黄金：进铜器后（骑兵 / 方阵兵 / 科技要用）才安排，冲铜器阶段不采
    int wantGold = 0;
    if (!preBronze && has_resource(RESOURCE_GOLD)) {
        wantGold = total / 8;
        if (wantGold < 1) wantGold = 1;
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

    // ---- 打猎（瞪羚）开闸：人口/木头到位 + 瞪羚旁已拍好仓库 ----
    //   1) 正常路径：人口到 HUNT_START_POP 且木头到 HUNT_START_WOOD，
    //      并且满足"先拍仓库、再杀瞪羚"——hunt_dropoff_ready() 为真才开闸；
    //   2) 例外：浆果阶段结束又完全没有农田（farmTarget/farmCnt 都是 0，
    //      即还没来得及规划农田）= 食物断供，过了开局保护期直接开闸续命。
    // 开闸后锁存常开，不再随木头存量波动来回切。
    bool foodCut = (wantBush == 0 && berrySeen
                    && farmCnt == 0 && farmTarget == 0
                    && info.GameFrame >= HUNT_STARVE_MIN_FRAME);
    if (!huntStarted
        && (foodCut || (farmerNum >= HUNT_START_POP
                        && info.Wood >= HUNT_START_WOOD && hunt_dropoff_ready())))
        huntStarted = true;

    // 打猎是"额外增加"的采集位，不从 FOOD_GATHERERS 里挤（浆果那 6 人不动）。
    // 人数按总人口百分比给：后期人口一涨，打猎的人跟着涨。
    int wantHunt = 0;
    if (huntStarted && has_resource(RESOURCE_GAZELLE)) {
        wantHunt = total * HUNT_PERCENT / 100;
        if (wantHunt < 1) wantHunt = 1;
    }

    // ---- 农田：一个村民对应一格农田 ----
    // 想派 farmTarget 个人种田（数量在 bt_sync 里按人口算），
    // 但实际只能派到"已经建好的农田数"为止（内核一块农田只认一个采集者）。
    // 农田采完会被自动删除，demand_build 会补建，人数就跟着农田数一起长。
    int wantFarm = farmTarget;
    if (wantFarm > farmCnt) wantFarm = farmCnt;

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
    int wantWood = rest;

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
        if (f.FarmerSort == FARMERTYPE_FARMER && f.NowState == HUMAN_STATE_IDLE) idleNow++;

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
            if (!has_resource(fallbackTypes[k])) continue;
            for (int i = 0; i < spare; i++) {
                Task t;
                t.id = nextTaskId++; t.type = TASK_GATHER; t.priority = 5;
                t.resourceType = fallbackTypes[k];
                taskQueue.push_back(t);
            }
            spare = 0;
        }
    }

    // ---- 兜底 2：仍空闲的村民 → 一律去砍树 ----
    // 木头是冲铜器阶段的瓶颈（建筑链要 545 木），所以莫名其妙空下来的人全部去伐木；
    // 只有视野内没有树时才退而求其次，去最近的其它资源，保证不闲着。
    for (tagFarmer &f : info.farmers) {
        if (f.FarmerSort != FARMERTYPE_FARMER) continue;
        if (f.NowState != HUMAN_STATE_IDLE) continue;

        int treeSN = -1;
        double treeD = 1e18;
        int anySN = -1;
        double anyD = 1e18;
        for (tagResource &r : info.resources) {
            if (r.Cnt <= 0 && r.Blood <= 0) continue;
            if (r.Type != RESOURCE_TREE && r.Type != RESOURCE_STONE
                && r.Type != RESOURCE_GOLD && r.Type != RESOURCE_BUSH
                && r.Type != RESOURCE_GAZELLE && r.Type != RESOURCE_ELEPHANT)
                continue;
            if (r.Type == RESOURCE_GAZELLE && !huntStarted)
                continue;   // 开局不杀瞪羚
            if (r.Type == RESOURCE_BUSH && !berryPhase)
                continue;   // 浆果阶段结束：兜底也不再去采浆果
            double d = calDistance(f.DR, f.UR, r.DR, r.UR);
            if (d < anyD) { anyD = d; anySN = r.SN; }
            if (r.Type == RESOURCE_TREE && d < treeD) { treeD = d; treeSN = r.SN; }
        }

        int pick = (treeSN != -1) ? treeSN : anySN;
        if (pick != -1) HumanAction(f.SN, pick);
    }
}

// ---------- 第二阶段：造兵需求 ----------
// 按"学院方阵兵 > 马厩骑兵 > 靶场弓箭手 > 兵营棍棒兵"的优先级造兵，
// 每帧最多给一座空闲军事建筑下一条命令（建筑随后进入忙碌状态，自然不会重复下达）。
void UsrAI::demand_army()
{
    if (phase < 2) return;   // 铜器时代前优先经济，不造兵

    // 统计现有兵力（祭司不计入战斗兵）
    int clubman = 0, bowman = 0, cavalry = 0, hoplite = 0, totalArmy = 0;
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) continue;
        totalArmy++;
        if (a.Sort == AT_CLUBMAN || a.Sort == AT_SWORDSMAN) clubman++;
        else if (a.Sort == AT_BOWMAN || a.Sort == AT_COMPOSITE_BOWMAN
                 || a.Sort == AT_SLINGER) bowman++;
        else if (a.Sort == AT_CAVALRY || a.Sort == AT_CHARIOT) cavalry++;
        else if (a.Sort == AT_HOPLITE) hoplite++;
    }

    int target = (phase >= 3) ? armyTarget + 8 : armyTarget;
    if (totalArmy >= target) return;
    if (info.Human_Num + 1 > info.Human_MaxNum) return;   // 人口已满

    auto free_building = [&](int type) -> tagBuilding* {
        for (tagBuilding &b : info.buildings)
            if (b.Type == type && b.Percent >= 100 && b.Project == 0)
                return &b;
        return nullptr;
    };

    // 学院：方阵兵（铜器时代最强近战）
    tagBuilding *col = free_building(BUILDING_COLLAGE);
    if (col && info.civilizationStage >= CIVILIZATION_BRONZEAGE
        && hoplite < (target + 3) / 4
        && info.Meat >= BUILDING_COLLAGE_CREATE_HOPLITE_FOOD
        && info.Gold >= BUILDING_COLLAGE_CREATE_HOPLITE_GOLD) {
        BuildingAction(col->SN, BUILDING_COLLAGE_CREATE_HOPLITE);
        return;
    }

    // 马厩：骑兵（需食物 + 黄金）
    tagBuilding *st = free_building(BUILDING_STABLE);
    if (st && info.civilizationStage >= CIVILIZATION_BRONZEAGE
        && cavalry < (target + 2) / 3
        && info.Meat >= BUILDING_STABLE_CREATE_CAVALRY_FOOD
        && info.Gold >= BUILDING_STABLE_CREATE_CAVALRY_GOLD) {
        BuildingAction(st->SN, BUILDING_STABLE_CREATE_CAVALRY);
        return;
    }

    // 靶场：弓箭手（远程，前期主力）
    tagBuilding *rg = free_building(BUILDING_RANGE);
    if (rg && bowman < (target + 1) / 2
        && info.Meat >= BUILDING_RANGE_CREATE_BOWMAN_FOOD
        && info.Wood >= BUILDING_RANGE_CREATE_BOWMAN_WOOD) {
        BuildingAction(rg->SN, BUILDING_RANGE_CREATE_BOWMAN);
        return;
    }

    // 兵营：棍棒兵（工具时代即可生产，作为补充兵源）
    tagBuilding *cp = free_building(BUILDING_ARMYCAMP);
    if (cp && clubman < target / 2
        && info.Meat >= BUILDING_ARMYCAMP_CREATE_CLUBMAN_FOOD) {
        BuildingAction(cp->SN, BUILDING_ARMYCAMP_CREATE_CLUBMAN);
        return;
    }
}

// ---------- 第二阶段：科技研发 ----------
// 研发清单（两级科技的同一 Action 调用两次即可，用 level 追踪进度）
void UsrAI::init_researches()
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
    add("驯养动物", BUILDING_MARKET, BUILDING_MARKET_FARM_UPGRADE, 1,
        BUILDING_MARKET_FARM_UPGRADE_FOOD, BUILDING_MARKET_FARM_UPGRADE_WOOD, 0, 0,
        0, 0, 0, 0);
    add("车轮",     BUILDING_MARKET, BUILDING_MARKET_WHEEL_UPGRADE, 1,
        BUILDING_MARKET_WHEEL_UPGRADE_FOOD, BUILDING_MARKET_WHEEL_UPGRADE_WOOD, 0, 0,
        0, 0, 0, 0);
    add("石矿开采", BUILDING_MARKET, BUILDING_MARKET_STONE_UPGRADE, 1,
        BUILDING_MARKET_STONE_UPGRADE_FOOD, 0, BUILDING_MARKET_STONE_UPGRADE_STONE, 0,
        0, 0, 0, 0);
    add("金矿开采", BUILDING_MARKET, BUILDING_MARKET_GOLD_UPGRADE, 1,
        BUILDING_MARKET_GOLD_UPGRADE_FOOD, BUILDING_MARKET_GOLD_UPGRADE_WOOD, 0, 0,
        0, 0, 0, 0);

    // 军事类（兵营 / 靶场）
    add("战斧升级",  BUILDING_ARMYCAMP, BUILDING_ARMYCAMP_UPGRADE_CLUBMAN, 1,
        BUILDING_ARMYCAMP_UPGRADE_CLUBMAN_FOOD, 0, 0, 0, 0, 0, 0, 0);
    add("阔剑科技",  BUILDING_ARMYCAMP, BUILDING_ARMYCAMP_UPGRADE_BROADSWORD, 1,
        BUILDING_ARMYCAMP_UPGRADE_BROADSWORD_FOOD, 0, 0,
        BUILDING_ARMYCAMP_UPGRADE_BROADSWORD_GOLD, 0, 0, 0, 0);
    add("复合弓科技", BUILDING_RANGE, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW, 1,
        BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_FOOD,
        BUILDING_RANGE_UPGRADE_COMPOSITE_BOW_WOOD, 0, 0, 0, 0, 0, 0);

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
    add("骑兵护甲", BUILDING_STOCK, BUILDING_STOCK_UPGRADE_DEFENSE_RIDER, 2,
        BUILDING_STOCK_UPGRADE_DEFENSE_RIDER_FOOD, 0, 0, 0,
        BUILDING_STOCK_UPGRADE_DEFENSE_RIDER_2_FOOD, 0, 0,
        BUILDING_STOCK_UPGRADE_DEFENSE_RIDER_2_GOLD);
}

// 下单一条研发：先用 ins_ret 判断上一条是否成功/已满级，再按资源与空闲建筑下新单
void UsrAI::request_research(ResearchState &r)
{
    if (r.buildingType < 0) return;
    if (r.level >= r.maxLevel) return;

    // 1) 处理上一帧在研指令的返回值
    if (r.pendingId >= 0) {
        auto it = info.ins_ret.find(r.pendingId);
        if (it == info.ins_ret.end()) return;          // 结果尚未返回，下一帧再看
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
    for (tagBuilding &b : info.buildings) {
        if (b.Type != r.buildingType) continue;
        if (b.Percent < 100) continue;
        if (b.Project != 0) continue;
        r.pendingId = BuildingAction(b.SN, r.action);
        return;
    }
}

void UsrAI::demand_research()
{
    if (phase < 2) return;   // 先全力冲铜器时代，之后再研发科技
    if (researches.empty()) init_researches();
    for (ResearchState &r : researches) request_research(r);
}

// ---------- 第三阶段：反攻（转化敌方武器工程厂取胜）----------
void UsrAI::demand_attack()
{
    if (phase < 3) return;

    // 记录敌方武器工程厂（胜利目标）
    for (tagBuilding &eb : info.enemy_buildings) {
        if (eb.Type == BUILDING_SIEGE) {
            enemySiegeSN = eb.SN;
            enemySiegeDR = eb.BlockDR * BLOCKSIDELENGTH;
            enemySiegeUR = eb.BlockUR * BLOCKSIDELENGTH;
        }
    }
    if (enemySiegeSN == -1) return;   // 尚未发现敌方基地：继续由探图逻辑寻找

    // 敌方回攻我方城市时先守家（交给 combat_tactic），暂缓推进
    if (bt_enemy_at_home()) return;

    // 兵力不足不反攻
    int totalArmy = 0;
    for (tagArmy &a : info.armies)
        if (a.Sort != AT_PRIEST) totalArmy++;
    if (totalArmy < 8) return;

    // 目标点 = 敌方武器工程厂
    double tx = enemySiegeDR, ty = enemySiegeUR;

    // 全军推进 / 交战
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) continue;
        if (a.NowState == HUMAN_STATE_ATTACKING) continue;   // 已在交战，不打断

        // 1) 附近有敌方单位 → 先打人（否则野外行军时士兵一直挨打不还手）
        int unitSN = -1;
        double ubest = 1e18;
        for (tagArmy &e : info.enemy_armies) {
            double d = calDistance(a.DR, a.UR, e.DR, e.UR);
            if (d < ubest) { ubest = d; unitSN = e.SN; }
        }
        if (unitSN == -1) {
            for (tagFarmer &e : info.enemy_farmers) {
                double d = calDistance(a.DR, a.UR, e.DR, e.UR);
                if (d < ubest) { ubest = d; unitSN = e.SN; }
            }
        }
        if (unitSN != -1 && ubest < 12 * BLOCKSIDELENGTH) {
            HumanAction(a.SN, unitSN);
            continue;
        }

        // 2) 附近有敌方建筑 → 拆最近的
        int nearSN = -1;
        double best = 1e18;
        for (tagBuilding &eb : info.enemy_buildings) {
            double d = calDistance(a.DR, a.UR,
                                  eb.BlockDR * BLOCKSIDELENGTH,
                                  eb.BlockUR * BLOCKSIDELENGTH);
            if (d < best) { best = d; nearSN = eb.SN; }
        }
        if (nearSN != -1 && best < 12 * BLOCKSIDELENGTH) {
            HumanAction(a.SN, nearSN);
            continue;
        }

        // 3) 否则继续朝武器工程厂推进
        HumanMove(a.SN, tx, ty);
    }

    // 祭司随军压上，贴近武器工程厂后发动转化（胜利条件）
    tagArmy *priest = nullptr;
    for (tagArmy &a : info.armies)
        if (a.Sort == AT_PRIEST) { priest = &a; break; }
    if (priest == nullptr) return;

    double pd = calDistance(priest->DR, priest->UR, tx, ty);
    if (pd < 12 * BLOCKSIDELENGTH) {
        HumanAction(priest->SN, enemySiegeSN);   // 已抵近：内核负责贴近并转化
    } else {
        // 推进同样做节流，避免每帧重复下同一道移动指令
        int reissue = 2000 / TimePerFrame;   // 2 秒
        if (reissue < 1) reissue = 1;
        if (priestOrderFrame == 0 || info.GameFrame - priestOrderFrame >= reissue) {
            priestOrderFrame = info.GameFrame;
            HumanMove(priest->SN, tx, ty);
        }
    }
}

// 水域及其相邻一格都视为不可站立：
// 单位贴着水边寻路时容易卡住（岸边格常是斜坡或被判定为不可达），
// 因此选点时把"水域 + 岸边一格 + 斜坡"一并排除。
bool UsrAI::block_is_water_side(int x, int y)
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
bool UsrAI::find_free_spot_near(int cx, int cy, int r0, int r1, int &bx, int &by)
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
bool UsrAI::block_is_standable(int i, int j)
{
    if (info.theMap == nullptr) return false;
    int w = (int)info.theMap->size();
    if (w <= 0) return false;
    int h = (int)(*info.theMap)[0].size();
    if (w > 505) w = 505;
    if (h > 505) h = 505;
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

// 取下一个待访问的环上路点（环形广度优先）。
// 环半径从 SCOUT_RING_START 开始，每圈按弧长均匀布点（间距 ≈SCOUT_ARC_SPACING），
// 一圈扫完（或剩下的点在图外/不可站立）则半径 +SCOUT_RING_STEP。
bool UsrAI::next_ring_point(int &bx, int &by)
{
    if (info.theMap == nullptr) return false;
    int w = (int)info.theMap->size();
    if (w <= 0) return false;
    int h = (int)(*info.theMap)[0].size();
    if (w > 505) w = 505;
    if (h > 505) h = 505;

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
    int maxRing = w + h;                 // 足够大：扫到最远边界
    if (ringRadius == 0) ringRadius = SCOUT_RING_START;

    for (int guard = 0; guard < 8192; guard++) {
        if (ringRadius > maxRing) return false;      // 全部扫完

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

// 取"防御锚点"块坐标：优先己方已建成的箭塔（祭司躲到塔下才有掩护），
// 没有箭塔时退回市镇中心。找不到返回 false。
bool UsrAI::get_defense_anchor(int &cx, int &cy)
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
bool UsrAI::find_home_spot(int &bx, int &by, int attempt)
{
    int cx = -1, cy = -1;
    if (!get_defense_anchor(cx, cy)) return false;

    int r0 = 2 + attempt * 3;        // 先在锚点紧邻处找，卡住再向外扩
    return find_free_spot_near(cx, cy, r0, r0 + 3, bx, by);
}

// 探图途中遇到敌人的处置：**不是回村**，而是朝"背离附近所有敌人"的方向撤离，
// 拉开距离后继续探图。撤离指令下得勤一些（500ms），别让慢速单位追上来。
void UsrAI::scout_retreat(tagArmy *priest)
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
    for (tagFarmer &e : info.enemy_farmers) accumulate(e.DR, e.UR);

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
    if (priestOrderFrame != 0 && info.GameFrame - priestOrderFrame < gap) return;
    priestOrderFrame = info.GameFrame;

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
void UsrAI::recall_priest_home(tagArmy *priest)
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
bool UsrAI::priest_heal(tagArmy *priest)
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

    // 选最该治的伤兵：血最少的优先，同血量取离祭司近的；只治 HEAL_MAX_DIST 以内的
    int target = -1;
    int bestBlood = 0;
    double bestDist = 1e18;
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) continue;
        if (a.MaxBlood <= 0 || a.Blood >= a.MaxBlood) continue;   // 满血不用治
        double d = calDistance(priest->DR, priest->UR, a.DR, a.UR);
        if (d > maxDist) continue;
        if (target == -1 || a.Blood < bestBlood
            || (a.Blood == bestBlood && d < bestDist)) {
            target = a.SN;
            bestBlood = a.Blood;
            bestDist = d;
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

// ---------- 探路：祭司以营地为圆心做环形广度优先搜索 ----------
// 一圈一圈向外扫：环半径从 SCOUT_RING_START 开始，每圈按弧长均匀布点（间距约 8 格），
// 等角度路点，逐个走过去；一圈扫完半径 +SCOUT_RING_STEP。
// 好处：不会一条线扎得很远，也不会走回头路；扫完所有环后回村。
// 回村时间与旧逻辑一致（3.5 分钟）。
void UsrAI::demand_scout()
{
    // 已发现敌方武器工程厂后，第三阶段祭司随军行动，不再单独探图
    if (phase >= 3 && enemySiegeSN != -1) return;

    // 反攻阶段若仍未找到敌方基地，则必须继续探索（否则无法取胜）
    bool mustFindBase = (phase >= 3);

    // 探路者 = 祭司
    tagArmy *priest = nullptr;
    for (tagArmy &a : info.armies)
        if (a.Sort == AT_PRIEST) { priest = &a; break; }
    if (priest == nullptr) return;   // 祭司不在（死亡即游戏结束）

    // 敌方武器工程厂是胜利目标（转化它即获胜），探图时持续记录其位置
    for (tagBuilding &eb : info.enemy_buildings) {
        if (eb.Type == BUILDING_SIEGE) {
            enemySiegeSN = eb.SN;
            enemySiegeDR = eb.BlockDR * BLOCKSIDELENGTH;
            enemySiegeUR = eb.BlockUR * BLOCKSIDELENGTH;
        }
    }

    // ---- 敌方正在打我方的家：祭司交给 combat_tactic（箭塔拉仇恨 + 转化）----
    // 这一段必须放在"时间到回村"之前，否则回村指令会把同一帧刚下的转化指令覆盖掉。
    // 只有祭司还在外面很远时才叫它回村，已经在塔/中心附近就让它专心转化。
    if (bt_enemy_at_home()) {
        int ax = -1, ay = -1;
        if (get_defense_anchor(ax, ay)
            && calDistance(priest->DR, priest->UR,
                           ax * BLOCKSIDELENGTH, ay * BLOCKSIDELENGTH)
               > SCOUT_HOME_CALL_RADIUS * BLOCKSIDELENGTH) {
            recall_priest_home(priest);
        }
        return;
    }

    // ---- 时间到：探图结束，回村待命（躲到箭塔下）----
    int returnFrame = (int)(3.5 * 60 * 1000.0 / TimePerFrame);
    bool timeUp = (info.GameFrame > returnFrame);
    if (timeUp && !mustFindBase) {
        // 空余时间（家里没敌袭、又在治疗窗口内）：先给伤兵回血，别闲着；
        // 没有伤兵时 priest_heal 返回 false，自然落到回村逻辑上。
        if (priest_heal(priest)) return;
        recall_priest_home(priest);
        return;
    }

    // ---- 探图途中遇到敌人：记录位置 + 朝背离方向撤离（不回村）----
    // 祭司在家里时不撤：交给 combat_tactic 的「箭塔拉仇恨 + 祭司转化」
    record_enemy_spots();
    bool atHome = false;
    for (tagBuilding &b : info.buildings) {
        if (b.Type == BUILDING_CENTER && b.Percent >= 100) {
            atHome = (calDistance(priest->DR, priest->UR,
                                  b.BlockDR * BLOCKSIDELENGTH,
                                  b.BlockUR * BLOCKSIDELENGTH)
                      < HOME_DEFEND_RADIUS * BLOCKSIDELENGTH);
            break;
        }
    }
    if (!atHome
        && enemy_near(priest->DR, priest->UR, SCOUT_THREAT_RADIUS * BLOCKSIDELENGTH)) {
        scout_retreat(priest);
        return;
    }

    // 卡住检测：1 秒内位移不足 1 格 → 该路点不可达，跳到下一个
    bool stuck = false;
    int interval = 1000 / TimePerFrame;
    if (interval < 1) interval = 1;
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

    // 走到路点（空闲）或被卡住时才取下一个路点
    if (priest->NowState != HUMAN_STATE_IDLE && !stuck) return;

    // 取点节流：刚下过指令、单位可能还没进入行走状态时不重复取点
    if (!stuck) {
        int gap = 300 / TimePerFrame;
        if (gap < 1) gap = 1;
        if (priestOrderFrame != 0 && info.GameFrame - priestOrderFrame < gap) return;
    }

    int tx = -1, ty = -1;
    if (!next_ring_point(tx, ty)) {
        recall_priest_home(priest);   // 所有环都扫完了：回村待命
        return;
    }

    priestOrderFrame = info.GameFrame;
    HumanMove(priest->SN, (tx + 0.5) * BLOCKSIDELENGTH, (ty + 0.5) * BLOCKSIDELENGTH);
}

// ---------- 【旧逻辑，保留但不使用】前沿点 + 步长选点 ----------
// 本函数当前没有任何地方调用（已被 demand_scout() 的"环形广度优先"取代），
// 但保留下来以作参考，所以要保证它能编译过。
// 注意：函数内所有 "格数" 都先取成 double（bsl），再和 double 坐标一起运算。
// 不能直接写 `x * BLOCKSIDELENGTH - someDouble`——BLOCKSIDELENGTH 是引擎的
// Double（定点类型），与 double 做二元运算会产生重载歧义（Fixed 能由 __int128
// 构造，所以成员 operator-(const Fixed&) 和内置 operator-(double,double) 同时可行）。
void UsrAI::demand_scout_frontier()
{
    // 已发现敌方武器工程厂后，第三阶段祭司随军行动，不再单独探图
    if (phase >= 3 && enemySiegeSN != -1) return;

    // 反攻阶段若仍未找到敌方基地，则必须继续探索（否则无法取胜）
    bool mustFindBase = (phase >= 3);

    // 探路者 = 祭司（视野 12、速度 2.03，探图效率最高）
    tagArmy *priest = nullptr;
    for (tagArmy &a : info.armies)
        if (a.Sort == AT_PRIEST) { priest = &a; break; }
    if (priest == nullptr) return;   // 祭司不在（死亡即游戏结束）

    // 敌方武器工程厂是胜利目标（转化它即获胜），探图时持续记录其位置
    for (tagBuilding &eb : info.enemy_buildings) {
        if (eb.Type == BUILDING_SIEGE) {
            enemySiegeSN = eb.SN;
            enemySiegeDR = eb.BlockDR * BLOCKSIDELENGTH;
            enemySiegeUR = eb.BlockUR * BLOCKSIDELENGTH;
        }
    }

    // 3.5 分钟开始返程，留足 30s 路程
    int returnFrame = (int)(3.5 * 60 * 1000.0 / TimePerFrame);
    bool timeUp = (info.GameFrame > returnFrame);

    // ---- 时间到：探图结束，回村待命 ----
    // 例外：反攻阶段仍未发现敌方基地时继续探索（否则无法取胜）
    if (timeUp && !mustFindBase) {
        recall_priest_home(priest);
        return;
    }

    // ---- 探图 ----
    if (info.theMap == nullptr) return;
    int w = (int)info.theMap->size();
    if (w < 2) return;
    int h = (int)(*info.theMap)[0].size();
    if (h < 2) return;
    if (w > 505) w = 505;   // scoutSeen / scoutFront 的上界保护
    if (h > 505) h = 505;

    // BLOCKSIDELENGTH 是引擎的 Double（定点类型），和 double 混算容易触发重载歧义，
    // 这里先取成普通 double，本函数后面全部用 bsl 参与运算。
    const double bsl = BLOCKSIDELENGTH;

    // 卡住检测：每 1 秒采样一次位置，位移过小则判定上次目标不可达，本次跳过它。
    // 必须放在"空闲判断"之前：祭司被挡住时内核可能仍标记为 WALKING，
    // 若先按状态 return，这里就永远检测不到卡住、也就永远不会换目标。
    bool stuck = false;
    int checkInterval = 1000 / TimePerFrame;   // 1 秒对应的帧数（25fps → 25 帧）
    if (checkInterval < 1) checkInterval = 1;
    if (scoutCheckFrame == 0) {
        scoutCheckFrame = info.GameFrame;
        scoutCheckDR = priest->DR;
        scoutCheckUR = priest->UR;
    } else if (info.GameFrame - scoutCheckFrame >= checkInterval) {
        double movedDR = priest->DR - scoutCheckDR; if (movedDR < 0) movedDR = -movedDR;
        double movedUR = priest->UR - scoutCheckUR; if (movedUR < 0) movedUR = -movedUR;
        if (movedDR < 1.0 && movedUR < 1.0) stuck = true;   // 1 秒内位移不足 1 格
        scoutCheckFrame = info.GameFrame;
        scoutCheckDR = priest->DR;
        scoutCheckUR = priest->UR;
    }

    // ---- 探图途中遇到敌人：记录其位置 + 朝背离方向撤离（不回村）----
    // 回村只由上面的时间条件决定。
    // 祭司在家里（我方城市范围内）时不撤：那种情况交给 combat_tactic 的
    // 「箭塔拉仇恨 + 祭司转化」处理，免得祭司把防守位置让出来。
    record_enemy_spots();
    bool atHome = false;
    for (tagBuilding &b : info.buildings) {
        if (b.Type == BUILDING_CENTER && b.Percent >= 100) {
            atHome = (calDistance(priest->DR, priest->UR,
                                  b.BlockDR * BLOCKSIDELENGTH,
                                  b.BlockUR * BLOCKSIDELENGTH)
                      < HOME_DEFEND_RADIUS * BLOCKSIDELENGTH);
            break;
        }
    }
    if (!atHome
        && enemy_near(priest->DR, priest->UR, SCOUT_THREAT_RADIUS * BLOCKSIDELENGTH)) {
        scout_retreat(priest);   // 立刻撤离，不等当前探图目标
        return;
    }

    // 空闲时才选新目标；卡住时即使正在移动也强制换目标
    if (priest->NowState != HUMAN_STATE_IDLE && !stuck) return;

    // 选点节流：刚下过移动指令、单位可能还没进入行走状态时不重复选点，
    // 否则一旦目标不可达（单位一直 IDLE），就会每帧换一个新目标，把前沿点瞬间耗光。
    if (!stuck) {
        int gap = 300 / TimePerFrame;
        if (gap < 1) gap = 1;
        if (priestOrderFrame != 0 && info.GameFrame - priestOrderFrame < gap) return;
    }

    // ==================== Frontier 探索（按步长选点）====================
    // 1) 重算前沿格 scoutFront：已知可站的陆地格，且邻域存在未探索格
    //    —— 走到这里就能把探索前沿继续往前推；
    //    水域及其相邻一格（岸边）直接排除，避免祭司在水边卡住。
    for (int i = 0; i < w; i++)
        for (int j = 0; j < h; j++)
            scoutFront[i][j] = 0;

    for (int i = 1; i < w - 1; i++) {
        for (int j = 1; j < h - 1; j++) {
            int selfType = (*info.theMap)[i][j].type;
            if (selfType != MAPPATTERN_GRASS && selfType != MAPPATTERN_DESERT
                && selfType != MAPPATTERN_SHOAL)
                continue;
            if (block_is_water_side(i, j)) continue;   // 水边一格视为水域

            // 邻域必须存在未探索格（才算是探索前沿）
            bool nearUnknown = false;
            for (int di = -1; di <= 1 && !nearUnknown; di++)
                for (int dj = -1; dj <= 1 && !nearUnknown; dj++)
                    if ((*info.theMap)[i + di][j + dj].type == MAPPATTERN_UNKNOWN)
                        nearUnknown = true;
            if (!nearUnknown) continue;

            // 排除被资源（树/石/浆果）或建筑占用的格子
            bool blocked = false;
            for (tagResource &r : info.resources)
                if (r.BlockDR == i && r.BlockUR == j) { blocked = true; break; }
            if (!blocked)
                for (tagBuilding &b : info.buildings) {
                    int bs = building_size(b.Type);
                    if (i >= b.BlockDR && i < b.BlockDR + bs &&
                        j >= b.BlockUR && j < b.BlockUR + bs) { blocked = true; break; }
                }
            if (blocked) continue;

            scoutFront[i][j] = 1;
        }
    }

    // 2) 选下一个探索目标：在所有"没走过"的前沿格里打分，分越低越好：
    //      score = |d - SCOUT_STRIDE|            —— 步长拟合（每步刚好揭开一圈新区域）
    //            + 回头惩罚（目标在行进方向后方时）
    //            + 地图边缘降权（越靠近边界/角落权重越低）
    //    只标记真正要去的那一格，不再把周围一整片标记掉，
    //    这样才能均匀地向外铺开（兼顾广度），而不是只探出一条窄走廊。
    int tx = -1, ty = -1;
    {
        bool haveHead = (scoutHeadDR != 0.0 || scoutHeadUR != 0.0);
        double bestScore = 1e18;
        int fx = -1, fy = -1;             // 兜底：不考虑敌人时的最优
        double bestFallback = 1e18;
        for (int i = 1; i < w - 1; i++) {
            for (int j = 1; j < h - 1; j++) {
                if (!scoutFront[i][j] || scoutSeen[i][j]) continue;
                double tDR = i * bsl;
                double tUR = j * bsl;
                double dx = tDR - priest->DR;
                double dy = tUR - priest->UR;
                double d = calDistance(priest->DR, priest->UR, tDR, tUR);
                if (d < 3.0 * bsl) continue;   // 太近（脚下）：不作为目标

                double score = d - SCOUT_STRIDE * bsl;
                if (score < 0) score = -score;           // |d - 步长|，越小越优

                if (haveHead && d > 1e-6) {
                    // 回头惩罚：目标在行进方向后方时，越远罚得越重
                    double dot = (dx * scoutHeadDR + dy * scoutHeadUR) / d;
                    if (dot < 0)
                        score += (-dot) * d * SCOUT_BACK_PENALTY;
                }

                // 地图边缘/角落降权：离边界越近越不优先
                int edge = i;
                if (j < edge) edge = j;
                if (w - 1 - i < edge) edge = w - 1 - i;
                if (h - 1 - j < edge) edge = h - 1 - j;
                if (edge < SCOUT_EDGE_MARGIN)
                    score += (SCOUT_EDGE_MARGIN - edge)
                             * SCOUT_EDGE_PENALTY * bsl;

                if (score < bestFallback) { bestFallback = score; fx = i; fy = j; }

                // 避开敌人所在方向
                if (spot_near(tDR, tUR, SPOT_AVOID_RADIUS * bsl)) continue;
                if (score < bestScore) { bestScore = score; tx = i; ty = j; }
            }
        }
        // 候选全被敌人挡住时退而求其次
        if (tx == -1) { tx = fx; ty = fy; }
    }

    // 本轮没找到可探索目标：回村待命，下一帧继续尝试（不结束探图）
    if (tx == -1) {
        recall_priest_home(priest);   // 目标取市中心附近的空块，不要用中心自己的块
        return;
    }

    scoutSeen[tx][ty] = 1;            // 只标记真正要去的那一格

    // 记录本次探索方向：下次选点据此惩罚"走回头路"
    {
        double hx = tx * bsl - priest->DR;
        double hy = ty * bsl - priest->UR;
        double hl = sqrt(hx * hx + hy * hy);
        if (hl > 1e-6) {
            scoutHeadDR = hx / hl;
            scoutHeadUR = hy / hl;
        }
    }

    priestOrderFrame = info.GameFrame;
    HumanMove(priest->SN, tx * bsl, ty * bsl);
}

// ---------- 派发：排序 + 派发 ----------
void UsrAI::bt_dispatch()
{
    sort_tasks();
    assign_tasks();
}

void UsrAI::sort_tasks()
{
    std::sort(taskQueue.begin(), taskQueue.end(),
        [](const Task &a, const Task &b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.id < b.id;
        });
}

void UsrAI::assign_tasks()
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

    auto find_idle = [&]() -> tagFarmer* {
        for (tagFarmer &f : info.farmers) {
            if (f.FarmerSort != FARMERTYPE_FARMER) continue;
            if (f.NowState != HUMAN_STATE_IDLE) continue;
            if (assignedThisFrame.count(f.SN)) continue;
            return &f;
        }
        return nullptr;
    };

    for (Task &t : taskQueue) {
        if (t.state != TASK_WAITING) continue;

        if (t.type == TASK_GATHER) {
            tagFarmer *f = find_idle();
            if (f == nullptr) continue;

            int resSN = -1;
            double best = 1e18;
            if (t.resourceType == GATHER_FARM) {
                // 农田是建筑：找最近且有剩余食物的农田
                for (tagBuilding &b : info.buildings) {
                    if (b.Type != BUILDING_FARM) continue;
                    if (b.Percent < 100 || b.Cnt <= 0) continue;
                    if (lockedRes.count(b.SN)) continue;
                    double d = calDistance(f->DR, f->UR,
                                           b.BlockDR * BLOCKSIDELENGTH,
                                           b.BlockUR * BLOCKSIDELENGTH);
                    if (d < best) { best = d; resSN = b.SN; }
                }
            } else {
                // 两遍扫描：
                //   第一遍只在"没挤满"（已经挂了 < GATHER_PER_RESOURCE_MAX 人）
                //   的资源点里选最近的；
                //   第一遍一个都没找到（资源点太少）时，第二遍放开限制选最近的，
                //   宁可挤一点也不要让村民干等。
                for (int pass = 0; pass < 2 && resSN == -1; pass++) {
                    for (tagResource &r : info.resources) {
                        if (r.Type != t.resourceType) continue;
                        // 活动物（Cnt=0 但 Blood>0）也允许选中，用于打猎；尸体/普通资源看 Cnt
                        if (r.Cnt <= 0 && r.Blood <= 0) continue;
                        if (lockedRes.count(r.SN)) continue;
                        if (pass == 0
                            && gatherers_on(r.SN) >= GATHER_PER_RESOURCE_MAX)
                            continue;
                        double d = calDistance(f->DR, f->UR, r.DR, r.UR);
                        if (d < best) { best = d; resSN = r.SN; }
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
            if (t.resourceType == GATHER_FARM)
                lockedRes.insert(resSN);   // 农田互斥；其它资源允许多人同时采
        }
        else if (t.type == TASK_BUILD) {
            tagFarmer *f = find_idle();
            if (f == nullptr) continue;

            int size = building_size(t.buildingType);

            // 锚点优先级：
            //   房屋 → 挨着已有房屋（聚成居住区）
            //   资源旁的谷仓/仓库（resourceType/targetSN 有值）→ 挨着目标资源点
            //   其余 → 以市镇中心为中心
            int ax = -1, ay = -1;
            bool anchorIsCenter = false;   // 锚点是不是市中心（需给它留通道）
            if (t.buildingType == BUILDING_HOME) {
                for (tagBuilding &b : info.buildings) {
                    if (b.Type == BUILDING_HOME) { ax = b.BlockDR; ay = b.BlockUR; break; }
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
                        anchorIsCenter = true;
                        break;
                    }
                }
            }
            if (ax == -1) continue;   // 无锚点（异常）

            int x = -1, y = -1;
            // 以锚点为中心一圈一圈向外扩（环形搜索）；
            //   · 资源旁的存放建筑 → 从 2 格起找，尽快贴着资源建；
            //   · 农田 → 紧贴市中心（环距 1 格），村民采完走一格就能上交；
            //   · 房屋 → 挨着已有房屋，聚成居住区；
            //   · 其余（市场/兵营/靶场/马厩/学院/箭塔…）→ 从 CENTER_BUILD_START_R
            //     格起找，把市中心周围那圈地面让给农田。
            int startR = CENTER_BUILD_START_R;
            int step   = 4;
            if (t.resourceType != -1) startR = 2;
            if (t.buildingType == BUILDING_HOME) startR = 4;
            if (t.buildingType == BUILDING_FARM) { startR = 2; step = 1; }
            for (int r = startR; r <= 48 && x == -1; r += step) {
                for (int i = -r; i <= r && x == -1; i++) {
                    for (int j = -r; j <= r && x == -1; j++) {
                        int di = i < 0 ? -i : i;
                        int dj = j < 0 ? -j : j;
                        if (di != r && dj != r) continue;   // 只取本圈环上的点
                        if (!find_block(ax + i, ay + j, size, size)) continue;
                        // 以市中心为锚点的建筑（含农田）：别把村民进出的通道堵了
                        if (anchorIsCenter
                            && blocks_home_corridor(ax, ay, ax + i, ay + j, size))
                            continue;
                        x = ax + i;
                        y = ay + j;
                    }
                }
            }
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
}

void UsrAI::recycle_tasks()
{
    for (Task &t : taskQueue) {
        if (t.state == TASK_DONE || t.state == TASK_FAILED) continue;

        if (t.type == TASK_GATHER) {
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
                        farmerIdle = (f.NowState == HUMAN_STATE_IDLE);
                        break;
                    }
            }
            // 注意：村民阵亡时目标可能还在，必须用 farmerGone 收尾，
            // 否则任务永远停在 ASSIGNED、占着 active_gather 名额，导致该资源不再派新任务。
            if (targetGone || farmerIdle || farmerGone)
                t.state = TASK_DONE;
        }
        else if (t.type == TASK_BUILD) {
            // 0) 被派去建造的村民不在了（阵亡）→ 立即失败并释放占位
            if (t.state != TASK_DONE && t.state != TASK_FAILED && t.farmerSN != -1) {
                bool gone = true;
                for (tagFarmer &f : info.farmers)
                    if (f.SN == t.farmerSN) { gone = false; break; }
                if (gone) t.state = TASK_FAILED;
            }

            // 1) 内核返回错误：建造被拒绝，立即失败并释放占位
            if (t.targetSN >= 0 && info.ins_ret.count(t.targetSN)) {
                if (info.ins_ret[t.targetSN] < 0)
                    t.state = TASK_FAILED;
            }

            // 2) 建筑建成
            if (t.state != TASK_FAILED && t.blockDR != -1) {
                for (tagBuilding &b : info.buildings) {
                    if (b.Type == t.buildingType && b.BlockDR == t.blockDR
                        && b.BlockUR == t.blockUR && b.Percent >= 100) {
                        t.state = TASK_DONE;
                        break;
                    }
                }
            }

            // 3) 超时
            if (t.state != TASK_DONE && t.state != TASK_FAILED
                && info.GameFrame - t.startFrame > 60 * 120)
                t.state = TASK_FAILED;

            // 4) 失败时释放占位（建成则保留占位）
            if (t.state == TASK_FAILED && t.blockDR != -1) {
                int size = building_size(t.buildingType);
                for (int i = t.blockDR; i < t.blockDR + size; i++)
                    for (int j = t.blockUR; j < t.blockUR + size; j++)
                        MAP[i][j] = 0;   // 释放占位
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
void UsrAI::combat_tactic()
{
    // 防御触发条件：敌方必须已逼近我方城市
    if (!bt_enemy_at_home()) return;

    // 收集可见敌人（军队优先，其次农民）
    std::vector<int> enemies;
    for (tagArmy &e : info.enemy_armies) enemies.push_back(e.SN);
    for (tagFarmer &f : info.enemy_farmers) enemies.push_back(f.SN);
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
        for (tagFarmer &e : info.enemy_farmers) {
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
        //   - 其余：按距离，但超出 PRIEST_CONVERT_RADIUS 要加罚，
        //     免得祭司丢下脚边的敌人跑去追远处那个（路上还会被反杀）。
        // 不能只按"最近"排：enemy_armies 每帧都会被打乱，按下标取会选到很远的目标。
        int target = -1;
        double best = 1e18;
        const double bsl = BLOCKSIDELENGTH;      // 1 格 = 多少细节坐标
        const double nearMax = PRIEST_CONVERT_RADIUS * bsl;
        auto consider = [&](int sn, double dr, double ur, bool attacking) {
            if (sn == towerFocusSN) return;   // 留给箭塔继续拉仇恨
            double d = calDistance(priest->DR, priest->UR, dr, ur);
            double score = d;
            if (d > nearMax) score += 100.0 * bsl;   // 离太远：加罚，别丢下近的去追
            if (attacking)   score -= 200.0 * bsl;   // 正咬着祭司：最优先拉下来
            if (score < best) { best = score; target = sn; }
        };
        for (tagArmy &e : info.enemy_armies)
            consider(e.SN, e.DR, e.UR, attackingPriest(e.SN));
        for (tagFarmer &e : info.enemy_farmers)
            consider(e.SN, e.DR, e.UR, false);
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
    // 就近攻击可见敌人；已在攻击状态的士兵不打断，避免来回改目标损失输出
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) continue;
        if (a.NowState == HUMAN_STATE_ATTACKING) continue;

        int target = -1;
        double best = 1e18;
        for (tagArmy &e : info.enemy_armies) {
            double d = calDistance(a.DR, a.UR, e.DR, e.UR);
            if (d < best) { best = d; target = e.SN; }
        }
        if (target == -1) {
            for (tagFarmer &e : info.enemy_farmers) {
                double d = calDistance(a.DR, a.UR, e.DR, e.UR);
                if (d < best) { best = d; target = e.SN; }
            }
        }
        if (target != -1) HumanAction(a.SN, target);
    }
}

// ==================== 行为树节点实现 ====================
UsrAI::BTStatus UsrAI::BTSelector::tick(BTContext &ctx)
{
    if (ctx.traceOn) ctx.trace.push_back(btName);
    for (BTNodePtr &c : children) {
        BTStatus s = c->tick(ctx);
        if (s != BTStatus::Failure) return s;   // Success / Running 都直接返回
    }
    return BTStatus::Failure;
}

UsrAI::BTStatus UsrAI::BTSequence::tick(BTContext &ctx)
{
    if (ctx.traceOn) ctx.trace.push_back(btName);
    for (BTNodePtr &c : children) {
        BTStatus s = c->tick(ctx);
        if (s != BTStatus::Success) return s;
    }
    return BTStatus::Success;
}

UsrAI::BTStatus UsrAI::BTInverter::tick(BTContext &ctx)
{
    if (ctx.traceOn) ctx.trace.push_back(btName);
    BTStatus s = child->tick(ctx);
    if (s == BTStatus::Success) return BTStatus::Failure;
    if (s == BTStatus::Failure) return BTStatus::Success;
    return s;   // Running
}

UsrAI::BTStatus UsrAI::BTLeaf::tick(BTContext &ctx)
{
    if (ctx.traceOn) ctx.trace.push_back(btName);
    if (cond && !cond(ctx)) return BTStatus::Failure;                 // 条件不满足
    if (action) return action(ctx) ? BTStatus::Success : BTStatus::Failure;
    return BTStatus::Success;                                          // 仅条件且通过
}

// ---------- 叶子行为 ----------
// 指定点半径内是否有可见敌军（用于"敌军是否已逼近某处"的判定）
bool UsrAI::enemy_near(double dr, double ur, double radius)
{
    for (tagArmy &e : info.enemy_armies)
        if (calDistance(dr, ur, e.DR, e.UR) <= radius) return true;
    for (tagFarmer &e : info.enemy_farmers)
        if (calDistance(dr, ur, e.DR, e.UR) <= radius) return true;
    return false;
}

// 记录当前可见敌人的位置：同一处敌群（10 格内）合并为一条，最多保留 16 处
void UsrAI::record_enemy_spots()
{
    auto add = [&](double dr, double ur) {
        for (EnemySpot &s : enemySpots) {
            if (calDistance(s.dr, s.ur, dr, ur) < 10 * BLOCKSIDELENGTH) {
                s.dr = dr;                  // 同一处敌群：更新到最新位置
                s.ur = ur;
                s.frame = info.GameFrame;
                return;
            }
        }
        EnemySpot s;
        s.dr = dr;
        s.ur = ur;
        s.frame = info.GameFrame;
        enemySpots.push_back(s);
        if (enemySpots.size() > 16) enemySpots.erase(enemySpots.begin());
    };

    for (tagArmy &e : info.enemy_armies) add(e.DR, e.UR);
    for (tagFarmer &e : info.enemy_farmers) add(e.DR, e.UR);
}

// 该点是否离已记录的敌点太近（探图时用来避开敌人所在方向）
// 只考虑最近看到过的敌点，陈旧记录不再影响选点。
bool UsrAI::spot_near(double dr, double ur, double radius)
{
    for (EnemySpot &s : enemySpots) {
        if (info.GameFrame - s.frame > SPOT_AVOID_FRESH_FRAMES) continue;
        if (calDistance(s.dr, s.ur, dr, ur) <= radius) return true;
    }
    return false;
}

// 敌方是否已逼近我方城市：以已建成的市镇中心为圆心、HOME_DEFEND_RADIUS 格内出现可见敌军。
bool UsrAI::bt_enemy_at_home()
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
void UsrAI::build_behavior_tree()
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
    //   build    : 建造需求（房屋 / 冲铜器链 / 学院 / 农田 / 箭塔）
    //   produce  : 生产村民
    //   army     : 第二阶段造兵（方阵兵 / 骑兵 / 弓箭手 / 棍棒兵）
    //   research : 第二阶段科技研发（市场 / 兵营 / 靶场 / 谷仓 / 仓库）
    //   gather   : 采集需求（食物 / 木 / 石 / 金 / 打猎）
    //   scout    : 探图（祭司，第三阶段停止）
    //   attack   : 第三阶段反攻 + 祭司转化敌方武器工程厂
    //   dispatch : 任务排序 + 派发
    // )
    btRoot = seq({
        leaf("sync", nullptr,
             [](BTContext &c) { c.ai->bt_sync(); return true; }),

        sel({
            seq({
                leaf("enemy_at_home", [](BTContext &c) { return c.ai->bt_enemy_at_home(); }, nullptr),
                leaf("defense",        nullptr, [](BTContext &c) { c.ai->combat_tactic(); return true; })
            }),
            leaf("no_threat", nullptr, [](BTContext &) { return true; })
        }),

        leaf("build",    nullptr, [](BTContext &c) { c.ai->demand_build();   return true; }),
        leaf("produce",  nullptr, [](BTContext &c) { c.ai->demand_produce(); return true; }),
        leaf("army",     nullptr, [](BTContext &c) { c.ai->demand_army();    return true; }),
        leaf("research", nullptr, [](BTContext &c) { c.ai->demand_research();return true; }),
        leaf("gather",   nullptr, [](BTContext &c) { c.ai->demand_gather();  return true; }),
        leaf("scout",    nullptr, [](BTContext &c) { c.ai->demand_scout();   return true; }),
        leaf("attack",   nullptr, [](BTContext &c) { c.ai->demand_attack();  return true; }),
        leaf("dispatch", nullptr, [](BTContext &c) { c.ai->bt_dispatch();    return true; })
    });
    btRoot->btName = "root";
}

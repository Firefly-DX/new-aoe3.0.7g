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
// 实测 4 张图这三样都在离市中心 11~20 格内，所以：
//   · 环半径封顶 SCOUT_RING_MAX(40) 格 —— 再外面不是祭司的活；
//   · **不要搞"见着浆果/石/金就提前收工"**（试过，坑）：这三样都在 11~20 格内，
//     祭司走到半径 ~18 就全看见了，1 分半就把自己判“探完”回村了，
//     而半径 20~40 那一圈（更多矿/树/瞪羚）全黑着——经济后面要找矿时抓瞎。
//     所以老老实实扫到 3.5 分钟（或撞上 SCOUT_RING_MAX），正好回家应付 4 点那波。
//   · 只有"侦察骑兵没了、又还没找到敌方基地"（不打就赢不了）这种绝境，
//     才允许祭司把环继续往外扩（见 next_ring_point 的 nearOnly 参数）。
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
//     · 卡住（目标走不到）→ 清掉 scoutHead 重挑，并把那个格子拉黑一段时间，
//       免得反复往一个到不了的点扎。
//   **"朝家降权"是找到大本营的关键**（营地在地图角落、敌营在斜对面），别删。
static const int SCOUT_DFS_RANGE = 40;      // 找前沿的搜索半径（格）
static const int SCOUT_DFS_MIN   = 2;       // 比这还近的前沿不选（避免原地抖）。
                                            // **别调大**：侦察兵贴在前沿边上时，
                                            // 正前方的新前沿就在 3~6 格外，调大就把
                                            // "继续往前扎"否掉了，变成只能沿边走。
static const double SCOUT_DFS_HEAD_W = 2.0; // "继续朝当前方向"权重
static const double SCOUT_DFS_FAR_W  = 1.0; // "越远越好"权重
static const int SCOUT_BAD_MS = 30000;      // 走不到的目标拉黑多久（毫秒）
static const double SCOUT_BACK_HOME_PENALTY = 0.5;  // 朝家方向降权

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

// 伐木人数软上限（用户要求："现在减少伐木人口"）。
// 人堆在树上收益递减：一棵树站不下几个人，挤在一起还砍不到
// （res_stand_spots 已经在控站位），而且伐木科技一到位
// （BUILDING_MARKET_WOOD_UPGRADE：采集速度 +50%、背包 +2）同样人数产木更快。
// 所以开猎之后把伐木人数软封顶在这里，多出来的人转去打猎；
// 下面的"木材保底"只往下压打猎人数，压不住时伐木会自然涨回来。
static const int WOOD_MAX_GATHERERS = 6;

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
// 进铜器后再补到 2 座；二三波之间再补到 3 座——祭司在塔下转化时会被弓手白嫆，
// 多一座塔能把仇恨拉走，明显减少祭司掉血。
static const int TOWER_TARGET_EARLY  = 1;
static const int TOWER_TARGET_BRONZE = 2;
static const int TOWER_TARGET_LATE   = 3;
static const int TOWER_LATE_MIN      = 8;   // 第几分钟开始补第三座（二三波之间）

// 侦察兵数量：专门用来探路的快速单位（SPEED_SCOUT = 4.07，祭司只有 2.24）。
// 保持 1 个；它占人口但不计入战斗兵。
static const int SCOUT_UNITS = 1;

// 采集点分散：同一个资源点最多同时挂 GATHER_PER_RESOURCE_MAX 个村民。
// 树 / 矿石都只占一格，周围站不下太多人（碰撞会把后到的人挤开），
// 全挤在离卸货点最近的那棵树上，结果就是谁也采不踏实。
// 资源点够多时按上限分散；一个符合条件的都没有时（资源太少）
// 会自动放开限制选最近的，不让村民干等。
static const int GATHER_PER_RESOURCE_MAX = 3;

// 采集半径硬上限（格，以市镇中心为圆心）：**超过这个距离的资源一概不采**。
// 为什幺要它：村民跑一趟远点的资源，路上时间远超干活时间，而且走远了遇到敌军
// 就是一具尸体（本 AI 不会给采集队派兵护送）。超过 100 格时单程就要走 25 秒以上，
// 还不如在家门口多派一个人。
// 四张图里有效资源都在 20 格内（见文件顶部的距离表），所以正常打法不受影响，
// 它只用来兜住“地图另一头的孤树/孤矿”。注意它是欧氏距离，不是走到了才判的路。
static const int GATHER_MAX_DIST = 100;

void UsrAI::processData()
{
    info = getInfo();

    // 首次进入时构建行为树
    if (!btRoot) build_behavior_tree();

    btCtx.info = &info;
    btCtx.ai = this;

    btRoot->tick(btCtx);
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

    // 箭塔数量目标：前期 1 座，进铜器 2 座，二三波之间补到 3 座
    const int towerLateFrame = (int)(TOWER_LATE_MIN * 60 * 1000.0 / TimePerFrame);
    if (phase >= 2) arrowTowerTarget = TOWER_TARGET_BRONZE;
    else            arrowTowerTarget = TOWER_TARGET_EARLY;
    if (info.GameFrame >= towerLateFrame) arrowTowerTarget = TOWER_TARGET_LATE;

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

    // 农田绑定表：清掉"田没了（采完被内核删）"或"人没了"的条目
    prune_farm_holders();

    recycle_tasks();
}

// 清理 farmHolder：田不存在/已采完，或农民已阵亡，就解除绑定。
// 必须做——否则被删掉的田会永远占着一条绑定，后续永远匹配不上。
void UsrAI::prune_farm_holders()
{
    for (auto it = farmHolder.begin(); it != farmHolder.end(); ) {
        bool farmAlive = false;
        for (tagBuilding &b : info.buildings)
            if (b.SN == it->first && b.Cnt > 0) { farmAlive = true; break; }
        bool farmerAlive = false;
        for (tagFarmer &f : info.farmers)
            if (f.SN == it->second) { farmerAlive = true; break; }
        if (farmAlive && farmerAlive) ++it;
        else it = farmHolder.erase(it);
    }
}

// ---------- 统计辅助 ----------
// 建造位置拉黑表（见 UsrAI.h 里的说明：防止"每几秒重下一单、每次都被内核驳回"）
bool UsrAI::build_site_ok(int x, int y)
{
    int key = (x << 12) | y;
    std::unordered_map<int,int>::iterator it = badBuildSite.find(key);
    if (it == badBuildSite.end()) return true;
    return it->second <= info.GameFrame;      // 过期即视为可用
}

void UsrAI::mark_build_site_bad(int x, int y)
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
int UsrAI::res_stand_spots(int resSN)
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

// 队列里所有"还没建成/还没失败"的建造任务，总共要花多少木头。
// 为什么需要它：内核是在**执行建造那一刻**才检查并扣资源的
// （Core_List：ACTION_INVALID_RESOURCE "当前资源不足"），
// 而我们是在**排任务那一刻**用 info.Wood 判断的。
// 同一帧排出去的多个建筑（建筑链和农田现在都是 priority 2）加起来就可能超支，
// 后执行的那个就会直接报失败。所以排队时要按"已排出的花费"预留。
int UsrAI::pending_build_wood()
{
    int sum = 0;
    for (Task &t : taskQueue) {
        if (t.type != TASK_BUILD) continue;
        if (t.state == TASK_DONE || t.state == TASK_FAILED) continue;
        sum += build_wood_cost(t.buildingType);
    }
    return sum;
}

bool UsrAI::has_resource(int rtype)
{
    // 普通资源看 Cnt；活动物 Cnt=0 但 Blood>0，也算"有资源"（可打猎）
    bool isAnimal = (rtype == RESOURCE_GAZELLE || rtype == RESOURCE_ELEPHANT
                     || rtype == RESOURCE_LION);
    for (tagResource &r : info.resources) {
        if (r.Type != rtype) continue;
        if (res_too_far(r.BlockDR, r.BlockUR)) continue;   // 离市中心太远：当它不存在
        if (r.Cnt > 0) return true;
        if (isAnimal && r.Blood > 0) return true;
    }
    return false;
}

// 该资源点是否离市镇中心太远（超过 GATHER_MAX_DIST 格）→ 一律不派人去采。
// 用**块坐标的平方距离**做整数比较：不用开方，也顺手避开了
// BLOCKSIDELENGTH（Fixed 定点类型）和 double 混算的重载歧义。
// 找不到已建成的市镇中心（异常情况）时返回 false：宁可照常采集，也不要让 AI 停摆。
bool UsrAI::res_too_far(int blockDR, int blockUR)
{
    for (tagBuilding &b : info.buildings) {
        if (b.Type != BUILDING_CENTER || b.Percent < 100) continue;
        int dx = blockDR - b.BlockDR;
        int dy = blockUR - b.BlockUR;
        return dx * dx + dy * dy > GATHER_MAX_DIST * GATHER_MAX_DIST;
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
            && info.Wood >= pending_build_wood() + BUILD_HOUSE_WOOD
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
            // （建筑链和农田现在都是 priority 2）加起来会超支，
            // 后执行的那个会被内核以 ACTION_INVALID_RESOURCE"当前资源不足"拒绝。
            if (count_done(n.type) == 0 && active_build(n.type) == 0
                && info.Wood >= pending_build_wood() + n.wood) {
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
            && info.Wood >= pending_build_wood() + BUILD_STABLE_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = 2;
            t.buildingType = BUILDING_STABLE;
            taskQueue.push_back(t);
        } else if (count_done(BUILDING_COLLAGE) == 0 && active_build(BUILDING_COLLAGE) == 0
            && info.Wood >= pending_build_wood() + BUILD_COLLAGE_WOOD) {
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
        && info.Wood >= pending_build_wood() + BUILD_FARM_WOOD + 50) {
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
    // **还要等市场建好**（2026-09 用户要求"先点出伐木科技"之后加的）：
    //   市场是伐木加工的前置，而这座仓库要 120 木 —— 不挡一下它就会跟冲铜器链抢木头，
    //   把"伐木科技"和铜器升级一起拖后。顺序应该是：市场 → 伐木加工 → 打猎仓库 → 开杀。
    // 这条不能等进铜器：否则"没仓库 → 打猎不开闸"互相等死。
    // 【注意】条件里**不再要求 !huntStarted**：2026-09 起打猎不再等仓库就能开闸
    // （先派先遣队去打），所以仓库是在打猎已经开始之后才补建的，不能因为
    // huntStarted 已经为真就永远不建这座仓库。
    if (!hunt_dropoff_ready()
        && farmerNum >= HUNT_START_POP - 2
        && info.Wood >= HUNT_START_WOOD
        && count_done(BUILDING_MARKET) > 0
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
                if (res_too_far(r.BlockDR, r.BlockUR)) continue;   // 太远：不给它建仓库
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
        if (info.Wood < pending_build_wood() + wood) continue;

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
            if (res_too_far(r.BlockDR, r.BlockUR)) continue;   // 太远：不给它建仓库
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
        if (res_too_far(r.BlockDR, r.BlockUR)) continue;   // 太远的猎物不算数
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
        int move = restNow - WOOD_MAX_GATHERERS;
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
    //
    // 【必须按"站位"分配，不能各挑各的最近】老写法是每个人各自找最近的树，
    // 结果一群人同时空下来时（浆果采完、一大批人同时交完货……）**全被派到同一棵树**，
    // 只有一两个能挤进去砍，其余原地挤着"砍不到"——这就是用户反馈的
    // "某些特定情况下砍树的还是砍不到"。现在按 res_stand_spots()（该点周围能站几人）
    // 分配，并把本帧已经派出的人数（sentNow）也算上，保证同一帧不会超卖；
    // 实在所有点都站满了，才退回"就近硬挤"（总比站着不动强）。
    {
        std::unordered_map<int,int> sentNow;   // 本帧兜底已经派到每个资源点的人数
        for (tagFarmer &f : info.farmers) {
            if (f.FarmerSort != FARMERTYPE_FARMER) continue;
            if (f.NowState != HUMAN_STATE_IDLE) continue;
            // 正负责建造的村民即使这一帧看着空闲（建造关系被内核断了）也不能拉走：
            // 拉走就烂尾了，recycle_tasks 会把它们叫回工地。
            if (on_build_task(f.SN)) continue;

            int pick = -1;
            // 两遍：第一遍只挑"还没站满"的点（树优先）；都站满了第二遍才就近硬挤
            for (int pass = 0; pass < 2 && pick == -1; pass++) {
                int treeSN = -1, anySN = -1;
                double treeD = 1e18, anyD = 1e18;
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
                    if (res_too_far(r.BlockDR, r.BlockUR)) continue;   // 太远：不采
                    int spots = res_stand_spots(r.SN);
                    if (spots <= 0) continue;   // 走不到跟前，别白跑
                    if (pass == 0
                        && gatherers_on(r.SN) + sentNow[r.SN] >= spots)
                        continue;               // 这个点已经站满了
                    double d = calDistance(f.DR, f.UR, r.DR, r.UR);
                    if (d < anyD) { anyD = d; anySN = r.SN; }
                    if (r.Type == RESOURCE_TREE && d < treeD) { treeD = d; treeSN = r.SN; }
                }
                pick = (treeSN != -1) ? treeSN : anySN;
            }

            if (pick != -1) {
                HumanAction(f.SN, pick);
                sentNow[pick]++;
            }
        }
    }
}

// ---------- 第二阶段：造兵需求 ----------
// 优先级：学院方阵兵 > 马厩骑兵 > 靶场弓箭手（复合弓科技升完后改出复合弓兵）。
// **兵营不造棍棒兵**（见函数末尾注释）。
// 每帧最多给一座空闲军事建筑下一条命令（建筑随后进入忙碌状态，自然不会重复下达）。
void UsrAI::demand_army()
{
    // 统计现有兵力（祭司不计入战斗兵；侦察兵单独算，也不计入战斗兵）
    // 注意：不再统计 AT_CLUBMAN —— 棍棒兵已经不允许生产了，留着计数只会变成
    // "赋值但从不读取"的变量（GCC -Wall 会警告）。
    int bowman = 0, cavalry = 0, hoplite = 0, scout = 0, totalArmy = 0;
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) continue;
        if (a.Sort == AT_SCOUT) { scout++; continue; }
        totalArmy++;
        if (a.Sort == AT_BOWMAN || a.Sort == AT_COMPOSITE_BOWMAN
            || a.Sort == AT_SLINGER) bowman++;
        else if (a.Sort == AT_CAVALRY || a.Sort == AT_CHARIOT) cavalry++;
        else if (a.Sort == AT_HOPLITE) hoplite++;
    }

    auto free_building = [&](int type) -> tagBuilding* {
        for (tagBuilding &b : info.buildings)
            if (b.Type == type && b.Percent >= 100 && b.Project == 0)
                return &b;
        return nullptr;
    };

    if (info.Human_Num + 1 > info.Human_MaxNum) return;   // 人口已满

    // ---- 侦察兵：专职探路。速度 4.07，是祭司（2.24）的 1.8 倍；
    //      马厩在**工具时代**就解锁它、而且只花食物（Development.cpp:700），
    //      所以不受下面"铜器前不造兵"的限制，马厩一建好就能出。----
    if (scout < SCOUT_UNITS) {
        tagBuilding *st0 = free_building(BUILDING_STABLE);
        if (st0 && info.Meat >= BUILDING_STABLE_CREATE_SCOUT_FOOD) {
            BuildingAction(st0->SN, BUILDING_STABLE_CREATE_SCOUT);
            return;
        }
    }

    if (phase < 2) return;   // 铜器时代前不打仗，只留上面那个侦察兵

    int target = (phase >= 3) ? armyTarget + 8 : armyTarget;
    if (totalArmy >= target) return;

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

    // 靶场：弓箭手（远程，主力）。复合弓科技升完后改出复合弓兵。
    // 【让位】复合弓科技冲刺窗口内（rangeReservedForResearch），靶场先别造兵：
    //   demand_army 在行为树里排在 demand_research 前面，靶场一空就会被造兵订单
    //   抢走，科技永远排不上队——这正是复合弓拖到 20 分钟以后还没升完的原因。
    tagBuilding *rg = free_building(BUILDING_RANGE);
    if (rg && !rangeReservedForResearch() && bowman < (target + 1) / 2) {
        // 科技已升完 → 出复合弓兵（40 食物 + 20 黄金，比普通弓兵强得多）
        if (compositeBowReady()
            && info.Meat >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_FOOD
            && info.Gold >= BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN_GOLD) {
            BuildingAction(rg->SN, BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN);
            return;
        }
        // 否则普通弓兵（40 食物 + 20 木）；黄金不够时它仍是可靠的主力
        if (info.Meat >= BUILDING_RANGE_CREATE_BOWMAN_FOOD
            && info.Wood >= BUILDING_RANGE_CREATE_BOWMAN_WOOD) {
            BuildingAction(rg->SN, BUILDING_RANGE_CREATE_BOWMAN);
            return;
        }
    }

    // 兵营：**不造棍棒兵**（用户明确要求禁掉）。
    // 理由：棍棒兵（AT_CLUBMAN，atk 最低、无护甲科技）在铜器时代之后毫无价值，
    // 造出来不但占人口（Human_MaxNum 被房屋卡着），还会被拉去防守白送。
    // 兵营留着只为研发（战斧升级/阔剑科技），生产一律走靶场/马厩/学院。
    // 注意：demand_army 下面那条"camp 造兵"分支已经删掉了，这里刻意不再补。
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
    // 【已删除】战斧升级（BUILDING_ARMYCAMP_UPGRADE_CLUBMAN）：它只强化棍棒兵，
    //   而棍棒兵已经禁止生产了（见 demand_army），研发它纯属白花食物、
    //   还占着兵营的研发位拖慢阔剑科技。Development.cpp 里阔剑科技并不以它为前置。
    add("阔剑科技",  BUILDING_ARMYCAMP, BUILDING_ARMYCAMP_UPGRADE_BROADSWORD, 1,
        BUILDING_ARMYCAMP_UPGRADE_BROADSWORD_FOOD, 0, 0,
        BUILDING_ARMYCAMP_UPGRADE_BROADSWORD_GOLD, 0, 0, 0, 0);
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
bool UsrAI::compositeBowReady()
{
    for (ResearchState &r : researches)
        if (r.action == BUILDING_RANGE_UPGRADE_COMPOSITE_BOW)
            return r.level >= r.maxLevel;
    return false;
}

bool UsrAI::compositeBowUrgent()
{
    for (ResearchState &r : researches)
        if (r.action == BUILDING_RANGE_UPGRADE_COMPOSITE_BOW)
            return r.deadlineFrame > 0 && r.level < r.maxLevel
                && info.GameFrame >= r.urgentFromFrame;
    return false;
}

// 靶场是否需要让位给复合弓科技。
// 冲刺窗口内一律让位（哪怕资源暂时不够）：让位后不再拿食物去造弓箭手，
// 食物才能攒到 180；否则"靶场一直造兵 → 食物永远不到 180 → 科技永远开不了"
// 会死循环。代价是靶场在这段时间可能空转，但冲刺窗口是 16:20 起、只到 20:00，
// 换"复合弓准时到位"是值得的。
bool UsrAI::rangeReservedForResearch()
{
    return compositeBowUrgent();
}

void UsrAI::demand_research()
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
            && info.GameFrame >= r.urgentFromFrame;
    };

    // 第一遍：只处理冲刺中的硬截止科技 —— 食物/木头优先给它们，免得被
    // 后面那几条经济科技先花掉（复合弓原本排在清单第 8 位）。
    for (ResearchState &r : researches)
        if (isUrgent(r)) request_research(r);

    // 第二遍：其余科技按原优先级（已经下过单的紧急科技在这一遍里会被跳过）
    for (ResearchState &r : researches)
        if (!isUrgent(r)) request_research(r);
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

    // 兵力不足不反攻（侦察骑兵不计入战斗兵，也不参加反攻）
    int totalArmy = 0;
    for (tagArmy &a : info.armies)
        if (a.Sort != AT_PRIEST && a.Sort != AT_SCOUT) totalArmy++;
    if (totalArmy < 8) return;

    // 敌方回攻我方城市时一般先守家（交给 combat_tactic 的箭塔+祭司）——
    // **但兵力已经攒够时不再拖**：反攻是唯一的取胜手段，硬上限是 30:00
    // （GAME_LOSE_SEC），把攒好的兵按在家里跟对方拼消耗才是最大的浪费。
    // 所以 totalArmy >= ATTACK_FORCE 时直接全军压上，家里留给箭塔 + 祭司。
    if (bt_enemy_at_home() && totalArmy < ATTACK_FORCE) return;

    // 目标点 = 敌方武器工程厂
    double tx = enemySiegeDR, ty = enemySiegeUR;

    // 全军推进 / 交战
    // 【关键】不能每帧重下指令：内核 addRelation 会先 suspendRelation 再重建，
    // 于是"走过去 → 攻击"的蓄力阶段永远走不完，看着就是"兵到位了却不打"。
    // 只在"目标变了"或"单位空闲（上一条已完成）"时才重下（见 attackOrderSN）。
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) continue;
        // 侦察骑兵只负责探路，不参加反攻（把它拉上去只会白白送掉）
        if (a.Sort == AT_SCOUT) continue;
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
        int wantSN = -1;   // -1 = 没有可打的目标：继续朝敌营推进（HumanMove）
        if (unitSN != -1 && ubest < 12 * BLOCKSIDELENGTH) {
            wantSN = unitSN;
        } else {
            // 2) 附近有敌方建筑 → 拆最近的
            int nearSN = -1;
            double best = 1e18;
            for (tagBuilding &eb : info.enemy_buildings) {
                double d = calDistance(a.DR, a.UR,
                                       eb.BlockDR * BLOCKSIDELENGTH,
                                       eb.BlockUR * BLOCKSIDELENGTH);
                if (d < best) { best = d; nearSN = eb.SN; }
            }
            if (nearSN != -1 && best < 12 * BLOCKSIDELENGTH) wantSN = nearSN;
        }

        int lastSN = -2;
        std::unordered_map<int,int>::iterator it = attackOrderSN.find(a.SN);
        if (it != attackOrderSN.end()) lastSN = it->second;

        if (wantSN != lastSN || a.NowState == HUMAN_STATE_IDLE) {
            if (wantSN >= 0) HumanAction(a.SN, wantSN);
            else             HumanMove(a.SN, tx, ty);   // 3) 继续朝武器工程厂推进
            attackOrderSN[a.SN] = wantSN;
        }
    }

    // 祭司随军压上，贴近武器工程厂后发动转化（胜利条件）
    tagArmy *priest = nullptr;
    for (tagArmy &a : info.armies)
        if (a.Sort == AT_PRIEST) { priest = &a; break; }
    if (priest == nullptr) return;

    double pd = calDistance(priest->DR, priest->UR, tx, ty);
    if (pd < 12 * BLOCKSIDELENGTH) {
        // 已抵近：让内核负责贴近并转化。**同样只在目标变化或祭司空闲时重下**，
        // 否则每帧重下会把转化关系反复中止（转化需要时间）。
        if (attackConvertSN != enemySiegeSN
            || priest->NowState == HUMAN_STATE_IDLE) {
            attackConvertSN = enemySiegeSN;
            HumanAction(priest->SN, enemySiegeSN);
        }
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

// ---------- 祭司：环形广度优先（前期"找家附近资源点"，BFS）----------
// 取下一个待访问的环上路点。
// 环半径从 SCOUT_RING_START 开始，每圈按弧长均匀布点（间距 ≈SCOUT_ARC_SPACING），
// 一圈扫完（或剩下的点在图外/不可站立）则半径 +SCOUT_RING_STEP。
// nearOnly=true：环半径到 SCOUT_RING_MAX 就收工（"家附近"扫完了）；
//                false：一路扫到地图边界（只在侦察骑兵阵亡、又还没找到敌方基地时用）。
// 选过的点记在 scoutSeen 里（侦察兵的 DFS 盯的是引擎的迷雾掩码，不用这张表）。
bool UsrAI::next_ring_point(int &bx, int &by, bool nearOnly)
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
    int maxRing = w + h;                       // 兜底：扫到最远边界
    if (nearOnly && maxRing > SCOUT_RING_MAX) maxRing = SCOUT_RING_MAX;
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
// 取下一枝的落点。stuck=true 表示上次的目标走不到（清 scoutHead 重挑 + 把目标拉黑）。
// 返回 false = 附近完全没有可去的前沿格，调用方应让单位回村。
//
// 打分（分越高越优先）：
//     score = dot × SCOUT_DFS_HEAD_W          // dot = 目标方向与 scoutHead 的夹角余弦
//           + dist/(R) × SCOUT_DFS_FAR_W      // 同样顺路时优先更远的
//           − (1 − dotAway) × BACK_HOME       // 别往回（家）的方向跑
// R = SCOUT_DFS_RANGE 内找不到前沿 → 返回 false。
bool UsrAI::next_dfs_point(tagArmy *walker, bool stuck, int &bx, int &by)
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

    // ---- 卡住：把上次给出的目标拉黑一段时间，并丢掉当前方向 ----
    // 不清方向的话，下一轮又会挑中同一个"看着最顺路、其实走不到"的前沿格，
    // 于是永远扎在那儿（侦察兵"卡死"的另一种形态）。
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
        scoutHeadDR = 0;
        scoutHeadUR = 0;
    }
    const bool haveHead = (scoutHeadDR != 0.0 || scoutHeadUR != 0.0);

    // "离家方向"：给往回走的方向降权（营地在地图角落、敌营在斜对面）
    double awayDR = 0, awayUR = 0, awayLen = 0;
    for (tagBuilding &b : info.buildings) {
        if (b.Type != BUILDING_CENTER) continue;
        double hx = b.BlockDR * bsl, hy = b.BlockUR * bsl;
        double adr = wx - hx, aur = wy - hy;
        awayLen = sqrt(adr * adr + aur * aur);
        if (awayLen > 1e-6) { awayDR = adr / awayLen; awayUR = aur / awayLen; }
        break;
    }

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

            if (score > bestScore) {
                bestScore = score;
                bestX = i;
                bestY = j;
            }
        }
    }

    if (bestX < 0) return false;      // 附近已经没有可去的前沿格了

    // 记下方向（下一轮继续往这个方向扎）和目标（卡住时用来拉黑）
    double tx = (bestX + 0.5) * bsl, ty = (bestY + 0.5) * bsl;
    double dx = tx - wx, dy = ty - wy;
    double dist = sqrt(dx * dx + dy * dy);
    if (dist > 1e-6) { scoutHeadDR = dx / dist; scoutHeadUR = dy / dist; }
    curTargetX = bestX;
    curTargetY = bestY;
    bx = bestX;
    by = bestY;
    return true;
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
        // 侦察骑兵不参与防御，也不占用祭司的治疗额度（它的命不值钱，主力兵值钱）
        if (a.Sort == AT_SCOUT) continue;
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

// ---------- 探路：祭司和侦察兵干的是两件不同的事 ----------
//   祭司（前期）—— **把家附近的资源点探出来**：
//       环形广度优先 BFS（next_ring_point），以营地为圆心一圈圈往外扫。
//       目的是让 info.resources 里出现石/金/浆果/瞪羚（没探索到的资源 AI 根本看不到），
//       后面村民去哪采、矿边要不要补仓库全指着它。
//       环半径封顶 SCOUT_RING_MAX 格，扫到就收工；平时不用额外条件提前收
//       （试过"见着浆果/石/金就提前回"，结果 1 分半就回村，半径 20~40 全黑）。
//       它不能走 DFS：速度只有 2.24，一路扎到地图另一头的话，
//       家里 4 分钟那波敌袭它赶不回来防守/转化。
//   侦察骑兵（后期）—— **找到敌军大本营（敌方武器工程厂）**：
//       DFS（next_dfs_point），一路往深处扎，被挡住才转向，
//       转向时挑"前方未知格最多、且不朝家"的方向，尽快把远处摸一遍。
// 没造出侦察兵时先由祭司代劳（走环形；此时如果连敌方基地都没找到，
// 允许它把环往外扩，见 next_ring_point 的 nearOnly 参数）。
void UsrAI::demand_scout()
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

    // 敌方武器工程厂是胜利目标（转化它即获胜），探图时持续记录其位置。
    // 这段必须放在下面的提前 return 之前，否则"最后一次看到"的位置会被丢掉。
    // 注意：enemy_buildings 是**带迷雾**的（Core.cpp 里只有 explored/visible 的才发给我们），
    // 所以侦察兵必须真的走到敌营附近，这里才可能记到东西。
    for (tagBuilding &eb : info.enemy_buildings) {
        if (eb.Type == BUILDING_SIEGE) {
            enemySiegeSN = eb.SN;
            enemySiegeDR = eb.BlockDR * BLOCKSIDELENGTH;
            enemySiegeUR = eb.BlockUR * BLOCKSIDELENGTH;
        }
    }

    // 反攻阶段若仍未找到敌方基地，则必须继续探索（否则无法取胜）
    bool mustFindBase = (phase >= 3);

    // 已发现敌方武器工程厂后：祭司进第三阶段随军行动、不再单独探图；
    // 独立侦察骑兵则回村待命（留在敌人家门口只会被打死）。
    if (phase >= 3 && enemySiegeSN != -1) {
        if (scoutIsUnit) recall_priest_home(scout);
        return;
    }

    // 探路者不是祭司时，把祭司收回村待命（治疗/防守都在家附近做）。
    // 两个保护：只在它手里没活干时下令；有敌袭时不下（否则会覆盖掉
    // combat_tactic 同一帧刚下的转化指令，这个坑之前踩过）。
    if (scoutIsUnit && priest != nullptr
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
        // 探路者是独立侦察兵：它跑得快、下面有遇敌撒离逻辑，继续探图
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

    if (scoutIsUnit) {
        // 独立侦察骑兵：只有在找到敌方基地后才回家（没找到就一直探）
        if (enemySiegeSN != -1) {
            priest_heal(priest);              // 祭司在家专心治伤兵
            recall_priest_home(scout);
            return;
        }
    } else if (timeUp && !mustFindBase) {
        // 探路者就是祭司：到点收工 → 先给伤兵回血，没伤兵就回村待命
        if (priest_heal(priest)) return;
        recall_priest_home(priest);
        return;
    }

    // ---- 探图途中遇到敌人：朝背离方向撤离（不回村）----
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
    if (!atHome
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
    //   祭司     → 环形 BFS（找家附近资源点）。默认只用 SCOUT_RING_MAX 以内的环；
    //              只有在"侦察骑兵没了、又还没找到敌方基地"（不打就输）的绝境下，
    //              才让祭司把环继续往外扩（!!mustFindBase）。
    bool gotPoint = scoutIsUnit ? next_dfs_point(scout, stuck, tx, ty)
                                : next_ring_point(tx, ty, !mustFindBase);
    if (!gotPoint) {
        // 侦察兵：四面八方都被挡死；祭司：该扫的环都扫完了。→ 回村待命
        recall_priest_home(scout);
        return;
    }

    ordFrame = info.GameFrame;
    HumanMove(scout->SN, (tx + 0.5) * BLOCKSIDELENGTH, (ty + 0.5) * BLOCKSIDELENGTH);
}


// ---------- 派发：排序 + 派发 ----------
void UsrAI::bt_dispatch()
{
    sort_tasks();
    assign_tasks();
}

// 建造位外圈一圈是否干净：检查 (x,y) 起 size×size **外扩 1 格**的环形区域，
// 里面不能有建筑 / 敌方建筑 / 资源 / 我们规划中的占位（MAP）/ 水边一格。
// 用途：农田。农田本身只有 3x3，但村民得站到旁边才能干活，
// 外圈被别的东西堵住时村民挤不进去，就会卡在那里不动。
// 注意：不检查移动单位（村民/军队会走动，不能因为路过就否掉一个位置）。
bool UsrAI::build_margin_clear(int x, int y, int size)
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
            // 正负责一个还没建完的建造任务：绝不能被派去干别的。
            // 一旦被拉走，那栋楼就永远烂尾（同位置不能再下建造单，见 on_build_task 注释）。
            if (on_build_task(f.SN)) continue;
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
            double best = 1e18;
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
                    for (tagResource &r : info.resources) {
                        if (r.Type != t.resourceType) continue;
                        // 活动物（Cnt=0 但 Blood>0）也允许选中，用于打猎；尸体/普通资源看 Cnt
                        if (r.Cnt <= 0 && r.Blood <= 0) continue;
                        if (lockedRes.count(r.SN)) continue;
                        if (res_too_far(r.BlockDR, r.BlockUR)) continue;  // 太远：不采
                        int spots = res_stand_spots(r.SN);
                        if (spots <= 0) continue;          // 够不到：任何时候都不派
                        // 第一遍：守住"政策上限 3 人"和"物理站位上限"——把人分散开，
                        //         每个人都能真的站到位置上下手；
                        // 第二遍：只剩这些资源点了，允许超员硬挤（总比站着不动强，
                        //         但"够不到"的点(failed spots<=0)依然不派）。
                        if (pass == 0) {
                            int used = gatherers_on(r.SN);
                            if (used >= GATHER_PER_RESOURCE_MAX) continue;
                            if (used >= spots) continue;
                        }
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
                        // 被内核驳回过、还在拉黑期的位置直接跳过，否则会反复挑中它、
                        // 反复被驳回（见 UsrAI.h 里 badBuildSite 的说明）
                        if (!build_site_ok(ax + i, ay + j)) continue;
                        // 以市中心为锚点的建筑（含农田）：别把村民进出的通道堵了
                        if (anchorIsCenter
                            && blocks_home_corridor(ax, ay, ax + i, ay + j, size))
                            continue;
                        // 农田：周围一圈必须干净。农田虽然占 3x3，但村民要站到旁边
                        // 才能干活，四周被别的东西堵住时会卡住不动。
                        if (t.buildingType == BUILDING_FARM
                            && !build_margin_clear(ax + i, ay + j, size))
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

// 该村民是否正挂着一个"还没建完的建造任务"。
// 【为什么必须保护】内核里建房是**下单那一刻建筑对象就创建好（0%）**，
// 村民再走过去把它一点点修起来（Core_List::addRelation 的 CoreEven_CreatBuilding
// 分支会立刻 addBuilding + theMap->add_Map_Object）。后果：
//   1) 建造关系一旦被强制中断（被自己人挤开、路被堵死、无用地形移动次数超限），
//      那栋楼就永远停在半成品，没人能接手；
//   2) **同一位置不能重新下建造单** —— is_BuildingCanBuild 里 theMap->isHaveObject
//      会把这栋半成品判成"与其他物体重叠"直接驳回，等于那笔木/石白花了。
// 所以：只要 BUILD 任务还挂在某个村民身上没建完，任何派工逻辑都不许动他。
bool UsrAI::on_build_task(int farmerSN)
{
    if (farmerSN == -1) return false;
    for (Task &t : taskQueue)
        if (t.type == TASK_BUILD && t.state == TASK_ASSIGNED
            && t.farmerSN == farmerSN)
            return true;
    return false;
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
            // 农田不需要特殊处理："谁占着这块田"存在 farmHolder 里（见 assign_tasks），
            // 任务就算被回收，下次派工时也会按 owner 把主人派回去，
            // 而且不会把已经有人占的田派给第二个人。
            if (targetGone || farmerIdle || farmerGone)
                t.state = TASK_DONE;
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

            // 4) 失败时释放占位（建成则保留占位）
            if (t.state == TASK_FAILED && t.blockDR != -1) {
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
    // 侦察骑兵不参与防御：它的任务只有探路，被卷进防守战只会白白送掉
    // （所以这里和 demand_attack 一样要把它排除掉）。
    for (tagArmy &a : info.armies) {
        if (a.Sort == AT_PRIEST) continue;
        if (a.Sort == AT_SCOUT) continue;
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
    for (BTNodePtr &c : children) {
        BTStatus s = c->tick(ctx);
        if (s != BTStatus::Failure) return s;   // Success / Running 都直接返回
    }
    return BTStatus::Failure;
}

UsrAI::BTStatus UsrAI::BTSequence::tick(BTContext &ctx)
{
    for (BTNodePtr &c : children) {
        BTStatus s = c->tick(ctx);
        if (s != BTStatus::Success) return s;
    }
    return BTStatus::Success;
}

UsrAI::BTStatus UsrAI::BTLeaf::tick(BTContext &ctx)
{
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

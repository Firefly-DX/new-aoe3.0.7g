#ifndef USRAI_H
#define USRAI_H

#include "ai.h"
#include <unordered_map>
#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <initializer_list>

extern tagGame tagUsrGame;
extern ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

class UsrAI : public AI
{
public:
    UsrAI() { this->id = 0; }
    ~UsrAI() {}

private:
    void processData() override;
    tagInfo getInfo() { return tagUsrGame.getInfo(); }
    int AddToIns(instruction ins) override
    {
        UsrIns.lock.lock();
        ins.id = UsrIns.g_id;
        UsrIns.g_id++;
        UsrIns.instructions.push(ins);
        UsrIns.lock.unlock();
        return ins.id;
    }
    void clearInsRet() override
    {
        tagUsrGame.clearInsRet();
    }
    /*##########DO NOT MODIFY THE CODE IN THE CLASS##########*/

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
        // 建造专用：建造者中途掉线时“续建重发”的节流与次数
        // （内核里楼一旦开了地基就不能重新下单，只能把原建造者叫回去接着修，
        //   所以这里只是节流计数，不是资源所有权）
        int resendFrame = 0;      // 上次续建重发的帧号
        int resendCount = 0;      // 续建重发次数（超上限则判失败重排）
    };

    std::vector<Task> taskQueue;
    int nextTaskId = 0;
    int phase = 0;                // 阶段状态机：1冲铜器 2发展军事 3反攻
    bool huntStarted = false;     // 打猎开关（人口/木头到位后锁存，开了一直开）
    bool berryPhase = true;       // 浆果阶段：城边那几丛采完就结束，之后不再采浆果
    bool berrySeen  = false;      // 是否已见到过城边的浆果丛（防止开局没探到就误判结束）
    int farmTarget = 0;           // 目标农田数（= 打算派去种田的人数，一人一格农田）
    // 农田 SN → 采集它的村民 SN（一对一绑定，镜像内核的"地主"关系）。
    // 内核是地主制：一块田只认第一个到的采集者，而且建好农田时它会把**建造者**
    // 直接转成地主（object_FinishAction）。但 tagBuilding 没有暴露地主字段，
    // AI 查不到，只能自己记一份。清理见 prune_farm_holders()。
    std::unordered_map<int,int> farmHolder;
    void prune_farm_holders();

    void sort_tasks();
    void assign_tasks();
    void recycle_tasks();
    // 该村民是否正挂着一个"还没建完的建造任务"。
    // 内核里建房是"下单那一刻建筑就创建成 0%、村民再走过去一点点修"，
    // 所以中途换人 = 那块地永远烂尾（同一位置再下单会被 is_BuildingCanBuild
    // 判成"与其他物体重叠"直接驳回）。因此这些村民要保护起来，不许被派去干别的。
    bool on_build_task(int farmerSN);

    // 战斗模块：箭塔拉仇恨 + 祭司转化防御
    // 注意：只有当敌方逼近我方城市（enemy_at_home）时才启用，避免祭司探图途中被远处敌军误触发。
    void combat_tactic();
    int convertTargetSN = -1;        // 待转化的敌方单位 SN
    int convertStuckFrame = 0;                   // 上次检查"祭司是否卡在转化目标上"的帧
    double convertStuckDR = 0, convertStuckUR = 0;
    int towerFocusSN = -1;           // 箭塔集火目标 SN（仇恨标记，锁定后不切换）
    int scoutCheckFrame = 0;                     // 上次卡住检查的帧号（祭司）
    double scoutCheckDR = -1, scoutCheckUR = -1; // 上次卡住检查时的祭司位置
    // ---- 独立侦察骑兵的专属状态 ----
    // 侦察骑兵和祭司是两个不同的单位，两者都会做"卡住检测"和"指令节流"。
    // 若共用上面那组采样变量，两边会互相把对方的位置写进去，侦察兵的卡住检测
    // 就永远不会成立：一旦某个路点走不到（隔着水/树林），它会永远停在原地，
    // 表现就是"侦察骑兵造出来不侦察"。
    int scoutUnitCheckFrame = 0;                        // 上次卡住检查的帧号（侦察兵）
    double scoutUnitCheckDR = -1, scoutUnitCheckUR = -1; // 侦察兵的位置采样
    int scoutUnitOrderFrame = 0;                        // 侦察兵移动指令上次下达帧（节流）

    // ---- 回村 ----
    // 回村目标不能取市镇中心自己占的块（那是建筑，必然不可达，会让祭司卡住），
    // 必须另找一个可站立的空块作为落脚点。
    int homeSpotX = -1, homeSpotY = -1;   // 回村落脚点块坐标
    int homeSpotTry = 0;                  // 找落脚点的尝试次数（卡住时向外扩）
    int priestOrderFrame = 0;             // 祭司移动指令上次下达帧（节流用）
    void recall_priest_home(tagArmy *priest);            // 派祭司回村（带节流与卡住换点）
    bool find_home_spot(int &bx, int &by, int attempt);  // 在箭塔（或市中心）附近找可站立空块
    bool get_defense_anchor(int &cx, int &cy);           // 防御锚点：优先己方箭塔，其次市镇中心
    void scout_retreat(tagArmy *priest);                 // 探图遇敌：朝背离敌人方向撤离
    bool priest_heal(tagArmy *priest);                   // 空余时间给伤兵回血（true = 已接管祭司）
    int healTargetSN = -1;                               // 正在治疗的伤兵 SN
    bool find_free_spot_near(int cx, int cy, int r0, int r1, int &bx, int &by);  // 找可站立空块
    bool build_margin_clear(int x, int y, int size);  // 建造位外圈一圈是否干净（农田要求）

    // 祭司环形探路的"已选过路点"表（侦察骑兵不用它：它盯的是引擎的迷雾掩码
    // `(*info.theMap)[i][j].type == MAPPATTERN_UNKNOWN`，那才是"哪里没去过"的真值来源）。
    unsigned char scoutSeen[505][505] = {{0}};

    // ---- 祭司：环形广度优先（前期"找家附近资源点"专用）----
    // 一圈一圈向外扫：每个环按角度均匀取路点，逐个走过去；一圈扫完则半径 +STEP。
    // 祭司速度只有 2.24，环形扫得匀、始终在营地附近，也来不及跑远挨打。
    // nearOnly=true（默认）时环半径封顶 SCOUT_RING_MAX 格就收工
    // —— 再外面是侦察骑兵的活。
    // 【不要加"见着矿就提前收工"】：浆果/石/金都在 11~20 格内，
    // 祭司走到半径 ~18 就全看见了，1 分半就回村，半径 20~40 那片全黑着。
    int ringRadius = 0;                        // 当前正在搜索的环半径（块）
    int ringIndex = 0;                         // 当前环上的路点下标
    bool next_ring_point(int &bx, int &by, bool nearOnly = true);

    // ---- 侦察骑兵：DFS（后期"找敌军大本营"专用）----
    // 盯着**前沿格**（已知可站立、且邻域挨着 MAPPATTERN_UNKNOWN 的格子）一路往深处扎，
    // 候选按"继续朝 scoutHeadDR/UR 方向 + 越远越好"打分。
    // **绝不能**把目标直接定在未知格上——原因见 UsrAI.cpp 顶部"先搞清楚迷雾这件事"，
    // 那样所有方向都会"目标不可站立"失败，侦察兵被锁死在已探索区里原地打转。
    double scoutHeadDR = 0, scoutHeadUR = 0;  // 当前探索方向（单位向量，一直往这个方向扎）
    int curTargetX = -1, curTargetY = -1;     // 上一次给出的 DFS 目标格（卡住时要拉黑它）
    // DFS 目标黑名单：卡住过（走不到）的格子 → 解禁帧号。
    // 规模很小（几十条），每次写入时顺手清一遍过期项即可。
    std::unordered_map<long long,int> dfsBad;
    // 取下一枝的落点：stuck=true 表示上次目标走不到（清掉 scoutHead 重挑 + 拉黑那个格子）。
    // 返回 false = 附近完全没有前沿格了（调用方应让它回村）。
    bool next_dfs_point(tagArmy *walker, bool stuck, int &bx, int &by);

    // 水域及其相邻一格都视为"不可站立"：单位贴着水边寻路容易卡住
    bool block_is_water_side(int x, int y);
    int arrowTowerTarget = 1;        // 目标箭塔数量（前期 1 座即可，进铜器后再补）
    bool arrowTowerResearched = false;   // 箭塔科技是否已研发
    int arrowTowerResearchId = -1;   // 箭塔科技研发指令 id（-1 表示未在研）
    std::unordered_map<int,int> towerTargetSN;  // 箭塔 SN → 已下达的集火目标 SN（避免每帧重复索敌）
    int towerOrderFrame = 0;                    // 上次对箭塔下令的帧号（周期性刷新用）
    int lastTowerFocusSN = -1;                  // 上次下达的集火目标 SN
    int towerAggroFrame = 0;                    // 当前集火目标"开始被箭塔打"的帧号（用于让塔先拉仇恨）

    // ==================== 第二阶段：军事（造兵 + 科技）====================
    int armyTarget = 16;             // 目标军队规模（第三阶段自动提高）
    void demand_army();              // 造兵需求
    void demand_research();          // 科技研发需求

    // 科技研发状态：同一个 Action 可用一次或两次（两级科技），用等级追踪
    struct ResearchState {
        int buildingType = -1;   // 执行建筑类型
        int action = -1;         // BuildingAction 常量
        int maxLevel = 1;        // 1 = 单级；2 = 两级
        int level = 0;           // 已完成等级
        int pendingId = -1;      // 在研指令 id（-1 表示未在研）
        // 在研指令的下达帧。指令可能被内核去重丢掉（见下面 researchBuildingUsed
        // 的说明），所以必须做**超时重试**，否则这条科技会永远停在 pendingId 上。
        int pendingFrame = 0;
        // 这条科技最早可以在哪个阶段研发（phase：1 冲铜器/工具时代，2 铜器，3 反攻）。
        // 默认 2 = 等进铜器再研究；像"伐木加工"这种工具时代就解锁、收益又大的，
        // 设成 1 → 市场一建好就点（用户要求"先点出伐木科技"）。
        int minPhase = 2;
        int food = 0, wood = 0, stone = 0, gold = 0;      // 一级资源门槛
        int food2 = 0, wood2 = 0, stone2 = 0, gold2 = 0;  // 二级资源门槛
        // 硬截止科技（0 = 无要求）。到 urgentFromFrame 还没升完就插队：
        // 第一个下单（资源优先给它），并占住执行建筑（见 demand_army 里的靶场让位）。
        int deadlineFrame = 0;      // 必须升完的帧号
        int urgentFromFrame = 0;    // 从这一帧开始插队冲刺
        const char *name = "";
    };
    std::vector<ResearchState> researches;
    // 本帧已经下过研发单的建筑 SN。**同一帧同一座建筑只能下一条**：
    // 内核 Core::manageOrder 对同一个 self 只保留最后一条指令，前面被丢掉的那些
    // 永远不会回 ins_ret，对应的科技就永久卡在 pendingId 上。
    // 市场上有 5 条科技（伐木加工/驯养动物/车轮/石矿开采/金矿开采），不挡一下
    // 就会"只有最后一条能升，其余四条全卡死"。
    // 用 vector 而不是 set：本头文件的 include 区在 "DO NOT MODIFY" 标记之上，
    // 不想为了 <set> 去动它；这里元素最多两三个，线性查找完全够用。
    std::vector<int> researchBuildingUsed;
    void init_researches();
    void request_research(ResearchState &r);
    bool compositeBowReady();        // 复合弓科技是否已升完
    bool compositeBowUrgent();       // 是否已进入"必须尽快升完复合弓"的冲刺窗口
    bool rangeReservedForResearch(); // 靶场是否需要让位给科技（冲刺窗口且未升完）

    // ==================== 第三阶段：反攻（转化敌方武器工程厂取胜）====================
    int enemySiegeSN = -1;                        // 敌方武器工程厂 SN
    double enemySiegeDR = -1, enemySiegeUR = -1;  // 敌方武器工程厂细节坐标
    void demand_attack();                         // 反攻需求
    // 反攻阶段"上一次给每个单位下的目标"（单位 SN → 目标 SN；-1 = 正在推向敌营）。
    // 只在"目标变了"或"单位空闲（上一条指令已完成）"时才重下指令：
    // 每帧重下会被内核 suspendRelation 掉关系，"走过去 → 攻击/拆建筑"的蓄力阶段
    // 永远走不完（Core.cpp 里箭塔"每帧重下指令导致永远打不出伤害"就是这个坑）。
    std::unordered_map<int,int> attackOrderSN;
    int attackConvertSN = -1;                     // 祭司在反攻阶段正在转化的目标 SN

    // ==================== 行为树 ====================
public:
    enum class BTStatus { Success, Failure, Running };

    // 黑板：节点共享的上下文
    struct BTContext {
        tagInfo *info = nullptr;
        UsrAI   *ai   = nullptr;
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

    BTNodePtr btRoot;            // 行为树根节点（外部可替换任意子树）

private:
    BTContext btCtx;
    void build_behavior_tree();

    // ---- 行为树叶子行为 ----
    void bt_sync();              // 阶段推进 + 回收任务
    bool bt_enemy_at_home();     // 敌方是否已逼近我方城市（防御触发条件）
    bool enemy_near(double dr, double ur, double radius);   // 指定点半径内是否有可见敌军
    void demand_build();         // 建造需求（房屋 / 冲铜器链 / 学院 / 农田 / 箭塔）
    void demand_dropoff();       // 资源点太远时，就近补建谷仓/仓库
    double nearest_dropoff_dist(int resType, double dr, double ur);  // 最近的存放建筑距离
    bool hunt_dropoff_ready();   // 打猎前置：瞪羚附近是否已有可用存放建筑
    bool block_is_standable(int i, int j);   // 单格是否可站立（排除水/斜坡/水边/建筑/资源）
    void demand_produce();       // 生产需求（村民）
    void demand_gather();        // 采集需求（食物 / 木 / 石 / 金 / 打猎 / 农田）
    void demand_scout();         // 探路（祭司环形广度优先 / 侦察骑兵 DFS，见函数内注释）
    void bt_dispatch();          // 任务排序 + 派发

    // ---- 统计辅助 ----
    int  count_done(int type);
    int  active_build(int btype);
    int  active_gather(int rtype);
    int  gatherers_on(int resSN);   // 资源点 resSN 上已经派了几个采集村民
    // 资源点 resSN "最多能同时站几个人"（= 周围 8 格里可站立的格子数，
    // 上限 GATHER_PER_RESOURCE_MAX=3）。**返回 0 = 根本够不到**，不要派村民去。
    // 依据：内核要求采集者贴到目标 ~0.5 格内才能开工
    // （Core_CondiFunc.h 的 distance_AllowWork = 目标半宽 + 2*CRASHBOX_SINGLEOB），
    // 而树/矿/建筑在地图上都是障碍格——周围一个可站立格都没有的点
    // （密林深处、水里、被建筑围住），村民走过去也只会被卡住，
    // 内核判"行动无用"强制中断关系，村民变回 IDLE，看起来就是"砍不到"。
    int  res_stand_spots(int resSN);
    // 上面那个的每帧缓存（资源 SN → 可站格数）。按需计算：只有这一帧被问到过的
    // 资源点才算一次，下一帧自动清空（key = GameFrame）。
    std::unordered_map<int,int> resSpots;
    int resSpotsFrame = -1;
    // 该资源点是否离市镇中心超过 GATHER_MAX_DIST(100) 格——太远的一律不派人去采
    // （判定用块坐标的平方距离，整数运算；见 UsrAI.cpp 里的定义与理由）。
    bool res_too_far(int blockDR, int blockUR);
    int  pending_build_wood();      // 队列里还没建成的建造任务总共要花多少木头
    int  active_action(int btype, int action);
    bool has_resource(int rtype);
    bool center_free();

    // 建造占位图：标记已规划/已建成的建筑位置（>0 = 占用），避免重复选址
    int MAP[505][505] = {{0}};
    // ---- 被内核驳回过的建造位置（拉黑表）----
    // key = (x << 12) | y，value = 解禁帧号。
    // 为什么必须有：选址搜索是**确定性**的，同一个锚点反复搜索会一直挑中同一块
    // "看着能建、内核却判不行"的地（与其他物体重叠 / 高度差 / 位置不合适…），
    // 于是变成"每几秒下一单 → 被驳回 → 再下一单"的死循环：
    //   · 浪费村民：建造者被 on_build_task 锁在原地干等；
    //   · 更糟的是内核每次都要走 is_BuildingCanBuild（里面 new/delete 一个临时
    //     Building 对象、还刷 debug 文本），反复走这条路会拖慢甚至搞崩游戏
    //     ——用户反馈的"尝试建仓库多次但没成功、然后游戏崩溃"就是这个循环。
    // 拉黑时长只给十几秒：这类驳回很多是**临时**的（有单位/刚出生的动物站在那儿、
    // 锚点（瞪羚）刚走开），过一会儿那块地其实能建。
    std::unordered_map<int,int> badBuildSite;
    bool build_site_ok(int x, int y);        // 该位置是否还在拉黑期内
    void mark_build_site_bad(int x, int y);  // 拉黑一个位置（顺手清过期项）

    bool find_block(int x,int y,int dx,int dy);

};

/*##########YOUR CODE BEGINS HERE##########*/

/*##########YOUR CODE ENDS HERE##########*/
#endif // USRAI_H

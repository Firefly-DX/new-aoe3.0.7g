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

    enum TaskType  { TASK_GATHER, TASK_BUILD, TASK_PRODUCE, TASK_UPGRADE, TASK_ATTACK };
    enum TaskState { TASK_WAITING, TASK_ASSIGNED, TASK_RUNNING, TASK_DONE, TASK_FAILED };

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
    };

    std::vector<Task> taskQueue;
    int nextTaskId = 0;
    int phase = 0;                // 阶段状态机：1冲铜器 2发展军事 3反攻
    bool huntStarted = false;     // 打猎开关（人口/木头到位后锁存，开了一直开）
    bool berryPhase = true;       // 浆果阶段：城边那几丛采完就结束，之后不再采浆果
    bool berrySeen  = false;      // 是否已见到过城边的浆果丛（防止开局没探到就误判结束）
    int farmTarget = 0;           // 目标农田数（= 打算派去种田的人数，一人一格农田）

    void sort_tasks();
    void assign_tasks();
    void recycle_tasks();

    // 战斗模块：箭塔拉仇恨 + 祭司转化防御
    // 注意：只有当敌方逼近我方城市（enemy_at_home）时才启用，避免祭司探图途中被远处敌军误触发。
    void combat_tactic();
    int convertTargetSN = -1;        // 待转化的敌方单位 SN
    int convertStuckFrame = 0;                   // 上次检查"祭司是否卡在转化目标上"的帧
    double convertStuckDR = 0, convertStuckUR = 0;
    int towerFocusSN = -1;           // 箭塔集火目标 SN（仇恨标记，锁定后不切换）
    int scoutCheckFrame = 0;                     // 上次卡住检查的帧号
    double scoutCheckDR = -1, scoutCheckUR = -1; // 上次卡住检查时的祭司位置

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

    // ---- 探图时记录发现过的敌人位置 ----
    // 祭司遇到敌人不回家，而是记录敌人所在地并换个方向继续探索；
    // 回村只由时间条件决定（见 demand_scout 里的 timeUp）。
    struct EnemySpot { double dr = 0; double ur = 0; int frame = 0; };
    std::vector<EnemySpot> enemySpots;
    void record_enemy_spots();                           // 记录当前可见敌人的位置（去重合并）
    bool spot_near(double dr, double ur, double radius);  // 该点是否离已记录的敌点太近

    // ---- 祭司探图（前沿点 + 可控步长）----
    // 把"已探索陆地中紧邻未知区域的格子"当作前沿节点，每次选一个"未走过"的
    // 前沿格前进；只标记"真正要去"的那一格。
    // 注意：不能把目标周围一整片都标记成已访问——那样会在没实际走过的地方
    // 留下大量空洞，只能探出一条窄走廊（广度不足）。
    unsigned char scoutSeen[505][505] = {{0}};  // 已作为目标走过的前沿格
    unsigned char scoutFront[505][505] = {{0}}; // 本次重算出的前沿格标记
    double scoutHeadDR = 0, scoutHeadUR = 0;    // 当前探索方向（单位向量，用于惩罚走回头路）

    // ---- 新探路：以营地为圆心的环形广度优先 ----
    // 一圈一圈向外扫：每个环按角度均匀取路点，逐个走过去；一圈扫完则半径 +STEP。
    int ringRadius = 0;                        // 当前正在搜索的环半径（块）
    int ringIndex = 0;                         // 当前环上的路点下标
    bool next_ring_point(int &bx, int &by);    // 取下一个待访问的环上路点

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
        int food = 0, wood = 0, stone = 0, gold = 0;      // 一级资源门槛
        int food2 = 0, wood2 = 0, stone2 = 0, gold2 = 0;  // 二级资源门槛
        const char *name = "";
    };
    std::vector<ResearchState> researches;
    void init_researches();
    void request_research(ResearchState &r);

    // ==================== 第三阶段：反攻（转化敌方武器工程厂取胜）====================
    int enemySiegeSN = -1;                        // 敌方武器工程厂 SN
    double enemySiegeDR = -1, enemySiegeUR = -1;  // 敌方武器工程厂细节坐标
    void demand_attack();                         // 反攻需求

    // ==================== 行为树 ====================
public:
    enum class BTStatus { Success, Failure, Running };

    // 黑板：节点共享的上下文
    struct BTContext {
        tagInfo *info = nullptr;
        UsrAI   *ai   = nullptr;
        std::vector<const char*> trace;   // 本帧 tick 路径（调试）
        bool traceOn = false;
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
    // 装饰节点：取反
    struct BTInverter : BTNode {
        BTNodePtr child;
        BTStatus tick(BTContext &ctx) override;
    };
    // 叶子节点：cond（条件）与 action（动作），至少提供一个
    struct BTLeaf : BTNode {
        std::function<bool(BTContext&)> cond;     // 条件，可选
        std::function<bool(BTContext&)> action;   // 动作，可选
        BTStatus tick(BTContext &ctx) override;
    };

    BTNodePtr btRoot;            // 行为树根节点（外部可替换任意子树）
    bool btTraceEnabled = false; // 是否记录 tick 路径到调试输出

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
    void demand_scout();         // 探路（祭司环形广度优先探索）
    void demand_scout_frontier();// 【旧逻辑，保留但不使用】前沿点 + 步长选点
    void bt_dispatch();          // 任务排序 + 派发

    // ---- 统计辅助 ----
    int  count_done(int type);
    int  active_build(int btype);
    int  active_gather(int rtype);
    int  gatherers_on(int resSN);   // 资源点 resSN 上已经派了几个采集村民
    int  active_action(int btype, int action);
    bool has_resource(int rtype);
    bool center_free();

    // 建造占位图：标记已规划/已建成的建筑位置（>0 = 占用），避免重复选址
    int MAP[505][505] = {{0}};

    bool find_block(int x,int y,int dx,int dy);

};

/*##########YOUR CODE BEGINS HERE##########*/

/*##########YOUR CODE ENDS HERE##########*/
#endif // USRAI_H

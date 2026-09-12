#ifndef USRAI_H
#define USRAI_H

#include "ai.h"
#include <unordered_map>

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
    int phase = 0;                // 阶段状态机：0开局 1冲铜器 2军事航海 3决胜

    void produce_demands();
    void sort_tasks();
    void assign_tasks();
    void recycle_tasks();

    // 战斗模块：箭塔拉仇恨 + 祭司转化防御
    void combat_tactic();
    int convertTargetSN = -1;        // 待转化的敌方单位 SN
    int arrowTowerTarget = 2;        // 目标箭塔数量
    bool arrowTowerResearched = false;   // 箭塔科技是否已研发
    int arrowTowerResearchId = -1;   // 箭塔科技研发指令 id（-1 表示未在研）

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
    bool bt_has_enemy();         // 是否存在可见敌人
    void demand_build();         // 建造需求（房屋/冲铜器链/箭塔）
    void demand_produce();       // 生产需求（村民/兵）
    void demand_gather();        // 采集需求（食物/木/石/打猎）
    void bt_dispatch();          // 任务排序 + 派发

    // ---- 统计辅助 ----
    int  count_done(int type);
    int  active_build(int btype);
    int  active_gather(int rtype);
    int  active_action(int btype, int action);
    bool has_resource(int rtype);
    bool center_free();

    int MAP[505][505] = {{0}};
    int len = 4;

    unordered_map <int,int> farmer_task;
    unordered_map <int,int> farmer_task_type;
    unordered_map <int,int> farmer_resource;
    unordered_map <int,int> resource_farmer;
    unordered_map <int,int> farmer_building;
    
    int farmer_working_amount[4];
    // 0 == food,1 == wood,2 == stone,3 == gold; 

    void create_farmers();
    void collecting_bush();
    void collecting_tree();
    bool find_block(int x,int y,int dx,int dy);
    void walk_to(tagHuman &a,double &dx,double &dy);
    void collecting_resources();
    void workers_count();
    void check_and_clear();
    void build_building(int b);

    void run_timeline();

};

/*##########YOUR CODE BEGINS HERE##########*/

/*##########YOUR CODE ENDS HERE##########*/
#endif // USRAI_H

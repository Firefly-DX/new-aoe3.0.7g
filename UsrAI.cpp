#include "UsrAI.h"
#include <set>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <iostream>
#include <unordered_map>
#include <list>
#include <cstdlib>

using namespace std;

#define TASK_RESOURCE 1
#define TASK_BUILDING 2



tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/
tagInfo info;

// 建筑占地尺寸（块）：房屋/箭塔 2x2，其余 3x3
static int building_size(int type) {
    return (type == BUILDING_HOME || type == BUILDING_ARROWTOWER) ? 2 : 3;
}

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

// 生产与采集模块

void UsrAI::create_farmers(){
    int meat = info.Meat;

    tagBuilding *idlebuilding = nullptr;

    for (tagBuilding &building : info.buildings){
        if (building.Type == BUILDING_CENTER){
            idlebuilding = &building;
            break;
        }
    }

    if (idlebuilding != nullptr){
        if (meat >= BUILDING_CENTER_CREATEFARMER_FOOD && idlebuilding->Project == 0){
            BuildingAction(idlebuilding->SN, BUILDING_CENTER_CREATEFARMER);
        }
    }
}

void UsrAI::collecting_resources(){
    int total_num = info.farmers.size();

    for (tagFarmer &farmer : info.farmers){
        if (farmer.NowState == HUMAN_STATE_IDLE){
            int id;
            if (farmer_working_amount[0] < total_num * 6 / 10){
                id = RESOURCE_BUSH;
                farmer_working_amount[0] ++;
            } else if (farmer_working_amount[1] < total_num * 2 / 10){
                id = RESOURCE_TREE;
                farmer_working_amount[1] ++;
            } else if (farmer_working_amount[2] < total_num / 10){
                id = RESOURCE_STONE;
                farmer_working_amount[2] ++;
            }

            for (tagResource &resource : info.resources){
                if (resource.Type == id && !(resource_farmer.count(resource.SN))){
                    HumanAction(farmer.SN,resource.SN);
                    farmer_task.insert({farmer.SN,id});
                    farmer_resource.insert({farmer.SN,resource.SN});
                    resource_farmer.insert({resource.SN,farmer.SN});
                    break;
                }
            }
        }
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

void UsrAI::walk_to(tagHuman &a,double &dx,double &dy){

    double targetDR = a.DR + dx,targetUR = a.UR + dy;
    int target_block_DR = int(targetDR / BLOCKSIDELENGTH);
    int target_block_UR = int(targetUR / BLOCKSIDELENGTH);

    tagTerrain field = (*info.theMap)[target_block_DR][target_block_UR];
    if (field.type == MAPPATTERN_GRASS){
        HumanMove(a.SN,targetDR,targetUR);
    }
}

void UsrAI::workers_count(){
    for (int i = 0;i < 4;i ++){
        farmer_working_amount[i] = 0;
    }
    for (tagFarmer &farmer : info.farmers){
        int task = farmer_task[farmer.SN];
        if (task == RESOURCE_BUSH){
            farmer_working_amount[0] ++;
        } else if (task == RESOURCE_TREE){
            farmer_working_amount[1] ++;
        } else if (task == RESOURCE_STONE){
            farmer_working_amount[2] ++;
        } else if (task == RESOURCE_GOLD){
            farmer_working_amount[3] ++;
        } 
    }
}




// 建筑建造模块

void UsrAI::build_building(int b){
    int x,y;
    for (tagBuilding &building : info.buildings){
        if (building.Type == BUILDING_CENTER){
            x = building.BlockDR;
            y = building.BlockUR;
            break;
        }
    }

    int f = 0;
    while (f == 0){
        for (int i = -len;i <= len;i ++){
            for (int j = -len;j <= len;j ++){
                if (find_block(x + i,y + j,2,2)){
                    x += i;y += j;
                    f = 1;
                    break;
                }
            }
            if (f == 1){
                break;
            }
        }
        if (f == 0){
            len += 4;
        } else {
            break;
        }
    }
    
    for (tagFarmer &farmer : info.farmers){
        if (farmer.NowState == HUMAN_STATE_IDLE){
            HumanBuild(farmer.SN,b,x,y);
            for (int i = x;i < x + 3;i ++){
                for (int j = y;j < y + 3;j ++){
                    MAP[i][j] = b;
                }
            }
            farmer_task.insert({farmer.SN,b});
            farmer_task_type.insert({farmer.SN,TASK_BUILDING});
            farmer_resource.insert({farmer.SN,0});
            break;
        }
    }
}

// 战斗模块


// 时间线与其他零碎功能

void UsrAI::check_and_clear(){
    for (tagFarmer &farmer : info.farmers){
        if (farmer.NowState == HUMAN_STATE_IDLE){
            resource_farmer.erase(farmer_resource[farmer.SN]);
            farmer_resource.erase(farmer.SN);
            farmer_task.erase(farmer.SN);
        }
    }
}

void UsrAI::run_timeline(){
    int frame = info.GameFrame;
    if (frame < 6000){
        collecting_resources();

        if (info.Meat >= BUILDING_CENTER_UPGRADE_BRONZEAGE_FOOD){
            int num = 0;
            tagBuilding* idlebuilding = nullptr;
            for (tagBuilding &building : info.buildings){
                if (building.Type == BUILDING_MARKET){
                    num ++;
                } else if (building.Type == BUILDING_RANGE){
                    num ++;
                } else if (building.Type == BUILDING_STABLE){
                    num ++;
                } else if (building.Type == BUILDING_CENTER){
                    idlebuilding = &building;
                }
            }
            if (num >= 2){
                BuildingAction(idlebuilding->SN,BUILDING_CENTER_UPGRADE);
            }
        } else {
            create_farmers();
        }

    } else if (frame < 13500){

    } else if (frame < 21000){

    }
}

// ==================== 任务系统 ====================
// 字段复用约定：PRODUCE/UPGRADE 任务中
//   buildingType = 执行动作的建筑类型；targetSN = 要执行的 Action 编号。

// ---------- 同步：阶段推进 + 回收任务 ----------
void UsrAI::bt_sync()
{
    if (info.civilizationStage >= CIVILIZATION_BRONZEAGE)
        phase = 2;
    else
        phase = 1;   // 开局即工具时代，直接冲铜器

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

bool UsrAI::has_resource(int rtype)
{
    for (tagResource &r : info.resources)
        if (r.Type == rtype && r.Cnt > 0) return true;
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
    // ---- 补房屋（人口快满时）----
    if (info.Human_MaxNum - info.Human_Num <= 2
        && info.Wood >= BUILD_HOUSE_WOOD
        && active_build(BUILDING_HOME) == 0) {
        Task t;
        t.id = nextTaskId++;
        t.type = TASK_BUILD;
        t.priority = 1;
        t.buildingType = BUILDING_HOME;
        taskQueue.push_back(t);
    }

    // ---- 冲铜器建筑链：谷仓 → 市场 → 兵营 → 马厩 ----
    if (phase < 2) {
        int p = 2;
        if (count_done(BUILDING_GRANARY) == 0 && active_build(BUILDING_GRANARY) == 0
            && info.Wood >= BUILD_GRANARY_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = p;
            t.buildingType = BUILDING_GRANARY;
            taskQueue.push_back(t);
        } else if (count_done(BUILDING_MARKET) == 0 && active_build(BUILDING_MARKET) == 0
            && info.Wood >= BUILD_MARKET_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = p;
            t.buildingType = BUILDING_MARKET;
            taskQueue.push_back(t);
        } else if (count_done(BUILDING_ARMYCAMP) == 0 && active_build(BUILDING_ARMYCAMP) == 0
            && info.Wood >= BUILD_ARMYCAMP_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = p;
            t.buildingType = BUILDING_ARMYCAMP;
            taskQueue.push_back(t);
        } else if (count_done(BUILDING_STABLE) == 0 && active_build(BUILDING_STABLE) == 0
            && info.Wood >= BUILD_STABLE_WOOD) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_BUILD; t.priority = p;
            t.buildingType = BUILDING_STABLE;
            taskQueue.push_back(t);
        }
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

}

// ---------- 生产需求 ----------
void UsrAI::demand_produce()
{
    int farmerNum = 0;
    for (tagFarmer &f : info.farmers)
        if (f.FarmerSort == FARMERTYPE_FARMER) farmerNum++;

    // ---- 造村民 ----
    if (phase < 2 && farmerNum < 20
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

    // ---- 造兵：兵营训练棍棒兵（第一阶段防御）----
    {
        int clubmanNum = 0;
        for (tagArmy &a : info.armies)
            if (a.Sort == AT_CLUBMAN) clubmanNum++;

        bool campFree = false;
        for (tagBuilding &b : info.buildings)
            if (b.Type == BUILDING_ARMYCAMP && b.Percent >= 100 && b.Project == 0)
                campFree = true;

        if (campFree && clubmanNum < 2
            && info.Human_Num + 1 <= info.Human_MaxNum
            && info.Meat >= BUILDING_ARMYCAMP_CREATE_CLUBMAN_FOOD
            && active_action(BUILDING_ARMYCAMP, BUILDING_ARMYCAMP_CREATE_CLUBMAN) == 0) {
            Task t;
            t.id = nextTaskId++;
            t.type = TASK_PRODUCE;
            t.priority = 0;
            t.buildingType = BUILDING_ARMYCAMP;
            t.targetSN = BUILDING_ARMYCAMP_CREATE_CLUBMAN;
            taskQueue.push_back(t);
        }
    }
}

// ---------- 采集需求 ----------
void UsrAI::demand_gather()
{
    int farmerNum = 0;
    for (tagFarmer &f : info.farmers)
        if (f.FarmerSort == FARMERTYPE_FARMER) farmerNum++;

    // ---- 采集需求（食物 + 木头 + 石头按需分配）----
    int total = farmerNum > 0 ? farmerNum : 1;

    // 箭塔未建够且石头不足时，分配村民采石（约 20%，至少 1 人）
    int towerCnt = 0;
    for (tagBuilding &b : info.buildings)
        if (b.Type == BUILDING_ARROWTOWER) towerCnt++;
    bool needStone = (towerCnt < arrowTowerTarget) && (info.Stone < BUILD_ARROWTOWER_STONE);

    int wantStone = needStone ? (total / 5) : 0;
    if (wantStone < 1 && needStone) wantStone = 1;

    int rest = total - wantStone; if (rest < 1) rest = 1;
    int wantWood = rest * 4 / 10; if (wantWood < 1) wantWood = 1;
    int wantFood = rest - wantWood;   // 食物拿大头 + 取整余数

    // 浆果丛有限：先分配村民采浆果，多余的村民去打猎（瞪羚）
    int bushCnt = 0;
    for (tagResource &r : info.resources)
        if (r.Type == RESOURCE_BUSH && r.Cnt > 0) bushCnt++;
    int wantBush = wantFood; if (wantBush > bushCnt) wantBush = bushCnt;
    int wantHunt = wantFood - wantBush;

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
    if (has_resource(RESOURCE_TREE)) {
        while (active_gather(RESOURCE_TREE) < wantWood) {
            Task t;
            t.id = nextTaskId++; t.type = TASK_GATHER; t.priority = 3;
            t.resourceType = RESOURCE_TREE;
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
}

// ---------- 派发：排序 + 派发 ----------
void UsrAI::bt_dispatch()
{
    sort_tasks();
    assign_tasks();
}

// ---------- 兼容包装（行为树不再使用，保留供旧调用）----------
void UsrAI::produce_demands()
{
    demand_build();
    demand_produce();
    demand_gather();
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
    for (Task &t : taskQueue)
        if (t.type == TASK_GATHER && t.targetSN != -1
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
            for (tagResource &r : info.resources) {
                if (r.Type != t.resourceType) continue;
                // 活动物（Cnt=0 但 Blood>0）也允许选中，用于打猎；尸体/普通资源看 Cnt
                if (r.Cnt <= 0 && r.Blood <= 0) continue;
                if (lockedRes.count(r.SN)) continue;
                double d = calDistance(f->DR, f->UR, r.DR, r.UR);
                if (d < best) { best = d; resSN = r.SN; }
            }
            if (resSN == -1) continue;   // 暂无可用资源，保持等待

            HumanAction(f->SN, resSN);
            t.farmerSN = f->SN;
            t.targetSN = resSN;
            t.state = TASK_ASSIGNED;
            t.startFrame = info.GameFrame;
            assignedThisFrame.insert(f->SN);
            lockedRes.insert(resSN);
        }
        else if (t.type == TASK_BUILD) {
            tagFarmer *f = find_idle();
            if (f == nullptr) continue;

            int size = building_size(t.buildingType);

            // 锚点：房屋优先挨着已有房屋（聚成居住区），其他建筑以市镇中心为中心
            int ax = -1, ay = -1;
            if (t.buildingType == BUILDING_HOME) {
                for (tagBuilding &b : info.buildings) {
                    if (b.Type == BUILDING_HOME) { ax = b.BlockDR; ay = b.BlockUR; break; }
                }
            }
            if (ax == -1) {
                for (tagBuilding &b : info.buildings) {
                    if (b.Type == BUILDING_CENTER) { ax = b.BlockDR; ay = b.BlockUR; break; }
                }
            }
            if (ax == -1) continue;   // 无锚点（异常）

            int x = -1, y = -1;
            // 以锚点为中心，半径 4 一圈一圈向外扩（环形搜索）
            for (int r = 4; r <= 48 && x == -1; r += 4) {
                for (int i = -r; i <= r && x == -1; i++) {
                    for (int j = -r; j <= r && x == -1; j++) {
                        int di = i < 0 ? -i : i;
                        int dj = j < 0 ? -j : j;
                        if (di != r && dj != r) continue;   // 只取本圈环上的点
                        if (find_block(ax + i, ay + j, size, size)) {
                            x = ax + i;
                            y = ay + j;
                        }
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
            }
            bool farmerIdle = false;
            if (t.farmerSN != -1) {
                for (tagFarmer &f : info.farmers)
                    if (f.SN == t.farmerSN) {
                        farmerIdle = (f.NowState == HUMAN_STATE_IDLE);
                        break;
                    }
            }
            if (targetGone || farmerIdle)
                t.state = TASK_DONE;
        }
        else if (t.type == TASK_BUILD) {
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
void UsrAI::combat_tactic()
{
    // 收集可见敌人（军队优先，其次农民）
    std::vector<int> enemies;
    for (tagArmy &e : info.enemy_armies) enemies.push_back(e.SN);
    for (tagFarmer &f : info.enemy_farmers) enemies.push_back(f.SN);
    if (enemies.empty()) return;

    // 找己方祭司
    tagArmy *priest = nullptr;
    for (tagArmy &a : info.armies)
        if (a.Sort == AT_PRIEST) { priest = &a; break; }

    // 1) 箭塔每帧索敌攻击（拉仇恨），把敌人从祭司身边引开
    int idx = 0;
    for (tagBuilding &tower : info.buildings) {
        if (tower.Type != BUILDING_ARROWTOWER) continue;
        if (tower.Percent < 100) continue;
        HumanAction(tower.SN, enemies[idx % (int)enemies.size()]);
        idx++;
    }

    // 2) 祭司转化敌人（优先转化正在攻击祭司的敌人）
    if (priest != nullptr && priest->ConvertCooldown == 0) {
        // 清理：若上次转化目标已不在敌方列表（转化成功或死亡），重置
        if (convertTargetSN != -1) {
            bool stillEnemy = false;
            for (int sn : enemies)
                if (sn == convertTargetSN) { stillEnemy = true; break; }
            if (!stillEnemy) convertTargetSN = -1;
        }

        // 选目标：优先正在攻击祭司的敌人，否则第一个
        int target = enemies[0];
        bool foundAttacker = false;
        for (int sn : enemies) {
            for (tagArmy &e : info.enemy_armies) {
                if (e.SN == sn && e.WorkObjectSN == priest->SN) {
                    target = sn;
                    foundAttacker = true;
                    break;
                }
            }
            if (foundAttacker) break;
        }

        if (convertTargetSN == -1) {
            convertTargetSN = target;
            HumanAction(priest->SN, target);
        }
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
bool UsrAI::bt_has_enemy()
{
    return !info.enemy_armies.empty() || !info.enemy_farmers.empty();
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
    //   defense  : Selector —— 有敌人则「箭塔拉仇恨 + 祭司转化」，否则跳过
    //   build    : 建造需求（房屋 / 冲铜器链 / 箭塔）
    //   produce  : 生产需求（村民 / 兵）
    //   gather   : 采集需求（食物 / 木 / 石 / 打猎）
    //   dispatch : 任务排序 + 派发
    // )
    btRoot = seq({
        leaf("sync", nullptr,
             [](BTContext &c) { c.ai->bt_sync(); return true; }),

        sel({
            seq({
                leaf("has_enemy", [](BTContext &c) { return c.ai->bt_has_enemy(); }, nullptr),
                leaf("defense",   nullptr, [](BTContext &c) { c.ai->combat_tactic(); return true; })
            }),
            leaf("no_enemy", nullptr, [](BTContext &) { return true; })
        }),

        leaf("build",    nullptr, [](BTContext &c) { c.ai->demand_build();   return true; }),
        leaf("produce",  nullptr, [](BTContext &c) { c.ai->demand_produce(); return true; }),
        leaf("gather",   nullptr, [](BTContext &c) { c.ai->demand_gather();  return true; }),
        leaf("dispatch", nullptr, [](BTContext &c) { c.ai->bt_dispatch();    return true; })
    });
    btRoot->btName = "root";
}

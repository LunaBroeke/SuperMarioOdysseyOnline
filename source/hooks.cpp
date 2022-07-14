#include <stdint.h>
#include <sys/types.h>
#include "rs/util.hpp"
#include "server/Client.hpp"
#include "al/LiveActor/LiveActor.h"
#include "al/actor/ActorInitInfo.h"
#include "al/actor/Placement.h"
#include "al/byaml/ByamlIter.h"
#include "al/nerve/Nerve.h"
#include "al/nerve/NerveExecutor.h"
#include "al/nerve/NerveKeeper.h"
#include "al/util.hpp"
#include "al/util/ControllerUtil.h"
#include "al/util/LiveActorUtil.h"
#include "al/util/NerveUtil.h"
#include "game/Actors/WorldEndBorderKeeper.h"
#include "game/Layouts/CoinCounter.h"
#include "game/Player/Actions/PlayerActionGroundMoveControl.h"
#include "game/Player/PlayerActorHakoniwa.h"
#include "game/Player/PlayerConst.h"
#include "game/Player/States/PlayerStateRunHakoniwa.h"
#include "game/StageScene/StageSceneStateOption.h"
#include "game/StageScene/StageSceneStatePauseMenu.h"
#include "game/StageScene/StageSceneStateServerConfig.hpp"
#include "game/StageScene/StageSceneStateTrailColor.hpp"
#include "logger.hpp"
#include "main.hpp"
#include "al/byaml/writer/ByamlWriter.h"
#include "math/seadVector.h"
#include "rs/util/InputUtil.h"
#include "sead/prim/seadSafeString.h"
#include "server/HideAndSeekMode.hpp"
#include "timeWarp.h"

bool comboBtnHook(int port) {
    if (Client::isModeActive()) { // only switch to combo if any gamemode is active
        return !al::isPadHoldL(port) && al::isPadTriggerDown(port);
    } else {
        return al::isPadTriggerDown(port);
    }
}

void saveWriteHook(al::ByamlWriter* saveByml) {

    const char *serverIP = Client::getCurrentIP();
    const int serverPort = Client::getCurrentPort();
    const uint trailColor = getTimeContainer().getPatternNum();
    const uint keybindId = getTimeContainer().getKeybindId();

    if (serverIP) saveByml->addString("ServerIP", serverIP);
    else saveByml->addString("ServerIP", "0.0.0.0");

    if (serverPort) saveByml->addInt("ServerPort", serverPort);
    else saveByml->addInt("ServerPort", 0);

    if (trailColor) saveByml->addUInt("TrailColor", trailColor);
    else saveByml->addUInt("TrailColor", 0);

    if (keybindId) saveByml->addUInt("RewindKeybindId", keybindId);
    else saveByml->addUInt("RewindKeybindId", 0);

    saveByml->pop();
}

bool saveReadHook(int* padRumbleInt, al::ByamlIter const& saveByml, char const* padRumbleKey) {

    const char *serverIP = "";
    int serverPort = 0;
    uint trailColor = 0;
    uint keybindId = 0;

    if (al::tryGetByamlString(&serverIP, saveByml, "ServerIP")) {
        Client::setLastUsedIP(serverIP);
    }

    if (al::tryGetByamlS32(&serverPort, saveByml, "ServerPort")) {
        Client::setLastUsedPort(serverPort);
    }

    if (al::tryGetByamlU32(&trailColor, saveByml, "TrailColor")) {
        getTimeContainer().setCurrentColorPattern(trailColor);
    }

    if (al::tryGetByamlU32(&keybindId, saveByml, "RewindKeybindId")) {
        getTimeContainer().setControlBinding(keybindId);
    }
    
    return al::tryGetByamlS32(padRumbleInt, saveByml, padRumbleKey);
}

bool registerShineToList(Shine* shineActor) {

    if (shineActor->shineId >= 0) {
        Client::tryRegisterShine(shineActor);
    } 

    return al::isAlive(shineActor);
}

void overrideNerveHook(StageSceneStatePauseMenu* thisPtr, al::Nerve* nrvSet) {

    if (al::isPadHoldZL(-1)) {
        al::setNerve(thisPtr, &nrvStageSceneStatePauseMenuServerConfig);
    } else if (al::isPadHoldZR(-1)){
        al::setNerve(thisPtr, &nrvStageSceneStatePauseMenuTrailColor);
    } else {
        al::setNerve(thisPtr, nrvSet);
    }
}

StageSceneStateServerConfig *sceneStateServerConfig = nullptr;
StageSceneStateTrailColor *sceneStateTrailColor = nullptr;

void initStateHook(StageSceneStatePauseMenu *thisPtr, char const *stateName, al::Scene *host, al::LayoutInitInfo const &initInfo, FooterParts *footer,
                   GameDataHolder *data, bool unkBool) {
    thisPtr->mStateOption =
        new StageSceneStateOption(stateName, host, initInfo, footer, data, unkBool);

    sceneStateServerConfig = new StageSceneStateServerConfig("ServerConfig", host, initInfo, footer, data, unkBool);
    sceneStateTrailColor = new StageSceneStateTrailColor("TrailColor", host, initInfo, footer, data, unkBool);
}

void initNerveStateHook(StageSceneStatePauseMenu* stateParent, StageSceneStateOption* stateOption,
                        al::Nerve const* executingNerve, char const* stateName) {

    al::initNerveState(stateParent, stateOption, executingNerve, stateName);

    al::initNerveState(stateParent, sceneStateServerConfig, &nrvStageSceneStatePauseMenuServerConfig, "CustomNerveOverride");
    al::initNerveState(stateParent, sceneStateTrailColor, &nrvStageSceneStatePauseMenuTrailColor, "TrailColorNerve");
}

// skips starting both coin counters
void startCounterHook(CoinCounter* thisPtr) {
    if (!Client::isModeActive()) {
        thisPtr->tryStart();
    }
}

// Simple hook that can be used to override isModeE3 checks to enable/disable certain behaviors
bool modeE3Hook() {
    return Client::isModeActive();
}

// Skips ending the play guide layout if a mode is active, since the mode would have already ended it
void playGuideEndHook(al::SimpleLayoutAppearWaitEnd* thisPtr) {
    if (!Client::isModeActive()) {
        thisPtr->end();
    }
}

// Gravity Hooks

void initHackCapHook(al::LiveActor *cappy) {
    al::initActorPoseTQGSV(cappy);
}

al::PlayerHolder* createTicketHook(StageScene* curScene) {
    // only creates custom gravity camera ticket if hide and seek mode is active
    if (Client::isSelectedMode(GameMode::HIDEANDSEEK)) {
        al::CameraDirector* director = curScene->getCameraDirector();
        if (director) {
            if (director->mFactory) {
                al::CameraTicket* gravityCamera = director->createCameraFromFactory(
                    "CameraPoserCustom", nullptr, 0, 5, sead::Matrix34f::ident);

                HideAndSeekMode* mode = Client::getMode<HideAndSeekMode>();

                mode->setCameraTicket(gravityCamera);
            }
        }
    }

    return al::getScenePlayerHolder(curScene);
}

bool borderPullBackHook(WorldEndBorderKeeper* thisPtr) {

    bool isFirstStep = al::isFirstStep(thisPtr);

    if (isFirstStep) {
        if (Client::isSelectedMode(GameMode::HIDEANDSEEK) && Client::isModeActive()) {

            HideAndSeekMode* mode = Client::getMode<HideAndSeekMode>();

            if (mode->isUseGravity()) {
                killMainPlayer(thisPtr->mActor);
            }
        }
    }
    
    return isFirstStep;
}

bool triggerR(int port)
{
    return getTimeContainer().isKeybindBumperR() ? false : al::isPadTriggerR(port);
}

bool triggerL(int port)
{
    return getTimeContainer().isKeybindBumperL() ? false : al::isPadTriggerL(port);
}

bool reduceOxygenForce()
{
    TimeContainer& container = getTimeContainer();
    StageScene* stageScene = container.stageSceneRef;
    if(!stageScene) return false;

    container.isPInWater = rs::isPlayerInWater(rs::getPlayerActor(stageScene));

    //Perform usual check for if player is in water, but if not, force it on if cooldown is active
    if(container.isPInWater) return true;
    else return getTimeContainer().isOnCooldown();
}

void oxygenReduce(PlayerOxygen* thisPtr)
{
    TimeContainer& container = getTimeContainer();
    container.oxygen = thisPtr;
    float oxygenRingCalc;
    
    //If the player is in water, perform usual calculation
    if(container.isPInWater){
        thisPtr->mOxygenFrames++;
        if (thisPtr->mOxygenFrames >= thisPtr->mOxygenTarget) { thisPtr->mDamageFrames++; }

        oxygenRingCalc = 1.f - (static_cast<float>(thisPtr->mOxygenFrames) / static_cast<float>(thisPtr->mOxygenTarget));
        if (oxygenRingCalc <= 0.f) oxygenRingCalc = 0.f;
    }

    //If the cooldown is running, replace the value in the ring
    if(container.isOnCooldown()){
        oxygenRingCalc = container.calcCooldownPercent();
        if(thisPtr->mOxygenFrames == 0) thisPtr->mOxygenFrames = thisPtr->mPercentageDelay;
        if(thisPtr->mOxygenFrames >= thisPtr->mOxygenTarget) thisPtr->mOxygenFrames = thisPtr->mOxygenTarget-1;
    }

    thisPtr->mPercentage = oxygenRingCalc;
    return;
}
#include "server/freeze/FreezeTagMode.hpp"
#include <cmath>
#include "actors/PuppetActor.h"
#include "al/async/FunctorV0M.hpp"
#include "al/util.hpp"
#include "al/util/CameraUtil.h"
#include "al/util/ControllerUtil.h"
#include "al/util/LiveActorUtil.h"
#include "al/util/NerveUtil.h"
#include "cameras/CameraPoserActorSpectate.h"
#include "game/GameData/GameDataHolderAccessor.h"
#include "game/Layouts/CoinCounter.h"
#include "game/Layouts/MapMini.h"
#include "game/Player/HackCap.h"
#include "game/Player/PlayerActorBase.h"
#include "game/Player/PlayerActorHakoniwa.h"
#include "heap/seadHeapMgr.h"
#include "layouts/FreezeTagIcon.h"
#include "logger.hpp"
#include "puppets/PuppetInfo.h"
#include "rs/util.hpp"
#include "server/freeze/FreezeTagScore.hpp"
#include "server/gamemode/GameModeBase.hpp"
#include "server/Client.hpp"
#include "server/gamemode/GameModeTimer.hpp"
#include <heap/seadHeap.h>
#include "server/gamemode/GameModeManager.hpp"
#include "server/gamemode/GameModeFactory.hpp"
#include "rs/util/InputUtil.h"

#include "basis/seadNew.h"
#include "server/freeze/FreezeTagConfigMenu.hpp"

FreezeTagMode::FreezeTagMode(const char* name) : GameModeBase(name) {}

void FreezeTagMode::init(const GameModeInitInfo& info) {
    mSceneObjHolder = info.mSceneObjHolder;
    mMode = info.mMode;
    mCurScene = (StageScene*)info.mScene;
    mPuppetHolder = info.mPuppetHolder;

    GameModeInfoBase* curGameInfo = GameModeManager::instance()->getInfo<HideAndSeekInfo>();

    if (curGameInfo) Logger::log("Gamemode info found: %s %s\n", GameModeFactory::getModeString(curGameInfo->mMode), GameModeFactory::getModeString(info.mMode));
    else Logger::log("No gamemode info found\n");
    if (curGameInfo && curGameInfo->mMode == mMode) {
        mInfo = (FreezeTagInfo*)curGameInfo;
    } else {
        if (curGameInfo) delete curGameInfo;  // attempt to destory previous info before creating new one
        mInfo = GameModeManager::instance()->createModeInfo<FreezeTagInfo>();
    }

    mInfo->mRunnerPlayers.allocBuffer(0x10, al::getSceneHeap());
    mInfo->mChaserPlayers.allocBuffer(0x10, al::getSceneHeap());

    Logger::log("Scene Heap Free Size: %f/%f\n", al::getSceneHeap()->getFreeSize() * 0.001f, al::getSceneHeap()->getSize() * 0.001f);

    mModeLayout = new FreezeTagIcon("FreezeTagIcon", *info.mLayoutInitInfo);
    mInfo->mPlayerTagScore.setTargetLayout(mModeLayout);
    
    Logger::log("Scene Heap Free Size: %f/%f\n", al::getSceneHeap()->getFreeSize() * 0.001f, al::getSceneHeap()->getSize() * 0.001f);

    //Create main player's ice block
    mMainPlayerIceBlock = new FreezePlayerBlock("MainPlayerBlock");
    mMainPlayerIceBlock->init(*info.mActorInitInfo);
}

void FreezeTagMode::begin() {
    mModeLayout->appear();

    mIsFirstFrame = true;

    MapMini* compass = mCurScene->mSceneLayout->mMapMiniLyt;
    al::SimpleLayoutAppearWaitEnd* playGuideLyt = mCurScene->mSceneLayout->mPlayGuideMenuLyt;

    mInvulnTime = 0.f;
    mSpectateIndex = -1;
    mPrevSpectateIndex = -2;

    if (compass->mIsAlive)
        compass->end();
    if (playGuideLyt->mIsAlive)
        playGuideLyt->end();

    mCurScene->mSceneLayout->end();

    //Update other players on your freeze tag state when starting
    Client::sendFreezeInfPacket();

    GameModeBase::begin();
}

void FreezeTagMode::end() {

    mModeLayout->tryEnd();

    MapMini* compass = mCurScene->mSceneLayout->mMapMiniLyt;
    al::SimpleLayoutAppearWaitEnd* playGuideLyt = mCurScene->mSceneLayout->mPlayGuideMenuLyt;

    mInvulnTime = 0.f;

    if (!compass->mIsAlive)
        compass->appearSlideIn();
    if (!playGuideLyt->mIsAlive)
        playGuideLyt->appear();
    
    mCurScene->mSceneLayout->start();
    
    if(!GameModeManager::instance()->isPaused()) {
        if(mInfo->mIsPlayerFreeze)
            trySetPlayerRunnerState(FreezeState::ALIVE);
        
        if(mTicket->mIsActive)
            al::endCamera(mCurScene, mTicket, 0, false);
        
        if(al::isAlive(mMainPlayerIceBlock) && !al::isNerve(mMainPlayerIceBlock, &nrvFreezePlayerBlockDisappear)) {
            mMainPlayerIceBlock->end();
            al::invalidatePostProcessingFilter(mCurScene);
        }
    }

    GameModeBase::end();
}

void FreezeTagMode::update() {
    PlayerActorBase* playerBase = rs::getPlayerActor(mCurScene);
    bool isYukimaru = !playerBase->getPlayerInfo(); // if PlayerInfo is a nullptr, that means we're dealing with the bound bowl racer

    // First frame stuff, remove this?
    if (mIsFirstFrame) {
        mIsFirstFrame = false;
    }

    //Verify standard hud is hidden
    if(!mCurScene->mSceneLayout->isEnd())
        mCurScene->mSceneLayout->end();

    //Main player's ice block state and post processing
    if(mInfo->mIsPlayerFreeze) {
        if(!al::isAlive(mMainPlayerIceBlock)) {
            mMainPlayerIceBlock->appear();
            al::validatePostProcessingFilter(mCurScene);
            int effectIndex = al::getPostProcessingFilterPresetId(mCurScene);
            while(effectIndex != 1) {
                al::incrementPostProcessingFilterPreset(mCurScene);
                effectIndex = (effectIndex + 1) % 18;
            }
        }
        
        //Lock block onto player
        al::setTrans(mMainPlayerIceBlock, al::getTrans(playerBase));

    } else {
        if(al::isAlive(mMainPlayerIceBlock) && !al::isNerve(mMainPlayerIceBlock, &nrvFreezePlayerBlockDisappear)) {
            mMainPlayerIceBlock->end();
            al::invalidatePostProcessingFilter(mCurScene);
        }
    }

    //Create list of runner and chaser player indexs
    mInfo->mRunnerPlayers.clear();
    mInfo->mChaserPlayers.clear();

    for(int i = 0; i < mPuppetHolder->getSize(); i++) {
        PuppetInfo *curInfo = Client::getPuppetInfo(i);
        if(!curInfo->isConnected)
            continue;
            
        if(curInfo->isFreezeTagRunner)
            mInfo->mRunnerPlayers.pushBack(curInfo);
        else
            mInfo->mChaserPlayers.pushBack(curInfo);
    }

    //Verify you are never frozen on chaser team
    if(!mInfo->mIsPlayerRunner && mInfo->mIsPlayerFreeze)
        trySetPlayerRunnerState(FreezeState::ALIVE);
    
    mInvulnTime += Time::deltaTime;

    // Runner team frame checks
    if (mInfo->mIsPlayerRunner) {
        if (mInvulnTime >= 3 && !isYukimaru) {
            bool isPDead = PlayerFunction::isPlayerDeadStatus(playerBase);
            bool isP2D = ((PlayerActorHakoniwa*)playerBase)->mDimKeeper->is2D;

            for (size_t i = 0; i < mPuppetHolder->getSize(); i++) {
                PuppetInfo *curInfo = Client::getPuppetInfo(i);
                float pupDist = al::calcDistance(playerBase, curInfo->playerPos);

                if(!curInfo->isConnected)
                    continue;

                //Check for freeze
                if (!mInfo->mIsPlayerFreeze && pupDist < 200.f && isP2D == curInfo->is2D && !isPDead && !curInfo->isFreezeTagRunner)
                    trySetPlayerRunnerState(FreezeState::FREEZE);

                //Check for unfreeze
                if (mInvulnTime >= 3.75f && mInfo->mIsPlayerFreeze && pupDist < 150.f && isP2D == curInfo->is2D
                && !isPDead && curInfo->isFreezeTagRunner && !curInfo->isFreezeTagFreeze) {
                    trySetPlayerRunnerState(FreezeState::ALIVE);
                }
            }
        }
    }

    //Change teams
    if (al::isPadTriggerRight(-1) && !al::isPadHoldZL(-1) && !al::isPadHoldX(-1) && !al::isPadHoldY(-1) && !mInfo->mIsPlayerFreeze && mRecoveryEventFrames == 0) {
        mInfo->mIsPlayerRunner = !mInfo->mIsPlayerRunner;
        mInvulnTime = 0.f;

        Client::sendFreezeInfPacket();
    }

    //Update recovery event timer
    if(mRecoveryEventFrames > 0) {
        mRecoveryEventFrames--;
        if(mRecoveryEventFrames == 0)
            tryEndRecoveryEvent();
    }

    //Update other players if your score changes
    FreezeTagScore* score = &mInfo->mPlayerTagScore;
    if(score->mScore != score->mPrevScore) {
        score->mPrevScore = score->mScore;
        Client::sendFreezeInfPacket();
    };

    //Debug freeze buttons
    if (al::isPadTriggerUp(-1) && al::isPadHoldX(-1) && mInfo->mIsPlayerRunner)
        trySetPlayerRunnerState(FreezeState::ALIVE);
    if (al::isPadTriggerUp(-1) && al::isPadHoldY(-1) && mInfo->mIsPlayerRunner)
        trySetPlayerRunnerState(FreezeState::FREEZE);
    if (al::isPadTriggerUp(-1) && al::isPadHoldA(-1)) {
        mInfo->mPlayerTagScore.eventScoreDebug();
    }

    //Spectate camera
    if(!mTicket->mIsActive && mInfo->mIsPlayerFreeze)
        al::startCamera(mCurScene, mTicket, -1);
    if(mTicket->mIsActive && !mInfo->mIsPlayerFreeze)
        al::endCamera(mCurScene, mTicket, 0, false);
    
    if(mTicket->mIsActive && mInfo->mIsPlayerFreeze)
        updateSpectateCam(playerBase);
}

bool FreezeTagMode::isPlayerLastSurvivor(PuppetInfo* changingPuppet)
{
    Logger::log("Runner Size: %i\n", mInfo->mRunnerPlayers.size());
    if(!mInfo->mIsPlayerRunner)
        return false; // If player is on the chaser team, just return false instantly
    
    if(mInfo->mRunnerPlayers.size() == 0)
        return false; // If there's no other player on the runner team, last survivor stuff is disabled
    
    for(int i = 0; i < mInfo->mRunnerPlayers.size(); i++) {
        PuppetInfo* inf = mInfo->mRunnerPlayers.at(i);
        if(changingPuppet == inf)
            continue; // If the puppet getting updated is the one currently being checked, skip this one

        if(!inf->isFreezeTagFreeze)
            return false; // Found another non-frozen player, not last survivor
    }

    return true; //Last survivor check passed!
}

bool FreezeTagMode::trySetPlayerRunnerState(FreezeState newState)
{
    PlayerActorBase* playerBase = rs::getPlayerActor(mCurScene);
    bool isYukimaru = !playerBase->getPlayerInfo();

    if(mInfo->mIsPlayerFreeze == newState || isYukimaru || !mInfo->mIsPlayerRunner)
        return false;
    
    PlayerActorHakoniwa* player = (PlayerActorHakoniwa*)playerBase;
    HackCap* hackCap = player->mHackCap;

    mInvulnTime = 0.f;
    
    if(newState == FreezeState::ALIVE) {
        mInfo->mIsPlayerFreeze = FreezeState::ALIVE;
        playerBase->endDemoPuppetable();
    } else {
        mInfo->mIsPlayerFreeze = FreezeState::FREEZE;
        playerBase->startDemoPuppetable();
        player->mPlayerAnimator->endSubAnim();
        player->mPlayerAnimator->startAnim("DeadIce");

        hackCap->forcePutOn();

        mSpectateIndex = -1;
    }

    Client::sendFreezeInfPacket();

    return true;
}

bool FreezeTagMode::tryStartRecoveryEvent(bool isMakeFrozen, bool isResetScore)
{
    PlayerActorBase* playerBase = rs::getPlayerActor(mCurScene);
    bool isYukimaru = !playerBase->getPlayerInfo();

    if(mRecoveryEventFrames > 0 || !mWipeHolder || isYukimaru)
        return false; //Something isn't applicable here, return fail
    
    Logger::log("Starting recovery event\n");
    
    PlayerActorHakoniwa* player = (PlayerActorHakoniwa*)playerBase;
    
    mRecoveryEventFrames = mRecoveryEventLength / 2;
    mWipeHolder->startClose("FadeBlack", mRecoveryEventLength / 4);

    Logger::log("Fade started\n");

    mRecoverySafetyPoint = player->mPlayerRecoverySafetyPoint->mSafetyPointPos;

    return true;
}

void FreezeTagMode::tryScoreEvent(FreezeInf* incomingPacket, PuppetInfo* sourcePuppet)
{
    if(!mCurScene || !GameModeManager::instance()->isModeAndActive(GameMode::FREEZETAG))
        return;

    // Get the distance of the incoming player
    float puppetDistance = al::calcDistance(rs::getPlayerActor(mCurScene), sourcePuppet->playerPos);
    bool isInRange = puppetDistance < 375.f; // Only apply this score event if player is less than this many units away

    if(isInRange) {
        //Check for unfreeze score event
        if((mInfo->mIsPlayerRunner && !mInfo->mIsPlayerFreeze) && (sourcePuppet->isFreezeTagFreeze && !incomingPacket->isFreeze)) {
            mInfo->mPlayerTagScore.eventScoreUnfreeze();
        }

        //Check for freeze score event
        if((!mInfo->mIsPlayerRunner) && (!sourcePuppet->isFreezeTagFreeze && incomingPacket->isFreeze)) {
            mInfo->mPlayerTagScore.eventScoreFreeze();
        }
    }

    if(mInfo->mIsPlayerRunner && !mInfo->mIsPlayerFreeze && !sourcePuppet->isFreezeTagFreeze
    && incomingPacket->isFreeze && isPlayerLastSurvivor(sourcePuppet)) {
        mInfo->mPlayerTagScore.eventScoreLastSurvivor();
    }
}

bool FreezeTagMode::tryEndRecoveryEvent()
{
    if(!mWipeHolder)
        return false; //Recovery event is already started, return fail
    
    PlayerActorBase* playerBase = rs::getPlayerActor(mCurScene);
    al::setTrans(playerBase, mRecoverySafetyPoint);
    
    mWipeHolder->startOpen(mRecoveryEventLength / 2);
    trySetPlayerRunnerState(FreezeState::FREEZE);

    return true;
}

void FreezeTagMode::updateSpectateCam(PlayerActorBase* playerBase)
{
    //If the specate camera ticket is active, get the camera poser
    al::CameraPoser* curPoser;
    al::CameraDirector* director = mCurScene->getCameraDirector();

    if (director) {
        al::CameraPoseUpdater* updater = director->getPoseUpdater(0);
        if (updater && updater->mTicket) {
            curPoser = updater->mTicket->mPoser;
        }
    }
    
    //Verify 100% that this poser is the actor spectator
    if (al::isEqualString(curPoser->getName(), "CameraPoserActorSpectate")) {
        cc::CameraPoserActorSpectate* spectatePoser = (cc::CameraPoserActorSpectate*)curPoser;
        spectatePoser->setPlayer(playerBase);

        //Increase or decrease spectate index, followed by clamping it
        int indexDirection = 0;
        if(al::isPadTriggerRight(-1)) indexDirection = 1; //Move index right
        if(al::isPadTriggerLeft(-1)) indexDirection = -1; //Move index left

        //Force index to decrease if your current index is higher than runner player count
        if(mSpectateIndex >= mInfo->mRunnerPlayers.size())
            indexDirection = -1;

        //Force index to decrease if your current target changes stages
        if(mSpectateIndex != -1 && indexDirection == 0)
            if(!mInfo->mRunnerPlayers.at(mSpectateIndex)->isInSameStage)
                indexDirection = -1; //Move index left

        //Loop over indexs until you find a sutible one in the same stage
        bool isFinalIndex = false;
        while(!isFinalIndex) {
            mSpectateIndex += indexDirection;

            // Start by clamping the index
            if(mSpectateIndex < -1) mSpectateIndex = mInfo->mRunnerPlayers.size() - 1;
            if(mSpectateIndex >= mInfo->mRunnerPlayers.size()) mSpectateIndex = -1;

            // If not in same stage, skip
            if(mSpectateIndex != -1) {
                if(mInfo->mRunnerPlayers.at(mSpectateIndex)->isInSameStage)
                    isFinalIndex = true;
            } else {
                isFinalIndex = true;
            }

        }
        
        //If no index change is happening, end here
        if(mPrevSpectateIndex == mSpectateIndex)
            return;

        //Apply index to target actor and HUD
        if(mSpectateIndex == -1) {
            spectatePoser->setTargetActor(al::getTransPtr(playerBase));
            mModeLayout->setSpectateString("Spectate");
        } else {
            spectatePoser->setTargetActor(&mInfo->mRunnerPlayers.at(mSpectateIndex)->playerPos);
            mModeLayout->setSpectateString(mInfo->mRunnerPlayers.at(mSpectateIndex)->puppetName);
        }

        mPrevSpectateIndex = mSpectateIndex;
    }
}
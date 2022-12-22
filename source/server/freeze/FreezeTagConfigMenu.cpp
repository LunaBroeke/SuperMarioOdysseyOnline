#include "server/freeze/FreezeTagConfigMenu.hpp"
#include <cmath>
#include <stdint.h>
#include "Keyboard.hpp"
#include "logger.hpp"
#include "server/gamemode/GameModeManager.hpp"
#include "server/freeze/FreezeTagMode.hpp"
#include "server/Client.hpp"

FreezeTagConfigMenu::FreezeTagConfigMenu() : GameModeConfigMenu() {
    mScoreKeyboard = new Keyboard(6);
    mScoreKeyboard->setHeaderText(u"Set your Freeze Tag score");
    mScoreKeyboard->setSubText(u"Must be in game and have the game mode active to set your score");
}

void FreezeTagConfigMenu::initMenu(const al::LayoutInitInfo &initInfo) {}

const sead::WFixedSafeString<0x200> *FreezeTagConfigMenu::getStringData() {
    sead::SafeArray<sead::WFixedSafeString<0x200>, mItemCount>* gamemodeConfigOptions =
        new sead::SafeArray<sead::WFixedSafeString<0x200>, mItemCount>();

    gamemodeConfigOptions->mBuffer[0].copy(u"Set Score");
    gamemodeConfigOptions->mBuffer[1].copy(u"Enable Debug Mode");
    gamemodeConfigOptions->mBuffer[2].copy(u"Disable Debug Mode");

    return gamemodeConfigOptions->mBuffer;
}

bool FreezeTagConfigMenu::updateMenu(int selectIndex) {

    FreezeTagInfo *curMode = GameModeManager::instance()->getInfo<FreezeTagInfo>();

    Logger::log("Updating freeze tag menu\n");

    if (!curMode) {
        Logger::log("Unable to Load Mode info!\n");
        return true;   
    }
    
    switch (selectIndex) {
        case 0: {
            if (GameModeManager::instance()->isModeAndActive(GameMode::FREEZETAG)) {
                uint16_t oldScore = GameModeManager::instance()->getInfo<FreezeTagInfo>()->mPlayerTagScore.mScore;
                uint16_t newScore = -1;

                // opens swkbd with the initial text set to the last saved port
                char buf[5];
                nn::util::SNPrintf(buf, 5, "%u", oldScore);

                mScoreKeyboard->openKeyboard(buf, [](nn::swkbd::KeyboardConfig& config) {
                    config.keyboardMode = nn::swkbd::KeyboardMode::ModeNumeric;
                    config.textMaxLength = 4;
                    config.textMinLength = 1;
                    config.isUseUtf8 = true;
                    config.inputFormMode = nn::swkbd::InputFormMode::OneLine;
                });

                while (true) {
                    if (mScoreKeyboard->isThreadDone()) {
                        if(!mScoreKeyboard->isKeyboardCancelled())
                            newScore = ::atoi(mScoreKeyboard->getResult());
                        break;
                    }
                    nn::os::YieldThread(); // allow other threads to run
                }
                
                if(newScore != uint16_t(-1))
                    curMode->mPlayerTagScore.mScore = newScore;
            }
            return true;
        }
        case 1: {
            if (GameModeManager::instance()->isMode(GameMode::FREEZETAG)) {
                curMode->mIsDebugMode = true;
            }
            return true;
        }
        case 2: {
            if (GameModeManager::instance()->isMode(GameMode::FREEZETAG)) {
                curMode->mIsDebugMode = false;
            }
            return true;
        }
        default:
            Logger::log("Failed to interpret Index!\n");
            return false;
    }
    
}
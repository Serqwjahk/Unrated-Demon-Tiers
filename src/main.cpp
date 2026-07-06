#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/LevelCell.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/general.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/binding/FLAlertLayer.hpp>
#include <Geode/binding/GJAccountManager.hpp>
#include <Geode/binding/GJDifficultySprite.hpp>
#include <argon/argon.hpp>

using namespace geode::prelude;

namespace {
    // codes aren't in tier order: 6 Hard, 7 Easy, 8 Medium, 9 Insane, 10 Extreme
    struct DemonTier {
        int code;
        const char* label;
    };
    constexpr DemonTier DEMON_TIERS[] = {
        {7, "Easy"}, {8, "Medium"}, {6, "Hard"}, {9, "Insane"}, {10, "Extreme"}
    };

    bool isValidTierCode(int code) {
        return code >= 6 && code <= 10;
    }

    const char* tierName(int code) {
        for (auto const& tier : DEMON_TIERS) {
            if (tier.code == code) return tier.label;
        }
        return "";
    }

    // local levels have no ID yet, so key by list slot instead
    std::string diffKey(GJGameLevel* level) {
        if (level->m_levelID.value() > 0) {
            return fmt::format("demon-tier-frame-{}", level->m_levelID.value());
        }
        return fmt::format("demon-tier-frame-local-{}", level->m_levelIndex);
    }

    // m_demon isn't set yet for unrated demons - a 10-star rate request is the tell
    bool isOwnUnratedDemon(GJGameLevel* level) {
        if (level->m_stars.value() > 0) return false;
        if (!level->m_demon.value() && level->m_starsRequested != 10) return false;

        if (level->m_isEditable) return true;

        auto accID = GJAccountManager::sharedState()->m_accountID;
        if (accID <= 0) return false;
        return level->m_accountID.value() == accID;
    }

    // same idea as above, but for any viewer, not just the owner
    bool isSharableUnratedDemon(GJGameLevel* level) {
        return level->m_levelID.value() > 0
            && level->m_stars.value() == 0
            && (level->m_demon.value() || level->m_starsRequested == 10);
    }

    constexpr const char* SERVER_URL = "https://tiers.djvemo.net.pe"; //the url will probably change in the future, but for now it works

    std::string serverUrl() {
        return SERVER_URL;
    }

    // "Unrated X Demon" stays in English even for es, matches the game's own wording
    const char* tr(const char* es, const char* en) {
        bool english = Mod::get()->getSettingValue<std::string>("language") == "English";
        return english ? en : es;
    }

    // A-Z setting, defaults to T (for "tier")
    cocos2d::enumKeyCodes pickerKey() {
        auto s = Mod::get()->getSettingValue<std::string>("open-key");
        char c = s.empty() ? 'T' : std::toupper(static_cast<unsigned char>(s[0]));
        if (c < 'A' || c > 'Z') c = 'T';
        return static_cast<cocos2d::enumKeyCodes>(c);
    }

    // Batch requests to avoid hammering the server. (its a i3 core server, almost a potato)
    constexpr double REMOTE_CACHE_TTL = 300.0; // seconds

    struct RemoteEntry {
        int tier;
        std::chrono::steady_clock::time_point when;

        bool fresh() const {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now() - when).count() < REMOTE_CACHE_TTL;
        }
    };

    std::unordered_map<int, RemoteEntry>& remoteTierCache() {
        static std::unordered_map<int, RemoteEntry> cache;
        return cache;
    }
    std::unordered_map<int, std::vector<std::function<void(int)>>>& tierWaiters() {
        static std::unordered_map<int, std::vector<std::function<void(int)>>> waiters;
        return waiters;
    }

    // >0 = tier, 0 = server has none, -1 = not cached
    int cachedRemoteTier(int levelID) {
        auto& cache = remoteTierCache();
        if (auto it = cache.find(levelID); it != cache.end() && it->second.fresh()) {
            return it->second.tier;
        }
        return -1;
    }

    void flushTierBatch() {
        auto& waiters = tierWaiters();
        if (waiters.empty()) return;

        std::string ids;
        for (auto const& [id, _] : waiters) {
            if (!ids.empty()) ids += ',';
            ids += std::to_string(id);
        }
        auto url = fmt::format("{}/v1/tiers?ids={}", serverUrl(), ids);

        geode::async::spawn(
            [url = std::move(url)] {
                return web::WebRequest()
                    .timeout(std::chrono::seconds(8))
                    .get(url);
            },
            [](web::WebResponse resp) {
                auto pending = std::move(tierWaiters());
                tierWaiters().clear();

                // don't cache failures, let it retry next time (server might be down)
                if (!resp.ok()) return;

                matjson::Value tiers;
                if (auto json = resp.json(); json.isOk()) {
                    tiers = json.unwrap()["tiers"];
                }
                auto now = std::chrono::steady_clock::now();
                for (auto& [levelID, callbacks] : pending) {
                    int tier = static_cast<int>(
                        tiers[std::to_string(levelID)].asInt().unwrapOr(0));
                    remoteTierCache()[levelID] = {tier, now};
                    if (isValidTierCode(tier)) {
                        for (auto& cb : callbacks) cb(tier);
                    }
                }
            }
        );
    }

    void fetchRemoteTier(int levelID, std::function<void(int)> onTier) {
        int cached = cachedRemoteTier(levelID);
        if (cached >= 0) {
            if (isValidTierCode(cached)) onTier(cached);
            return;
        }

        auto& waiters = tierWaiters();
        bool scheduleFlush = waiters.empty();
        waiters[levelID].push_back(std::move(onTier));
        if (scheduleFlush) {
            geode::queueInMainThread([] { flushTierBatch(); });
        }
    }

    void cacheRemoteTier(int levelID, int tier) {
        remoteTierCache()[levelID] = {tier, std::chrono::steady_clock::now()};
    }
}

class UploadStatusPopup : public geode::Popup {
protected:
    CCSprite* m_spinner = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;
    CCMenuItemSpriteExtra* m_okBtn = nullptr;
    bool m_finished = false;

    bool init() {
        if (!Popup::init(260.f, 130.f))
            return false;

        this->setTitle(tr("Compartiendo tier", "Sharing tier"));
        m_closeBtn->setVisible(false);

        auto center = m_mainLayer->getContentSize() / 2;

        m_spinner = CCSprite::create("loadingCircle.png");
        m_spinner->setScale(0.7f);
        m_spinner->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
        m_spinner->setPosition(center + CCPoint{0.f, 2.f});
        m_spinner->runAction(CCRepeatForever::create(CCRotateBy::create(1.f, 360.f)));
        m_mainLayer->addChild(m_spinner);

        m_statusLabel = CCLabelBMFont::create(tr("Preparando...", "Preparing..."), "goldFont.fnt");
        m_statusLabel->setScale(0.5f);
        m_statusLabel->setPosition(center + CCPoint{0.f, -38.f});
        m_mainLayer->addChild(m_statusLabel);

        auto menu = CCMenu::create();
        menu->setPosition(center);
        m_mainLayer->addChild(menu);

        auto okSpr = ButtonSprite::create("OK", "goldFont.fnt", "GJ_button_01.png", 0.9f);
        m_okBtn = CCMenuItemSpriteExtra::create(
            okSpr, this, menu_selector(UploadStatusPopup::onOk)
        );
        m_okBtn->setPosition({0.f, -38.f});
        m_okBtn->setVisible(false);
        menu->addChild(m_okBtn);

        return true;
    }

    void onOk(CCObject*) {
        this->onClose(nullptr);
    }

    // can't back out until it's done
    void keyBackClicked() override {
        if (m_finished) Popup::keyBackClicked();
    }

public:
    void setStatus(std::string const& text) {
        if (m_statusLabel) m_statusLabel->setString(text.c_str());
    }

    void finish(bool success, std::string const& message) {
        m_finished = true;
        m_spinner->stopAllActions();
        m_spinner->setVisible(false);
        this->setTitle(success ? tr("Listo!", "Done!") : "Error");
        m_closeBtn->setVisible(true);
        m_okBtn->setVisible(true);

        m_statusLabel->setString(message.c_str());
        m_statusLabel->setPosition(m_mainLayer->getContentSize() / 2 + CCPoint{0.f, 8.f});
        m_statusLabel->setScale(std::min(
            0.5f, 220.f / std::max(1.f, m_statusLabel->getContentSize().width)));
    }

    static UploadStatusPopup* create() {
        auto ret = new UploadStatusPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

namespace {
    std::string_view describeAuthProgress(argon::AuthProgress progress) {
        switch (progress) {
            case argon::AuthProgress::RequestedChallenge:
            case argon::AuthProgress::RetryingRequest:
                return tr("Autenticando: pidiendo challenge...",
                          "Authenticating: requesting challenge...");
            case argon::AuthProgress::SolvingChallenge:
            case argon::AuthProgress::RetryingSolve:
                return tr("Autenticando: resolviendo challenge...",
                          "Authenticating: solving challenge...");
            case argon::AuthProgress::VerifyingChallenge:
            case argon::AuthProgress::RetryingVerify:
                return tr("Autenticando: verificando...",
                          "Authenticating: verifying...");
        }
        return tr("Autenticando...", "Authenticating...");
    }

    std::string serverErrorMessage(web::WebResponse const& resp) {
        if (auto json = resp.json(); json.isOk()) {
            if (auto detail = json.unwrap()["detail"].asString(); detail.isOk()) {
                return detail.unwrap();
            }
        }
        if (resp.code() <= 0) {
            return tr("No se pudo conectar al servidor", "Could not reach the server");
        }
        return fmt::format(fmt::runtime(tr("Error del servidor (HTTP {})", "Server error (HTTP {})")),
                           resp.code());
    }

    // tier 0 means delete - same endpoint either way
    void uploadTier(int levelID, int code) {
        if (!argon::signedIn()) {
            auto popup = UploadStatusPopup::create();
            popup->show();
            popup->finish(false, tr("Inicia sesion en tu cuenta de GD\npara compartir el tier",
                                    "Log into your GD account\nto share the tier"));
            return;
        }

        auto popup = UploadStatusPopup::create();
        popup->show();
        popup->setStatus(tr("Iniciando sesion con tu cuenta de GD...",
                            "Logging in with your GD account..."));
        auto popupRef = Ref(popup);

        // progress callbacks can fire off-thread, so hop back to main for UI updates
        argon::AuthOptions options;
        options.account = argon::getGameAccountData();
        options.progress = [popupRef](argon::AuthProgress progress) {
            std::string text{describeAuthProgress(progress)};
            geode::queueInMainThread([popupRef, text = std::move(text)] {
                popupRef->setStatus(text);
            });
        };
        auto url = serverUrl();

        geode::async::spawn(
            [options = std::move(options)]() mutable {
                return argon::startAuth(std::move(options));
            },
            [levelID, code, url = std::move(url), popupRef](geode::Result<std::string> res) {
                if (!res.isOk()) {
                    popupRef->finish(false, fmt::format(
                        fmt::runtime(tr("Login GD fallido:\n{}", "GD login failed:\n{}")),
                        res.unwrapErr()));
                    return;
                }
                popupRef->setStatus(code != 0
                    ? tr("Enviando tier...", "Uploading tier...")
                    : tr("Eliminando tier...", "Deleting tier..."));
                auto body = matjson::makeObject({
                    {"account_id", GJAccountManager::sharedState()->m_accountID},
                    {"level_id", levelID},
                    {"tier", code},
                    {"token", std::move(res).unwrap()},
                });
                geode::async::spawn(
                    [url, body = std::move(body)] {
                        return web::WebRequest()
                            .timeout(std::chrono::seconds(10))
                            .bodyJSON(body)
                            .post(url + "/v1/tiers");
                    },
                    [levelID, code, popupRef](web::WebResponse resp) {
                        if (resp.ok()) {
                            cacheRemoteTier(levelID, code);
                            popupRef->finish(true,
                                code != 0
                                    ? fmt::format(fmt::runtime(tr("Tier compartido:\nUnrated {} Demon",
                                                     "Tier shared:\nUnrated {} Demon")),
                                                  tierName(code))
                                    : tr("Tier eliminado del servidor", "Tier deleted from the server"));
                        } else {
                            popupRef->finish(false, serverErrorMessage(resp));
                        }
                    }
                );
            }
        );
    }
}

class DemonDifficultyPopup : public geode::Popup {
protected:
    GJGameLevel* m_level = nullptr;
    Function<void(int)> m_onPicked;

    bool init(GJGameLevel* level, Function<void(int)> onPicked) {
        if (!Popup::init(240.f, 140.f))
            return false;

        m_level = level;
        m_onPicked = std::move(onPicked);

        this->setTitle(tr("Tier de Demon", "Demon Tier"));

        int current = Mod::get()->getSavedValue<int>(diffKey(level), 0);

        auto menu = CCMenu::create();
        menu->setPosition(m_mainLayer->getContentSize() / 2 + CCPoint{0.f, 10.f});
        m_mainLayer->addChild(menu);

        constexpr float spacing = 44.f;
        constexpr float startX = -spacing * (std::size(DEMON_TIERS) - 1) / 2.f;

        for (size_t i = 0; i < std::size(DEMON_TIERS); i++) {
            auto sprite = GJDifficultySprite::create(DEMON_TIERS[i].code, GJDifficultyName::Long);
            sprite->setScale(0.8f);

            if (isValidTierCode(current) && DEMON_TIERS[i].code != current) {
                sprite->setColor({125, 125, 125});
            }

            auto btn = CCMenuItemSpriteExtra::create(
                sprite, this, menu_selector(DemonDifficultyPopup::onPick)
            );
            btn->setTag(DEMON_TIERS[i].code);
            btn->setPosition({startX + spacing * i, 0.f});
            menu->addChild(btn);
        }

        auto resetSpr = ButtonSprite::create(tr("Quitar", "Remove"), "goldFont.fnt", "GJ_button_04.png", 0.8f);
        auto resetBtn = CCMenuItemSpriteExtra::create(
            resetSpr, this, menu_selector(DemonDifficultyPopup::onReset)
        );
        resetBtn->setPosition({0.f, -42.f});
        menu->addChild(resetBtn);

        return true;
    }

    void onPick(CCObject* sender) {
        int code = static_cast<CCNode*>(sender)->getTag();
        Mod::get()->setSavedValue(diffKey(m_level), code);
        if (m_onPicked) m_onPicked(code);
        this->onClose(nullptr);
    }

    void onReset(CCObject*) {
        geode::createQuickPopup(
            tr("Quitar tier", "Remove tier"),
            tr("Esto quita el tier del nivel y <cr>deja de compartirlo</c> "
               "con otros jugadores.\n<cy>Continuar?</c>",
               "This removes the level's tier and <cr>stops sharing it</c> "
               "with other players.\n<cy>Continue?</c>"),
            tr("Cancelar", "Cancel"), tr("Quitar", "Remove"),
            [this](FLAlertLayer*, bool btn2) {
                if (!btn2) return;
                Mod::get()->setSavedValue(diffKey(m_level), 0);
                if (m_onPicked) m_onPicked(0);
                this->onClose(nullptr);
            }
        );
    }

public:
    static DemonDifficultyPopup* create(GJGameLevel* level, Function<void(int)> onPicked) {
        auto ret = new DemonDifficultyPopup();
        if (ret->init(level, std::move(onPicked))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(MyLevelInfoLayer, LevelInfoLayer) {
    struct Fields {
        bool shownDebugPopup = false;
        CCLabelBMFont* unratedLabel = nullptr;
        float coinShift = 0.f;
    };

    // Use the icon-only frames so we can draw our own label. (we were using different icons with their diff name in the image, WHY ROBTOP THOUGHT THAT WAS A GOOD IDEA?)
    void applyTierVisual(int code) {
        if (!m_difficultySprite) return;

        shiftCoins(-m_fields->coinShift);
        m_fields->coinShift = 0.f;

        float oldBottom = m_difficultySprite->boundingBox().getMinY();

        if (isValidTierCode(code)) {
            auto frameName = fmt::format("diffIcon_{:02d}_btn_001.png", code);
            auto frame = CCSpriteFrameCache::sharedSpriteFrameCache()
                             ->spriteFrameByName(frameName.c_str());
            if (frame) {
                m_difficultySprite->setDisplayFrame(frame);
            } else {
                m_difficultySprite->updateDifficultyFrame(code, GJDifficultyName::Short);
            }

            auto text = fmt::format("Unrated {}\nDemon", tierName(code));
            if (!m_fields->unratedLabel) {
                m_fields->unratedLabel = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
                m_fields->unratedLabel->setAlignment(kCCTextAlignmentCenter);
                m_fields->unratedLabel->setScale(0.45f);
                m_fields->unratedLabel->setAnchorPoint({0.5f, 1.f});
                this->addChild(m_fields->unratedLabel, m_difficultySprite->getZOrder());
            } else {
                m_fields->unratedLabel->setString(text.c_str());
                m_fields->unratedLabel->setVisible(true);
            }

            constexpr float labelGap = 1.f;
            constexpr float coinTighten = 6.f;

            auto faceBB = m_difficultySprite->boundingBox();
            m_fields->unratedLabel->setPosition(
                {m_difficultySprite->getPositionX(), faceBB.getMinY() - labelGap}
            );

            // coins assume the old frame size, so nudge them to match (its kinda bugging that the coin positions are hardcoded in the first place, but whatever)
            float newBottom = labelBottomInLayerSpace(m_fields->unratedLabel);
            m_fields->coinShift = newBottom - oldBottom + coinTighten;
            shiftCoins(m_fields->coinShift);
        } else {
            // back to the normal rated face
            m_difficultySprite->updateDifficultyFrame(
                m_level->getAverageDifficulty(), GJDifficultyName::Short
            );
            if (m_fields->unratedLabel) {
                m_fields->unratedLabel->setVisible(false);
            }
        }
    }

    float labelBottomInLayerSpace(CCNode* label) {
        auto bb = label->boundingBox();
        auto parent = label->getParent();
        if (!parent || parent == this) return bb.getMinY();
        auto world = parent->convertToWorldSpace({bb.getMinX(), bb.getMinY()});
        return this->convertToNodeSpace(world).y;
    }

    void shiftCoins(float dy) {
        if (dy == 0.f || !m_coins) return;
        for (auto coin : CCArrayExt<CCNode*>(m_coins)) {
            coin->setPositionY(coin->getPositionY() + dy);
        }
    }

    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge))
            return false;

        bool eligible = isOwnUnratedDemon(level);
        int saved = eligible ? Mod::get()->getSavedValue<int>(diffKey(level), 0) : 0;
        if (isValidTierCode(saved)) {
            applyTierVisual(saved);
        } else if (isSharableUnratedDemon(level)) {
            int levelID = level->m_levelID.value();
            fetchRemoteTier(levelID, [self = Ref(this), levelID](int tier) {
                // still the same level?
                if (self->m_level && self->m_level->m_levelID.value() == levelID) {
                    self->applyTierVisual(tier);
                }
            });
        }

        return true;
    }

    // own saved tier wins over the shared one
    int effectiveTier() {
        if (isOwnUnratedDemon(m_level)) {
            int saved = Mod::get()->getSavedValue<int>(diffKey(m_level), 0);
            if (isValidTierCode(saved)) return saved;
        }
        if (isSharableUnratedDemon(m_level)) {
            int cached = cachedRemoteTier(m_level->m_levelID.value());
            if (isValidTierCode(cached)) return cached;
        }
        return 0;
    }

    // Other mods touch the sprite from this hook too,
    // and it fires again once the level's downloaded - reapply a frame late so we win
    void updateLabelValues() {
        LevelInfoLayer::updateLabelValues();
        if (effectiveTier() != 0) {
            this->scheduleOnce(schedule_selector(MyLevelInfoLayer::reapplyTier), 0.f);
        }
    }

    void reapplyTier(float) {
        int tier = effectiveTier();
        if (isValidTierCode(tier)) applyTierVisual(tier);
    }

    // Wait one frame or the popup gets eaten
    void onEnterTransitionDidFinish() {
        LevelInfoLayer::onEnterTransitionDidFinish();

        if (m_fields->shownDebugPopup) return;
        m_fields->shownDebugPopup = true;

        if (Mod::get()->getSettingValue<bool>("debug-popup")) {
            this->scheduleOnce(schedule_selector(MyLevelInfoLayer::showDebugPopup), 0.f);
        }
    }

    void showDebugPopup(float) {
        auto level = m_level;
        auto content = fmt::format(
            "Name: {}\n"
            "ID: {}\n"
            "Demon: {}\n"
            "Difficulty: {}\n"
            "Stars: {}\n"
            "Stars requested: {}\n"
            "Demon difficulty: {}\n"
            "Account: {} (logged: {})\n"
            "Ratings: {}\n"
            "Featured: {}",
            level->m_levelName,
            level->m_levelID.value(),
            level->m_demon.value(),
            static_cast<int>(level->m_difficulty),
            level->m_stars.value(),
            level->m_starsRequested,
            level->m_demonDifficulty,
            level->m_accountID.value(),
            GJAccountManager::sharedState()->m_accountID,
            level->m_ratings,
            level->m_featured
        );
        geode::createQuickPopup(
            "Unrated Demon Tiers (debug)",
            content,
            "Copy",
            "OK",
            [content](FLAlertLayer*, bool btn2) {
                if (!btn2) {
                    geode::utils::clipboard::write(content);
                }
            }
        );
    }

    void openTierPopup() {
        DemonDifficultyPopup::create(m_level, [this](int code) {
            this->applyTierVisual(code);
            if (m_level->m_levelID.value() > 0) {
                uploadTier(m_level->m_levelID.value(), code);
            }
        })->show();
    }
};

// Same thing but for list cells. Cells get recycled while scrolling.
class $modify(MyLevelCell, LevelCell) {
    void loadFromLevel(GJGameLevel* level) {
        LevelCell::loadFromLevel(level);

        if (isOwnUnratedDemon(level)
            && isValidTierCode(Mod::get()->getSavedValue<int>(diffKey(level), 0))) {
            // one frame late, so it lands after other mods' hooks
            this->scheduleOnce(schedule_selector(MyLevelCell::applySavedTier), 0.f);
        } else if (isSharableUnratedDemon(level)) {
            int levelID = level->m_levelID.value();
            fetchRemoteTier(levelID, [self = Ref(this), levelID](int tier) {
                // still the same level?
                if (self->m_level && self->m_level->m_levelID.value() == levelID) {
                    self->applyTierFrame(tier);
                }
            });
        }
    }

    void applySavedTier(float) {
        if (!m_level) return;
        applyTierFrame(Mod::get()->getSavedValue<int>(diffKey(m_level), 0));
    }

    void applyTierFrame(int code) {
        if (!isValidTierCode(code)) return;

        auto container = m_mainLayer->getChildByID("difficulty-container");
        if (!container) return;
        if (auto sprite = typeinfo_cast<GJDifficultySprite*>(
                container->getChildByID("difficulty-sprite"))) {
            sprite->updateDifficultyFrame(code, GJDifficultyName::Short);
        }
    }
};

// hooked here since LevelInfoLayer never gets keyDown events itself
class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat && key == pickerKey()) {
            auto scene = CCDirector::sharedDirector()->getRunningScene();
            // don't do anything if a popup's already open
            if (scene && !scene->getChildByType<FLAlertLayer>(0)) {
                if (auto layer = scene->getChildByType<LevelInfoLayer>(0)) {
                    if (isOwnUnratedDemon(layer->m_level)) {
                        static_cast<MyLevelInfoLayer*>(layer)->openTierPopup();
                        return true;
                    }
                }
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
//ah, tmb hablo español btw
//en parte es por eso que hay modo en español y otro en ingles :/
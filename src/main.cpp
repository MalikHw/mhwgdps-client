#include <Geode/Geode.hpp>
#include <Geode/modify/GManager.hpp>
#include <Geode/modify/MusicDownloadManager.hpp>
#include <Geode/modify/LevelTools.hpp>
#include <Geode/modify/LevelSelectLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <km7dev.server_api/include/ServerAPIEvents.hpp>
#include "common.hpp"

using namespace geode::prelude;

#ifdef GEODE_IS_IOS
	static constexpr const char* saveFolder = "save/mhwgdps";
#else
	static constexpr const char* saveFolder = "mhwgdps";
#endif

struct MainLevel {
	std::string name, creator, desc, data;
	int length = 3, diff = 0, stars = 0;
};

static std::vector<MainLevel>& mainLevels() {
	static std::vector<MainLevel> list;
	static bool loaded = false;
	if (loaded) return list;
	loaded = true;

	auto dir = Mod::get()->getResourcesDir() / "levels";
	for (int i = 1;; i++) {
		auto res = file::readJson(dir / fmt::format("{}.json", i));
		if (!res) break;
		auto j = res.unwrap();
		MainLevel l;
		l.name = j["name"].asString().unwrapOr("Unnamed");
		l.creator = j["creator"].asString().unwrapOr("-");
		l.desc = j["description"].asString().unwrapOr("");
		l.data = j["data"].asString().unwrapOr("");
		l.length = j["length"].asInt().unwrapOr(3);
		l.diff = j["difficulty"].asInt().unwrapOr(0);
		l.stars = j["stars"].asInt().unwrapOr(0);
		list.push_back(std::move(l));
	}
	log::info("loaded {} main levels", list.size());
	return list;
}

static bool isMain(int id) {
	return mhEnabled() && id >= 1 && id <= (int)mainLevels().size();
}

static constexpr int trackBase = 1000;

$execute {
	if (!mhEnabled()) return;
	ServerAPIEvents::registerServer("https://mhwgdps.ps.fhgdps.com/", 0);
}

class $modify(MHManager, GManager) {
	struct Fields {
		std::string originalName;
	};

	void setup() {
		if (!mhEnabled() || !m_fields->originalName.empty()) return GManager::setup();

		auto dir = dirs::getSaveDir() / "mhwgdps";
		std::error_code err;
		std::filesystem::create_directories(dir, err);
		if (err) log::error("couldn't create {}: {}", dir, err.message());

		m_fields->originalName = m_fileName;
		m_fileName = fmt::format("{}/{}", saveFolder, m_fileName);
		GManager::setup();
	}
};

class $modify(MHMusic, MusicDownloadManager) {
	std::string pathForSFXFolder(int id) {
		if (!mhEnabled()) return MusicDownloadManager::pathForSFXFolder(id);
		return string::pathToString(dirs::getSaveDir() / "mhwgdps" / "");
	}

	std::string pathForSongFolder(int id) {
		if (!mhEnabled()) return MusicDownloadManager::pathForSongFolder(id);
		return string::pathToString(dirs::getSaveDir() / "mhwgdps" / "");
	}
};

class $modify(MHLevelTools, LevelTools) {
	static GJGameLevel* getLevel(int id, bool noString) {
		if (!isMain(id)) return LevelTools::getLevel(id, noString);

		auto& d = mainLevels()[id - 1];
		auto lvl = GJGameLevel::create();
		lvl->m_levelID = id;
		lvl->m_levelName = d.name;
		lvl->m_creatorName = d.creator;
		lvl->m_levelDesc = d.desc;
		lvl->m_levelLength = d.length;
		lvl->m_difficulty = (GJDifficulty)d.diff;
		lvl->m_stars = d.stars;
		lvl->m_levelType = GJLevelType::Main;
		lvl->m_audioTrack = trackBase + id;
		if (!noString) lvl->m_levelString = d.data;
		return lvl;
	}

	static gd::unordered_set<int> getLevelList() {
		if (!mhEnabled()) return LevelTools::getLevelList();
		gd::unordered_set<int> ret;
		for (int i = 1; i <= (int)mainLevels().size(); i++) ret.insert(i);
		return ret;
	}

	static bool verifyLevelIntegrity(gd::string str, int id) {
		if (isMain(id)) return true;
		return LevelTools::verifyLevelIntegrity(str, id);
	}

	static gd::string getAudioFileName(int id) {
		if (mhEnabled() && id > trackBase) {
			auto path = Mod::get()->getResourcesDir() / fmt::format("{}.mp3", id - trackBase);
			if (std::filesystem::exists(path)) return string::pathToString(path);
			log::warn("missing song {}", path);
			return LevelTools::getAudioFileName(0);
		}
		return LevelTools::getAudioFileName(id);
	}

	static gd::string getAudioTitle(int id) {
		if (mhEnabled() && id > trackBase) {
			int n = id - trackBase;
			if (n <= (int)mainLevels().size()) return mainLevels()[n - 1].name;
		}
		return LevelTools::getAudioTitle(id);
	}
};

class $modify(MHSelect, LevelSelectLayer) {
	bool init(int page) {
		if (!mhEnabled()) return LevelSelectLayer::init(page);

		int n = mainLevels().size();
		if (page >= n) page = n - 1;
		if (page < 0) page = 0;
		if (!LevelSelectLayer::init(page)) return false;

		while (m_scrollLayer->getTotalPages() > n) m_scrollLayer->removePageWithNumber(n);
		while (m_scrollLayer->m_dots && m_scrollLayer->m_dots->count() > n) {
			static_cast<CCNode*>(m_scrollLayer->m_dots->lastObject())->removeFromParent();
			m_scrollLayer->m_dots->removeLastObject();
		}
		m_scrollLayer->instantMoveToPage(page);
		return true;
	}
};

class $modify(MHMenu, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) return false;
		if (!mhEnabled()) return true;

		auto old = this->getChildByID("main-title");
		auto spr = CCSprite::create("title.png"_spr);
		if (!old || !spr) return true;

		float w = old->getContentWidth() * old->getScaleX();
		spr->setScale(w / spr->getContentWidth());
		spr->setPosition(old->getPosition());
		spr->setAnchorPoint(old->getAnchorPoint());
		spr->setID("main-title");
		this->addChild(spr, old->getZOrder());
		old->removeFromParent();
		return true;
	}
};

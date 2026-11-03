#include <Geode/Geode.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/ObjectToolbox.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/base64.hpp>
#include "common.hpp"

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif
#ifdef GEODE_IS_ANDROID
#include <Geode/cocos/platform/android/jni/JniHelper.h>
#endif

using namespace geode::prelude;

static constexpr int vibrateID = 9701;
static constexpr int toastID = 9702;

static int templateFor(int id) {
	return id == vibrateID ? 1520 : 1268;
}

static bool isMine(int id) {
	return id == vibrateID || id == toastID;
}

static std::string iconFor(int id) {
	return id == vibrateID ? "vibrate_trigger.png"_spr : "toast_trigger.png"_spr;
}

struct IdSwap {
	GameObject* obj;
	int orig;

	IdSwap(GameObject* o) : obj(o), orig(o->m_objectID) {
		if (mhEnabled() && isMine(orig)) obj->m_objectID = templateFor(orig);
	}
	~IdSwap() { obj->m_objectID = orig; }
};

#ifdef GEODE_IS_ANDROID
// jni snippets for toast + vibrate
static JNIEnv* getEnv() {
	auto vm = JniHelper::getJavaVM();
	if (!vm) return nullptr;
	JNIEnv* env = nullptr;
	if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK) return nullptr;
	return env;
}

static bool jfail(JNIEnv* env) {
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return true;
	}
	return false;
}

static int getSdk(JNIEnv* env) {
	auto cls = env->FindClass("android/os/Build$VERSION");
	if (!cls) { jfail(env); return 0; }
	auto id = env->GetStaticFieldID(cls, "SDK_INT", "I");
	if (!id) { jfail(env); return 0; }
	int sdk = env->GetStaticIntField(cls, id);
	env->DeleteLocalRef(cls);
	return sdk;
}

static jobject getApp(JNIEnv* env) {
	auto cls = env->FindClass("android/app/ActivityThread");
	if (!cls) { jfail(env); return nullptr; }
	auto m = env->GetStaticMethodID(cls, "currentApplication", "()Landroid/app/Application;");
	if (!m) { jfail(env); return nullptr; }
	auto app = env->CallStaticObjectMethod(cls, m);
	env->DeleteLocalRef(cls);
	if (jfail(env)) return nullptr;
	return app;
}

static bool doVibrate(int ms) {
	auto env = getEnv();
	if (!env) return false;
	auto app = getApp(env);
	if (!app) return false;

	auto ctxCls = env->GetObjectClass(app);
	auto gss = env->GetMethodID(ctxCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
	if (!gss) { jfail(env); return false; }
	auto name = env->NewStringUTF("vibrator");
	auto vib = env->CallObjectMethod(app, gss, name);
	if (jfail(env) || !vib) return false;

	auto vibCls = env->FindClass("android/os/Vibrator");
	if (!vibCls) { jfail(env); return false; }

	if (getSdk(env) >= 26) {
		auto effCls = env->FindClass("android/os/VibrationEffect");
		auto make = env->GetStaticMethodID(effCls, "createOneShot", "(JI)Landroid/os/VibrationEffect;");
		if (!make) { jfail(env); return false; }
		auto eff = env->CallStaticObjectMethod(effCls, make, (jlong)ms, -1); // -1 = default amplitude
		if (jfail(env) || !eff) return false;
		auto vibrate = env->GetMethodID(vibCls, "vibrate", "(Landroid/os/VibrationEffect;)V");
		if (!vibrate) { jfail(env); return false; }
		env->CallVoidMethod(vib, vibrate, eff);
	} else {
		auto vibrate = env->GetMethodID(vibCls, "vibrate", "(J)V");
		if (!vibrate) { jfail(env); return false; }
		env->CallVoidMethod(vib, vibrate, (jlong)ms);
	}
	return !jfail(env);
}

static bool doToast(std::string const& text) {
	auto env = getEnv();
	if (!env) return false;
	// text toasts only show properly without a looper loop on api 30+ (Android 11+)
	if (getSdk(env) < 30) return false;
	auto app = getApp(env);
	if (!app) return false;

	// gl thread has no looper, toast wants one
	auto looperCls = env->FindClass("android/os/Looper");
	if (!looperCls) { jfail(env); return false; }
	auto myLooper = env->GetStaticMethodID(looperCls, "myLooper", "()Landroid/os/Looper;");
	auto prepare = env->GetStaticMethodID(looperCls, "prepare", "()V");
	if (!myLooper || !prepare) { jfail(env); return false; }
	if (!env->CallStaticObjectMethod(looperCls, myLooper)) {
		env->CallStaticVoidMethod(looperCls, prepare);
		if (jfail(env)) return false;
	}

	auto toastCls = env->FindClass("android/widget/Toast");
	if (!toastCls) { jfail(env); return false; }
	auto make = env->GetStaticMethodID(toastCls, "makeText",
		"(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
	if (!make) { jfail(env); log::warn("Toast.makeText not found"); return false; }

	auto jtext = env->NewStringUTF(text.c_str());
	auto toast = env->CallStaticObjectMethod(toastCls, make, app, jtext, 0);
	if (jfail(env) || !toast) { log::warn("Toast.makeText threw"); return false; }

	auto show = env->GetMethodID(toastCls, "show", "()V");
	if (!show) { jfail(env); return false; }
	env->CallVoidMethod(toast, show);
	return !jfail(env);
}
#endif

#ifdef GEODE_IS_WINDOWS
class WindowShake : public CCNode {
	HWND m_win = nullptr;
	int m_x = 0, m_y = 0;
	float m_time = 0.f;
	static inline bool s_running = false;

	void move(int dx, int dy) {
		SetWindowPos(m_win, nullptr, m_x + dx, m_y + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	void update(float dt) override {
		m_time += dt;
		if (m_time >= 0.3f) return this->removeFromParent();
		this->move((std::rand() % 5) - 2, (std::rand() % 5) - 2);
	}

public:
	~WindowShake() {
		if (m_win) this->move(0, 0);
		s_running = false;
	}

	static void start() {
		auto scene = CCDirector::get()->getRunningScene();
		HWND win = GetActiveWindow();
		RECT rect;
		if (s_running || !scene || !win || !GetWindowRect(win, &rect)) return;

		auto node = new WindowShake();
		node->m_win = win;
		node->m_x = rect.left;
		node->m_y = rect.top;
		node->autorelease();
		node->scheduleUpdate();
		scene->addChild(node);
		s_running = true;
	}
};
#endif

static void vibrate() {
#ifdef GEODE_IS_ANDROID
	if (!doVibrate(300)) log::warn("vibrate failed");
#elif defined(GEODE_IS_WINDOWS)
	WindowShake::start();
#endif
}

static void toast(std::string const& text) {
#ifdef GEODE_IS_ANDROID
	if (doToast(text)) return;
#endif
	Notification::create(text, NotificationIcon::Info, 2.f)->show();
}

class $modify(MHEffect, EffectGameObject) {
	struct Fields {
		std::string text;
		int kind = 0;
	};

	// m_objectID can be swapped while gd runs, so remember what we are
	int myKind() {
		if (m_fields->kind) return m_fields->kind;
		return isMine(m_objectID) ? m_objectID : 0;
	}

	void customSetup() {
		IdSwap swap(this);
		EffectGameObject::customSetup();
	}

	void triggerActivated(float x) {
		IdSwap swap(this);
		EffectGameObject::triggerActivated(x);
	}

	void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
		if (mhEnabled()) {
			int kind = this->myKind();
			if (kind == vibrateID) return vibrate();
			if (kind == toastID) return toast(m_fields->text);
		}
		EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
	}

	gd::string getSaveString(GJBaseGameLayer* layer) {
		std::string s = EffectGameObject::getSaveString(layer);
		if (this->myKind() == toastID && !m_fields->text.empty()) {
			if (s.ends_with(',')) s.pop_back();
			s += fmt::format(",31,{}", base64::encode(m_fields->text, base64::Base64Variant::UrlWithPad));
		}
		return s;
	}

	void customObjectSetup(gd::vector<gd::string>& values, gd::vector<void*>& exists) {
		IdSwap swap(this);
		EffectGameObject::customObjectSetup(values, exists);
		if (!isMine(swap.orig) || values.size() <= 31) return;
		std::string raw = values[31];
		if (raw.empty()) return;
		auto res = base64::decodeString(raw, base64::Base64Variant::UrlWithPad);
		if (res) m_fields->text = res.unwrap();
	}
};

class $modify(MHObject, GameObject) {
	void customSetup() {
		IdSwap swap(this);
		GameObject::customSetup();
	}

	static GameObject* createWithKey(int key) {
		if (!mhEnabled() || !isMine(key)) return GameObject::createWithKey(key);

		auto obj = GameObject::createWithKey(templateFor(key));
		if (!obj) return nullptr;
		obj->initWithSpriteFrameName(iconFor(key).c_str());
		if (auto eff = typeinfo_cast<EffectGameObject*>(obj)) static_cast<MHEffect*>(eff)->m_fields->kind = key;
		obj->m_objectID = key;
		return obj;
	}

	static GameObject* objectFromVector(gd::vector<gd::string>& values, gd::vector<void*>& exists, GJBaseGameLayer* layer, bool lowDetail) {
		auto obj = GameObject::objectFromVector(values, exists, layer, lowDetail);
		if (!obj || !mhEnabled() || !isMine(obj->m_objectID)) return obj;
		if (auto eff = typeinfo_cast<EffectGameObject*>(obj)) eff->customObjectSetup(values, exists);
		return obj;
	}
};

class $modify(MHLayer, GJBaseGameLayer) {
	void spawnObject(GameObject* object, double delay, gd::vector<int> const& remapKeys) {
		IdSwap swap(object);
		GJBaseGameLayer::spawnObject(object, delay, remapKeys);
	}
};

class $modify(MHToolbox, ObjectToolbox) {
	bool init() {
		if (!ObjectToolbox::init()) return false;
		if (!mhEnabled()) return true;
		for (int id : {vibrateID, toastID}) m_allKeys[id] = iconFor(id);
		return true;
	}
};

class MHPopup : public SetupTriggerPopup {
	Ref<TextInput> m_input;

	bool setupPopup(EffectGameObject* obj, CCArray* objs) {
		bool hasText = obj->m_objectID == toastID;
		float w = 340.f, h = hasText ? 230.f : 190.f;
		if (!SetupTriggerPopup::init(obj, objs, w, h, 1)) return false;

		this->addTitle(hasText ? "Toast Trigger" : "Vibrate Trigger");
		this->createMultiTriggerItemsDefault();
		this->addCloseButton("OK");

		if (hasText) {
			CCNode* parent = m_mainLayer;
			for (auto child : CCArrayExt<CCNode*>(m_mainLayer->getChildren())) {
				if (typeinfo_cast<extension::CCScale9Sprite*>(child)) { parent = child; break; }
			}

			m_input = TextInput::create(260.f, "Text", "bigFont.fnt");
			m_input->setLabel("Text");
			m_input->setMaxCharCount(120);
			m_input->setString(static_cast<MHEffect*>(obj)->m_fields->text, false);
			m_input->setCallback([this](std::string const& str) { this->applyText(str); });
			m_input->setPosition({ w / 2.f, h - 80.f });
			parent->addChild(m_input);
		}

		this->determineStartValues();
		return true;
	}

	void applyText(std::string const& str) {
		if (m_gameObjects && m_gameObjects->count()) {
			for (auto o : CCArrayExt<EffectGameObject*>(m_gameObjects)) static_cast<MHEffect*>(o)->m_fields->text = str;
		} else if (m_gameObject) {
			static_cast<MHEffect*>(m_gameObject)->m_fields->text = str;
		}
	}

public:
	static MHPopup* create(EffectGameObject* obj, CCArray* objs) {
		auto ret = new MHPopup();
		if (ret->setupPopup(obj, objs)) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}

	void onClose(CCObject* sender) override {
		if (m_input) this->applyText(m_input->getString());
		SetupTriggerPopup::onClose(sender);
	}
};

class $modify(MHEditor, EditorUI) {
	void setupCreateMenu() {
		EditorUI::setupCreateMenu();
		if (!mhEnabled()) return;

		auto gm = GameManager::get();
		int cols = gm->getIntGameVariable(GameVar::EditorButtonsPerRow);
		int rows = gm->getIntGameVariable(GameVar::EditorButtonRows);

		for (auto bar : CCArrayExt<EditButtonBar*>(m_createButtonBars)) {
			bool isTriggerBar = false;
			for (auto btn : CCArrayExt<CCNode*>(bar->m_buttonArray)) {
				auto item = typeinfo_cast<CreateMenuItem*>(btn);
				if (item && item->m_objectID == templateFor(vibrateID)) isTriggerBar = true;
			}
			if (!isTriggerBar) continue;

			bar->m_buttonArray->addObject(this->getCreateBtn(vibrateID, 4));
			bar->m_buttonArray->addObject(this->getCreateBtn(toastID, 4));
			bar->reloadItems(rows, cols);
			break;
		}
	}

	void editObject(CCObject* sender) {
		if (mhEnabled()) {
			CCArray* list = nullptr;
			if (m_selectedObjects->count()) list = m_selectedObjects;
			else if (m_selectedObject) list = CCArray::createWithObject(m_selectedObject);

			if (list && list->count()) {
				int id = static_cast<GameObject*>(list->objectAtIndex(0))->m_objectID;
				bool same = true;
				for (auto o : CCArrayExt<GameObject*>(list)) if (o->m_objectID != id) same = false;

				if (same && (id == vibrateID || id == toastID)) {
					MHPopup::create(static_cast<EffectGameObject*>(list->objectAtIndex(0)), list)->show();
					return;
				}
			}
		}
		EditorUI::editObject(sender);
	}
};
#include <Geode/Geode.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/ObjectToolbox.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/base64.hpp>
#include "common.hpp"

#ifdef GEODE_IS_WINDOWS
#include <Geode/cocos/robtop/glfw/glfw3.h>
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
	GLFWwindow* m_win = nullptr;
	int m_x = 0, m_y = 0;
	float m_time = 0.f;
	static inline bool s_running = false;

	void update(float dt) override {
		m_time += dt;
		if (m_time >= 0.3f) return this->removeFromParent();
		glfwSetWindowPos(m_win, m_x + (std::rand() % 5) - 2, m_y + (std::rand() % 5) - 2);
	}

public:
	~WindowShake() {
		if (m_win) glfwSetWindowPos(m_win, m_x, m_y);
		s_running = false;
	}

	static void start() {
		auto scene = CCDirector::get()->getRunningScene();
		auto view = CCEGLView::sharedOpenGLView();
		if (s_running || !scene || !view || !view->getWindow()) return;

		auto node = new WindowShake();
		node->m_win = view->getWindow();
		glfwGetWindowPos(node->m_win, &node->m_x, &node->m_y);
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

class $modify(MHObject, GameObject) {
	static GameObject* createWithKey(int key) {
		if (!mhEnabled() || (key != vibrateID && key != toastID)) return GameObject::createWithKey(key);
		auto obj = GameObject::createWithKey(templateFor(key));
		if (obj) obj->m_objectID = key;
		return obj;
	}
};

class $modify(MHEffect, EffectGameObject) {
	struct Fields {
		std::string text;
	};

	void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
		if (mhEnabled() && m_objectID == vibrateID) return vibrate();
		if (mhEnabled() && m_objectID == toastID) return toast(m_fields->text);
		EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
	}

	gd::string getSaveString(GJBaseGameLayer* layer) {
		std::string s = EffectGameObject::getSaveString(layer);
		if (m_objectID == toastID && !m_fields->text.empty()) {
			if (s.ends_with(',')) s.pop_back();
			s += fmt::format(",31,{}", base64::encode(m_fields->text, base64::Base64Variant::UrlWithPad));
		}
		return s;
	}

	void customObjectSetup(gd::vector<gd::string>& values, gd::vector<void*>& exists) {
		EffectGameObject::customObjectSetup(values, exists);
		if (m_objectID != toastID || values.size() <= 31) return;
		std::string raw = values[31];
		if (raw.empty()) return;
		auto res = base64::decodeString(raw, base64::Base64Variant::UrlWithPad);
		if (res) m_fields->text = res.unwrap();
	}
};

class $modify(MHToolbox, ObjectToolbox) {
	bool init() {
		if (!ObjectToolbox::init()) return false;
		if (!mhEnabled()) return true;
		// same icon as the template trigger
		for (int id : {vibrateID, toastID}) {
			auto it = m_allKeys.find(templateFor(id));
			if (it != m_allKeys.end()) m_allKeys[id] = it->second;
		}
		return true;
	}
};

class MHPopup : public SetupTriggerPopup {
	CCTextInputNode* m_input = nullptr;

	bool setupPopup(EffectGameObject* obj, CCArray* objs) {
		bool hasText = obj->m_objectID == toastID;
		if (!SetupTriggerPopup::init(obj, objs, 320.f, hasText ? 240.f : 200.f, 1)) return false;

		this->addTitle(hasText ? "Toast Trigger" : "Vibrate Trigger");
		this->addCloseButton("OK");
		this->createMultiTriggerItemsDefault();

		if (hasText) {
			auto bg = extension::CCScale9Sprite::create("square02b_001.png", { 0, 0, 80, 80 });
			bg->setContentSize({ 260.f, 40.f });
			bg->setColor({ 0, 0, 0 });
			bg->setOpacity(90);
			bg->setPosition({ 0.f, 40.f });
			m_buttonMenu->addChild(bg);

			m_input = CCTextInputNode::create(240.f, 30.f, "Text", "bigFont.fnt");
			m_input->setPosition({ 0.f, 40.f });
			m_input->setLabelPlaceholderColor({ 120, 120, 120 });
			m_input->setMaxLabelLength(100);
			m_input->setMaxLabelScale(0.6f);
			m_input->setDelegate(this);
			m_input->setString(static_cast<MHEffect*>(obj)->m_fields->text);
			m_buttonMenu->addChild(m_input);
		}

		this->determineStartValues();
		return true;
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

	void textChanged(CCTextInputNode* node) override {
		if (!m_input || node != m_input) return SetupTriggerPopup::textChanged(node);
		std::string str = node->getString();
		for (auto o : CCArrayExt<EffectGameObject*>(m_gameObjects)) {
			static_cast<MHEffect*>(o)->m_fields->text = str;
		}
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

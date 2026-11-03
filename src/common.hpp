#pragma once
#include <Geode/Geode.hpp>

inline bool mhEnabled() {
	static bool v = geode::Mod::get()->getSettingValue<bool>("enabled");
	return v;
}

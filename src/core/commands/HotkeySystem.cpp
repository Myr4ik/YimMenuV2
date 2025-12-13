#include "HotkeySystem.hpp"
#include "core/backend/FiberPool.hpp" // Вернули для корректного выполнения команд
#include "core/backend/ScriptMgr.hpp"
#include "Commands.hpp"
#include "LoopedCommand.hpp"
#include "core/util/Joaat.hpp"

// TODO: serialization isn't stable

#include "game/pointers/Pointers.hpp" // game import in core!
#include "game/gta/Natives.hpp"       // game import in core!
#include "game/frontend/GUI.hpp"

namespace YimMenu
{
	HotkeySystem::HotkeySystem() :
	    IStateSerializer("hotkeys")
	{
	}

	void HotkeySystem::RegisterCommands()
	{
		auto& cmds = Commands::GetCommands();

		for (auto& [hash, cmd] : cmds)
		{
			CommandLink link;
			m_CommandHotkeys.insert(std::make_pair(hash, link));
		}
		
		// Жесткая привязка чата убрана по просьбе.
	}

	bool HotkeySystem::ListenAndApply(int& Hotkey, std::vector<int> Blacklist)
	{
		static auto IsKeyBlacklisted = [Blacklist](int Key) -> bool {
			for (auto Key_ : Blacklist)
				if (Key_ == Key)
					return true;

			return false;
		};

		// VK_OEM_CLEAR Is about the limit in terms of virtual key codes
		for (int i = 0; i < VK_OEM_CLEAR; i++)
		{
			if ((GetKeyState(i) & 0x8000) && i != 1 && !IsKeyBlacklisted(i))
			{
				Hotkey = i;
				return true;
			}
		}

		return false;
	}

	std::string HotkeySystem::GetHotkeyLabel(int HotkeyModifier)
	{
		char KeyName[32];
		GetKeyNameTextA(MapVirtualKey(HotkeyModifier, MAPVK_VK_TO_VSC) << 16, KeyName, 32);

		if (std::string(KeyName).empty())
			strcpy(KeyName, std::to_string(HotkeyModifier).data());

		return KeyName;
	}

	// Функция проверки, используется ли уже такая цепочка клавиш другой командой
	bool HotkeySystem::IsChainUsed(const std::vector<int>& chain)
	{
		if (chain.empty()) return false;

		for (auto& [hash, link] : m_CommandHotkeys)
		{
			// Сравниваем цепочки. Если они идентичны по длине и содержанию -> конфликт
			if (link.m_Chain.size() == chain.size())
			{
				if (std::equal(link.m_Chain.begin(), link.m_Chain.end(), chain.begin()))
				{
					// Здесь можно получить имя команды через Commands::GetCommand(hash)->GetName() для лога
					return true; 
				}
			}
		}
		return false;
	}

	void HotkeySystem::CreateHotkey(std::vector<int>& chain)
	{
		static auto is_key_unique_in_chain = [](int Key, std::vector<int> List) -> bool {
			for (auto& _key : List)
				if (_key == Key)
					return false;
			return true;
		};

		int pressed_key = 0;
		if (ListenAndApply(pressed_key, chain))
		{
			// Сначала проверяем, нет ли дубликата самой клавиши в текущей цепочке (например F1 + F1)
			if (is_key_unique_in_chain(pressed_key, chain))
			{
				// Создаем временную копию цепочки, какой она станет после добавления
				std::vector<int> potential_chain = chain;
				potential_chain.push_back(pressed_key);

				// ПРОВЕРКА КОНФЛИКТОВ
				// Блокируем создание дубликата и пишем варнинг.
				if (IsChainUsed(potential_chain))
				{
					LOG(WARNING) << "Hotkey conflict detected! Key combination is already in use.";
					return; 
				}

				// Если конфликтов нет, применяем
				chain.push_back(pressed_key);
				MarkStateDirty();
			}
		}
	}

	void HotkeySystem::RunScriptImpl()
	{
		while (g_Running)
		{
			if (GetForegroundWindow() == *Pointers.Hwnd && !HUD::IS_PAUSE_MENU_ACTIVE() && !HUD::IS_SOCIAL_CLUB_ACTIVE() && !m_BeingModified && !GUI::IsUsingKeyboard())
			{
				for (auto& [hash, link] : m_CommandHotkeys)
				{
					if (link.m_Chain.empty())
						continue;

					bool all_keys_pressed = true;

					for (auto modifier : link.m_Chain)
					{
						if (!(GetAsyncKeyState(modifier) & 0x8000))
						{
							all_keys_pressed = false;
						}
					}

					if (all_keys_pressed && std::chrono::system_clock::now() - m_LastHotkeyTriggerTime > 100ms)
					{
						auto command = Commands::GetCommand(hash);
						if (command)
						{
							// FIX: Запускаем ВСЕ команды через FiberPool.
							// Это чинит ChatHelper (ему нужен скриптовый поток) и предотвращает 
							// подвисание системы ввода, если команда выполняется долго.
							FiberPool::Push([command] {
								command->Call();
							});
						}
						m_LastHotkeyTriggerTime = std::chrono::system_clock::now();
					}
				}
			}
			ScriptMgr::Yield();
		}
	}

	void HotkeySystem::RunScript()
	{
		g_HotkeySystem.RunScriptImpl();
	}

	void HotkeySystem::SaveStateImpl(nlohmann::json& state)
	{
		state.clear(); // FIX: Очистка старого состояния

		for (auto& hotkey : m_CommandHotkeys)
		{
			if (!hotkey.second.m_Chain.empty())
			{
				state[std::to_string(hotkey.first).data()] = hotkey.second.m_Chain;
			}
		}
	}

	void HotkeySystem::LoadStateImpl(nlohmann::json& state)
	{
		for (auto& [key, value] : state.items())
		{
			// FIX: Использование strtoul для больших хэшей
			auto hash = std::strtoul(key.data(), nullptr, 10);
			
			if (m_CommandHotkeys.contains(hash))
				m_CommandHotkeys[hash].m_Chain = value.get<std::vector<int>>();
		}
	}

	void HotkeySystem::SetBeingModifed(bool being_modified)
	{
		g_HotkeySystem.m_BeingModified = being_modified;
	}
}

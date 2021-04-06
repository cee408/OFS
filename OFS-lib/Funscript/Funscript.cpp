#include "Funscript.h"

#include "SDL.h"
#include "OFS_Util.h"
#include "OFS_Profiling.h"

#include "EventSystem.h"
#include "OFS_Serialization.h"
#include "FunscriptUndoSystem.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

Funscript::Funscript() 
{
	NotifyActionsChanged(false);
	saveMutex = SDL_CreateMutex();
	undoSystem = std::make_unique<FunscriptUndoSystem>(this);
}

Funscript::~Funscript()
{
	SDL_DestroyMutex(saveMutex);
}

void Funscript::setBaseScript(nlohmann::json& base)
{
	// the point of BaseLoaded is to not wipe any attributes added by other tools
	// BaseLoaded is supposed to hold everything that isn't generated by OFS
	BaseLoaded = base;
	BaseLoaded.erase("actions");
	BaseLoaded.erase("version");
	BaseLoaded.erase("inverted");
	BaseLoaded.erase("range");
	BaseLoaded.erase("OpenFunscripter");
	BaseLoaded.erase("metadata");
}

void Funscript::NotifySelectionChanged() noexcept
{
	selectionChanged = true;
}

void Funscript::loadMetadata() noexcept
{
	if (Json.contains("metadata")) {
		auto& meta = Json["metadata"];
		OFS::serializer::load(&metadata, &meta);
	}
}

void Funscript::saveMetadata() noexcept
{
	OFS_BENCHMARK(__FUNCTION__);
	OFS::serializer::save(&metadata, &Json["metadata"]);
}

void Funscript::startSaveThread(const std::string& path, std::vector<FunscriptAction>&& actions, nlohmann::json&& json) noexcept
{
	OFS_BENCHMARK(__FUNCTION__);
	struct SaveThreadData {
		nlohmann::json jsonObj;
		std::vector<FunscriptAction> actions;
		std::string path;
		nlohmann::json* base;
		SDL_mutex* mutex;
	};
	SaveThreadData* threadData = new SaveThreadData();
	threadData->mutex = saveMutex;
	threadData->path = path;
	threadData->jsonObj = std::move(json); // give ownership to the thread
	threadData->actions = std::move(actions);
	threadData->base = &BaseLoaded;

	auto thread = [](void* user) -> int {
		OFS_BENCHMARK("SaveFunscriptThread");
		SaveThreadData* data = static_cast<SaveThreadData*>(user);
		SDL_LockMutex(data->mutex);

		data->jsonObj["actions"] = nlohmann::json::array();
		data->jsonObj["version"] = "1.0";
		data->jsonObj["inverted"] = false;
		data->jsonObj["range"] = 100; // I think this is mostly ignored anyway

		data->jsonObj.merge_patch(*data->base);
		auto& actions = data->jsonObj["actions"];
		for (auto&& action : data->actions) {
			// a little validation just in case
			if (action.at < 0)
				continue;

			nlohmann::json actionObj = {
				{ "at", action.at },
				{ "pos", Util::Clamp<int32_t>(action.pos, 0, 100) }
			};
			actions.emplace_back(std::move(actionObj));
	}

#ifdef NDEBUG
		Util::WriteJson(data->jsonObj, data->path.c_str());
#else
		Util::WriteJson(data->jsonObj, data->path.c_str(), true);
#endif
		SDL_UnlockMutex(data->mutex);
		delete data;
		return 0;
	};
	auto handle = SDL_CreateThread(thread, "SaveScriptThread", threadData);
	SDL_DetachThread(handle);
}

void Funscript::update() noexcept
{
	if (funscriptChanged) {
		funscriptChanged = false;
		SDL_Event ev;
		ev.type = FunscriptEvents::FunscriptActionsChangedEvent;
		SDL_PushEvent(&ev);

		// TODO: find out how expensive this is on an already sorted array
		sortActions(data.Actions);
	}
	if (selectionChanged) {
		selectionChanged = false;
		SDL_Event ev;
		ev.type = FunscriptEvents::FunscriptSelectionChangedEvent;
		SDL_PushEvent(&ev);
	}
}

void Funscript::saveMinium(const std::string& path) noexcept
{
	saveMetadata();

	auto& actions = Json["actions"];
	actions.clear();
	sortActions(data.Actions);
	
	std::vector<FunscriptAction> filteredActions;
	if (data.Actions.size() >= 3) {
		filteredActions.reserve(data.Actions.size());
		filteredActions.emplace_back(data.Actions.front());
		for (int i = 1; i < data.Actions.size() - 1; i++) {
			auto previous = filteredActions.back();
			auto current = data.Actions[i];
			auto next = data.Actions[i + 1];

			float speedPreviousToNext = std::abs(previous.pos - next.pos) / (float)(next.at - previous.at);

			float speedPreviousToCurrent = std::abs(previous.pos - current.pos) / (float)(current.at - previous.at);
			float speedCurrentToNext = std::abs(current.pos - next.pos) / (float)(next.at - current.at);

			float avgSpeedSegments = (speedPreviousToCurrent + speedCurrentToNext) / 2.f;

			if (std::abs(speedPreviousToNext - avgSpeedSegments) > (speedPreviousToNext * 0.005)) {
				filteredActions.emplace_back(current);
			}
		}
		filteredActions.emplace_back(data.Actions.back());
	}
	else { filteredActions = data.Actions; }

	startSaveThread(path, std::move(filteredActions), std::move(Json));
}

float Funscript::GetPositionAtTime(int32_t time_ms) noexcept
{
	if (data.Actions.size() == 0) {	return 0; } 
	else if (data.Actions.size() == 1) return data.Actions[0].pos;

	for (int i = 0; i < data.Actions.size()-1; i++) {
		auto& action = data.Actions[i];
		auto& next = data.Actions[i + 1];

		if (time_ms > action.at && time_ms < next.at) {
			// interpolate position
			int32_t last_pos = action.pos;
			float diff = next.pos - action.pos;
			float progress = (float)(time_ms - action.at) / (next.at - action.at);
			
			float interp = last_pos +(progress * (float)diff);
			return interp;
		}
		else if (action.at == time_ms) {
			return action.pos;
		}

	}

	return data.Actions.back().pos;
}

FunscriptAction* Funscript::getAction(FunscriptAction action) noexcept
{
	auto it = std::find(data.Actions.begin(), data.Actions.end(), action);
	if (it != data.Actions.end())
		return &(*it);
	return nullptr;
}

FunscriptAction* Funscript::getActionAtTime(std::vector<FunscriptAction>& actions, int32_t time_ms, uint32_t max_error_ms) noexcept
{
	// gets an action at a time with a margin of error
	int32_t smallest_error = std::numeric_limits<int32_t>::max();
	FunscriptAction* smallest_error_action = nullptr;

	for (int i = 0; i < actions.size(); i++) {
		auto& action = actions[i];
		
		if (action.at > (time_ms + (max_error_ms/2)))
			break;

		int32_t error = std::abs(time_ms - action.at);
		if (error <= max_error_ms) {
			if (error <= smallest_error) {
				smallest_error = error;
				smallest_error_action = &action;
			}
			else {
				break;
			}
		}
	}
	return smallest_error_action;
}

FunscriptAction* Funscript::getNextActionAhead(int32_t time_ms) noexcept
{
	auto it = std::find_if(data.Actions.begin(), data.Actions.end(), 
		[&](auto& action) {
			return action.at > time_ms;
	});

	if (it != data.Actions.end())
		return &(*it);

	return nullptr;
}

FunscriptAction* Funscript::getPreviousActionBehind(int32_t time_ms) noexcept
{
	auto it = std::find_if(data.Actions.rbegin(), data.Actions.rend(),
		[&](auto& action) {
			return action.at < time_ms;
		});
	
	if (it != data.Actions.rend())
		return &(*it);

	return nullptr;
}

void Funscript::AddActionSafe(FunscriptAction newAction) noexcept
{
	auto it = std::find_if(data.Actions.begin(), data.Actions.end(), [&](auto&& action) {
		return newAction.at < action.at;
		});
	// checks if there's already an action with the same timestamp
	auto safety = std::find_if(it > data.Actions.begin() ? it - 1 : data.Actions.begin(), data.Actions.end(),
		[&](auto&& action) {
			return newAction.at == action.at;
		});
	if (safety == data.Actions.end()) {
		data.Actions.insert(it, newAction);
		NotifyActionsChanged(true);
	}
	else
	{
		LOGF_WARN("Failed to add action because there's already an action at %d ms", newAction.at);
	}
}

void Funscript::AddActionRange(const std::vector<FunscriptAction>& range, bool checkDuplicates) noexcept
{
	if (checkDuplicates) {
		std::unordered_set<FunscriptAction, FunscriptActionHashfunction> set;
		set.insert(data.Actions.begin(), data.Actions.end());
		for (auto action : range) {
			if (set.find(action) == set.end()) {
				data.Actions.push_back(action);
			}
		}
	}
	else {
		data.Actions.insert(data.Actions.end(), range.begin(), range.end());
	}

	sortActions(data.Actions);
	NotifyActionsChanged(true);
}

bool Funscript::EditAction(FunscriptAction oldAction, FunscriptAction newAction) noexcept
{
	// update action
	auto act = getAction(oldAction);
	if (act != nullptr) {
		act->at = newAction.at;
		act->pos = newAction.pos;
		checkForInvalidatedActions();
		NotifyActionsChanged(true);
		return true;
	}
	return false;
}

void Funscript::AddEditAction(FunscriptAction action, float frameTimeMs) noexcept
{
	auto close = getActionAtTime(data.Actions, action.at, frameTimeMs);
	if (close != nullptr) {
		*close = action;
		NotifyActionsChanged(true);
		checkForInvalidatedActions();
	}
	else {
		AddAction(action);
	}
}

void Funscript::PasteAction(FunscriptAction paste, int32_t error_ms) noexcept
{
	auto act = GetActionAtTime(paste.at, error_ms);
	if (act != nullptr) {
		RemoveAction(*act);
	}
	AddAction(paste);
	NotifyActionsChanged(true);
}

void Funscript::checkForInvalidatedActions() noexcept
{
	auto it = std::remove_if(data.selection.begin(), data.selection.end(), [&](auto&& selected) {
		auto found = getAction(selected);
		if (found == nullptr)
			return true;
		return false;
	});
	if (it != data.selection.end()) {
		data.selection.erase(it);
		NotifySelectionChanged();
	}
}

void Funscript::RemoveAction(FunscriptAction action, bool checkInvalidSelection) noexcept
{
	auto it = std::find(data.Actions.begin(), data.Actions.end(), action);
	if (it != data.Actions.end()) {
		data.Actions.erase(it);
		NotifyActionsChanged(true);

		if (checkInvalidSelection) { checkForInvalidatedActions(); }
	}
}

void Funscript::RemoveActions(const std::vector<FunscriptAction>& removeActions) noexcept
{
	for (auto&& action : removeActions)
		RemoveAction(action, false);
	NotifyActionsChanged(true);
}

std::vector<FunscriptAction> Funscript::GetLastStroke(int32_t time_ms) noexcept
{
	// TODO: refactor...
	// assuming "*it" is a peak bottom or peak top
	// if you went up it would return a down stroke and if you went down it would return a up stroke
	auto it = std::min_element(data.Actions.begin(), data.Actions.end(),
		[&](auto&& a, auto&& b) {
			return std::abs(a.at - time_ms) < std::abs(b.at - time_ms);
		});
	if (it == data.Actions.end() || it-1 == data.Actions.begin()) return std::vector<FunscriptAction>(0);

	std::vector<FunscriptAction> stroke;
	stroke.reserve(5);

	// search previous stroke
	bool goingUp = (it - 1)->pos > it->pos;
	int32_t prevPos = (it-1)->pos;
	for (auto searchIt = it-1; searchIt != data.Actions.begin(); searchIt--) {
		if ((searchIt - 1)->pos > prevPos != goingUp) {
			break;
		}
		else if ((searchIt - 1)->pos == prevPos && (searchIt-1)->pos != searchIt->pos) {
			break;
		}
		prevPos = (searchIt - 1)->pos;
		it = searchIt;
	}

	it--;
	if (it == data.Actions.begin()) return std::vector<FunscriptAction>(0);
	goingUp = !goingUp;
	prevPos = it->pos;
	stroke.emplace_back(*it);
	it--;
	for (;; it--) {
		bool up = it->pos > prevPos;
		if (up != goingUp) {
			break;
		}
		else if (it->pos == prevPos) {
			break;
		}
		stroke.emplace_back(*it);
		prevPos = it->pos;
		if (it == data.Actions.begin()) break;
	}
	return stroke;
}

void Funscript::SetActions(const std::vector<FunscriptAction>& override_with) noexcept
{
	data.Actions.clear();
	data.Actions.assign(override_with.begin(), override_with.end());
	sortActions(data.Actions);
	NotifyActionsChanged(true);
}

void Funscript::RemoveActionsInInterval(int32_t fromMs, int32_t toMs) noexcept
{
	data.Actions.erase(
		std::remove_if(data.Actions.begin(), data.Actions.end(),
			[&](auto action) {
				return action.at >= fromMs && action.at <= toMs;
			}), data.Actions.end()
	);
	checkForInvalidatedActions();
	NotifyActionsChanged(true);
}

void Funscript::RangeExtendSelection(int32_t rangeExtend) noexcept
{
	auto ExtendRange = [](std::vector<FunscriptAction*>& actions, int32_t rangeExtend) -> void {
		if (rangeExtend == 0) { return; }
		if (actions.size() < 0) { return; }

		auto StretchPosition = [](int32_t position, int32_t lowest, int32_t highest, int extension) -> int32_t
		{
			int32_t newHigh = Util::Clamp<int32_t>(highest + extension, 0, 100);
			int32_t newLow = Util::Clamp<int32_t>(lowest - extension, 0, 100);

			double relativePosition = (position - lowest) / (double)(highest - lowest);
			double newposition = relativePosition * (newHigh - newLow) + newLow;

			return Util::Clamp<int32_t>(newposition, 0, 100);
		};

		int lastExtremeIndex = 0;
		int32_t lastValue = (*actions[0]).pos;
		int32_t lastExtremeValue = lastValue;

		int32_t lowest = lastValue;
		int32_t highest = lastValue;

		enum class direction {
			NONE,
			UP,
			DOWN
		};
		direction strokeDir = direction::NONE;

		for (int index = 0; index < actions.size(); index++)
		{
			// Direction unknown
			if (strokeDir == direction::NONE)
			{
				if ((*actions[index]).pos < lastExtremeValue) {
					strokeDir = direction::DOWN;
				}
				else if ((*actions[index]).pos > lastExtremeValue) {
					strokeDir = direction::UP;
				}
			}
			else
			{
				if (((*actions[index]).pos < lastValue && strokeDir == direction::UP)     //previous was highpoint
					|| ((*actions[index]).pos > lastValue && strokeDir == direction::DOWN) //previous was lowpoint
					|| (index == actions.size() - 1))                            //last action
				{
					for (int i = lastExtremeIndex + 1; i < index; i++)
					{
						FunscriptAction& action = *actions[i];
						action.pos = StretchPosition(action.pos, lowest, highest, rangeExtend);
					}

					lastExtremeValue = (*actions[index - 1]).pos;
					lastExtremeIndex = index - 1;

					highest = lastExtremeValue;
					lowest = lastExtremeValue;

					strokeDir = (strokeDir == direction::UP) ? direction::DOWN : direction::UP;
				}

			}
			lastValue = (*actions[index]).pos;
			if (lastValue > highest)
				highest = lastValue;
			if (lastValue < lowest)
				lowest = lastValue;
		}
	};
	std::vector<FunscriptAction*> rangeExtendSelection;
	rangeExtendSelection.reserve(SelectionSize());
	int selectionOffset = 0;
	for (auto&& act : data.Actions) {
		for (int i = selectionOffset; i < data.selection.size(); i++) {
			if (data.selection[i] == act) {
				rangeExtendSelection.push_back(&act);
				selectionOffset = i;
				break;
			}
		}
	}
	if (rangeExtendSelection.size() == 0) { return; }
	ClearSelection();
	ExtendRange(rangeExtendSelection, rangeExtend);
}

bool Funscript::ToggleSelection(FunscriptAction action) noexcept
{
	auto it = std::find(data.selection.begin(), data.selection.end(), action);
	bool is_selected = it != data.selection.end();
	if (is_selected) {
		data.selection.erase(it);
	}
	else {
		data.selection.emplace_back(action);
	}
	NotifySelectionChanged();
	return !is_selected;
}

void Funscript::SetSelection(FunscriptAction action, bool selected) noexcept
{
	auto it = std::find(data.selection.begin(), data.selection.end(), action);
	bool is_selected = it != data.selection.end();
	if(is_selected && !selected)
	{
		data.selection.erase(it);
	}
	else if(!is_selected && selected) {
		data.selection.emplace_back(action);
		sortSelection();
	}
	NotifySelectionChanged();
}

void Funscript::SelectTopActions()
{
	if (data.selection.size() < 3) return;
	std::vector<FunscriptAction> deselect;
	for (int i = 1; i < data.selection.size() - 1; i++) {
		auto& prev = data.selection[i - 1];
		auto& current = data.selection[i];
		auto& next = data.selection[i + 1];

		auto& min1 = prev.pos < current.pos ? prev : current;
		auto& min2 = min1.pos < next.pos ? min1 : next;
		deselect.emplace_back(min1);
		if (min1.at != min2.at)
			deselect.emplace_back(min2);

	}
	for (auto& act : deselect)
		SetSelection(act, false);
	NotifySelectionChanged();
}

void Funscript::SelectBottomActions()
{
	if (data.selection.size() < 3) return;
	std::vector<FunscriptAction> deselect;
	for (int i = 1; i < data.selection.size() - 1; i++) {
		auto& prev = data.selection[i - 1];
		auto& current = data.selection[i];
		auto& next = data.selection[i + 1];

		auto& max1 = prev.pos > current.pos ? prev : current;
		auto& max2 = max1.pos > next.pos ? max1 : next;
		deselect.emplace_back(max1);
		if (max1.at != max2.at)
			deselect.emplace_back(max2);

	}
	for (auto& act : deselect)
		SetSelection(act, false);
	NotifySelectionChanged();
}

void Funscript::SelectMidActions()
{
	if (data.selection.size() < 3) return;
	auto selectionCopy = data.selection;
	SelectTopActions();
	auto topPoints = data.selection;
	data.selection = selectionCopy;
	SelectBottomActions();
	auto bottomPoints = data.selection;

	selectionCopy.erase(std::remove_if(selectionCopy.begin(), selectionCopy.end(),
		[&](auto val) {
			return std::any_of(topPoints.begin(), topPoints.end(), [&](auto a) { return a == val; })
				|| std::any_of(bottomPoints.begin(), bottomPoints.end(), [&](auto a) { return a == val; });
		}), selectionCopy.end());
	data.selection = selectionCopy;
	sortSelection();
	NotifySelectionChanged();
}

void Funscript::SelectTime(int32_t from_ms, int32_t to_ms, bool clear) noexcept
{
	if(clear)
		ClearSelection();

	for (auto& action : data.Actions) {
		if (action.at >= from_ms && action.at <= to_ms) {
			ToggleSelection(action);
		}
		else if (action.at > to_ms)
			break;
	}

	if (!clear)
		sortSelection();
	NotifySelectionChanged();
}

void Funscript::SelectAction(FunscriptAction select) noexcept
{
	auto action = GetAction(select);
	if (action != nullptr) {
		if (ToggleSelection(select)) {
			// keep selection ordered for rendering purposes
			sortSelection();
		}
		NotifySelectionChanged();
	}
}

void Funscript::DeselectAction(FunscriptAction deselect) noexcept
{
	auto action = GetAction(deselect);
	if (action != nullptr)
		SetSelection(*action, false);
	NotifySelectionChanged();
}

void Funscript::SelectAll() noexcept
{
	ClearSelection();
	data.selection.assign(data.Actions.begin(), data.Actions.end());
	NotifySelectionChanged();
}

void Funscript::RemoveSelectedActions() noexcept
{
	RemoveActions(data.selection);
	ClearSelection();
	//NotifyActionsChanged(); // already called in RemoveActions
	NotifySelectionChanged();
}

void Funscript::moveActionsTime(std::vector<FunscriptAction*> moving, int32_t time_offset)
{
	ClearSelection();
	for (auto move : moving) {
		move->at += time_offset;
	}
	NotifyActionsChanged(true);
}

void Funscript::moveActionsPosition(std::vector<FunscriptAction*> moving, int32_t pos_offset)
{
	ClearSelection();
	for (auto move : moving) {
		move->pos += pos_offset;
		move->pos = Util::Clamp<int16_t>(move->pos, 0, 100);
	}
	NotifyActionsChanged(true);
}

void Funscript::MoveSelectionTime(int32_t time_offset, float frameTimeMs) noexcept
{
	if (!HasSelection()) return;
	std::vector<FunscriptAction*> moving;

	// faster path when everything is selected
	if (data.selection.size() == data.Actions.size()) {
		for (auto& action : data.Actions)
			moving.push_back(&action);
		moveActionsTime(moving, time_offset);
		SelectAll();
		return;
	}

	auto prev = GetPreviousActionBehind(data.selection.front().at);
	auto next = GetNextActionAhead(data.selection.back().at);

	int32_t min_bound = 0;
	int32_t max_bound = std::numeric_limits<int32_t>::max();

	if (time_offset > 0) {
		if (next != nullptr) {
			max_bound = next->at - frameTimeMs;
			time_offset = std::min(time_offset, max_bound - data.selection.back().at);
		}
	}
	else
	{
		if (prev != nullptr) {
			min_bound = prev->at + frameTimeMs;
			time_offset = std::max(time_offset, min_bound - data.selection.front().at);
		}
	}

	for (auto& find : data.selection) {
		auto m = getAction(find);
		if(m != nullptr)
			moving.push_back(m);
	}

	ClearSelection();
	for (auto move : moving) {
		move->at += time_offset;
		data.selection.emplace_back(*move);
	}
	NotifyActionsChanged(true);
}

void Funscript::MoveSelectionPosition(int32_t pos_offset) noexcept
{
	if (!HasSelection()) return;
	std::vector<FunscriptAction*> moving;
	
	// faster path when everything is selected
	if (data.selection.size() == data.Actions.size()) {
		for (auto& action : data.Actions)
			moving.push_back(&action);
		moveActionsPosition(moving, pos_offset);
		SelectAll();
		return;
	}

	for (auto& find : data.selection) {
		auto m = getAction(find);
		if (m != nullptr)
			moving.push_back(m);
	}

	ClearSelection();
	for (auto move : moving) {
		move->pos += pos_offset;
		move->pos = Util::Clamp<int16_t>(move->pos, 0, 100);
		data.selection.emplace_back(*move);
	}
	NotifyActionsChanged(true);
}

void Funscript::SetSelection(const std::vector<FunscriptAction>& action_to_select, bool unsafe) noexcept
{
	ClearSelection();
	for (auto&& action : action_to_select) {
		if (unsafe) {
			data.selection.emplace_back(action);
		}
		else {
			auto it = std::find(data.Actions.begin(), data.Actions.end(), action);
			if (it != data.Actions.end()) {
				data.selection.emplace_back(action);
			}
		}
	}
	sortSelection();
}

bool Funscript::IsSelected(FunscriptAction action) noexcept
{
	auto it = std::find(data.selection.begin(), data.selection.end(), action);
	return it != data.selection.end();
}

void Funscript::EqualizeSelection() noexcept
{
	if (data.selection.size() < 3) return;
	sortSelection(); // might be unnecessary
	auto first = data.selection.front();
	auto last = data.selection.back();
	float duration = last.at - first.at;
	float stepMs = duration / (float)(data.selection.size()-1);
		
	auto copySelection = data.selection;
	RemoveSelectedActions(); // clears selection

	for (int i = 1; i < copySelection.size()-1; i++) {
		auto& newAction = copySelection[i];
		newAction.at = first.at + std::round(i * stepMs);
	}

	for (auto& action : copySelection)
		AddAction(action);

	data.selection = std::move(copySelection);
}

void Funscript::InvertSelection() noexcept
{
	if (data.selection.size() == 0) return;
	auto copySelection = data.selection;
	RemoveSelectedActions();
	for (auto& act : copySelection)
	{
		act.pos = std::abs(act.pos - 100);
		AddAction(act);
	}
	data.selection = copySelection;
}

int32_t FunscriptEvents::FunscriptActionsChangedEvent = 0;
int32_t FunscriptEvents::FunscriptSelectionChangedEvent = 0;

void FunscriptEvents::RegisterEvents() noexcept
{
	FunscriptActionsChangedEvent = SDL_RegisterEvents(1);
	FunscriptSelectionChangedEvent = SDL_RegisterEvents(1);
}

bool Funscript::Metadata::loadFromFunscript(const std::string& path) noexcept
{
	bool succ;
	auto json = Util::LoadJson(path, &succ);
	if (succ && json.contains("metadata")) {
		OFS::serializer::load(this, &json["metadata"]);
	}
	return succ;
}

bool Funscript::Metadata::writeToFunscript(const std::string& path) noexcept
{
	bool succ;
	auto json = Util::LoadJson(path, &succ);
	if (succ) {
		json["metadata"] = nlohmann::json::object();
		OFS::serializer::save(this, &json["metadata"]);
		Util::WriteJson(json, path, false);
	}
	return succ;
}
// SPDX-License-Identifier: Apache-2.0

// 利用者が設定ウィンドウで選んだ既存の OBS のソース (自分の枠の映像) への参照。合成のソース (composite_source.cpp) の
// モジュールに 1 つ持ち、すべての合成のソースが同じソースを描く。選んだソースは UUID と弱参照で持つ
// (名前は利用者が変えることがあるが、UUID は変わらない)。描画するソースは enum_active_sources と enum_all_sources の
// 両方を実装して選んだソースを返さないと、描画するソースが表示されていても、選んだソースがアクティブにならない。

#pragma once

#include <mutex>

#include <obs-module.h>

#include "diag/diag_log.h"

namespace cordmux::sources {

class TargetRef {
public:
	TargetRef() = default;
	TargetRef(const TargetRef &) = delete;
	TargetRef &operator=(const TargetRef &) = delete;
	~TargetRef() { set_uuid(nullptr); }

	// 弱参照を読み、強い参照 (obs_source_t*) を返す (なければ nullptr)。呼び出し元が obs_source_release で解放する。
	// ロックは弱参照の入れ替えだけを守り、obs_weak_source_get_source を含む短い区間だけ取る (ロックの中で描画などの
	// 重い処理を呼ばない)。
	obs_source_t *strong_ref()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return weak_ ? obs_weak_source_get_source(weak_) : nullptr;
	}

	// uuid のソースに弱参照を向ける。空文字列か nullptr なら外す。見つからなければ外す。新しい弱参照を作ってから入れ替えるので、
	// 描画のスレッドが、入れ替えの途中で何もない状態を見ることはない。
	void set_uuid(const char *uuid)
	{
		obs_weak_source_t *new_weak = nullptr;
		if (uuid && *uuid) {
			obs_source_t *target = obs_get_source_by_uuid(uuid);
			// 利用者が OBS で削除したソースは、参照が残る間は UUID で見つかるので、見つからないものとして扱う。
			if (target != nullptr && obs_source_removed(target)) {
				obs_source_release(target);
				target = nullptr;
			}
			if (target != nullptr) {
				new_weak = obs_source_get_weak_source(target);
				obs_source_release(target);
			} else {
				diag::logf(LOG_WARNING, "Self slot video source not found (uuid=%s)", uuid);
			}
		}

		obs_weak_source_t *old_weak = nullptr;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			old_weak = weak_;
			weak_ = new_weak;
		}
		if (old_weak)
			obs_weak_source_release(old_weak);
	}

	// 選び直した結果。前の選んだソースと現在の選んだソースの強い参照で、受け取った側が obs_source_release で解放する
	// (どちらも空のことがある)。
	struct Change {
		obs_source_t *old_target = nullptr;
		obs_source_t *new_target = nullptr;
	};

	// uuid のソースに選び直し、前と現在の選んだソースを返す。親が既にアクティブか表示中のときは、enum_active_sources に
	// 任せるだけでは選び直したことが子に伝わらない (libobs は、呼んだ時点の親の状態しか子に伝えない)。シーンの
	// obs_sceneitem_set_visible と同じように、呼び出し側が返した差分を子に伝える (obs_source_add_active_child と
	// obs_source_remove_active_child を呼ぶのは、enum_active_sources と enum_all_sources を実装する親のファイルの
	// 役目である。test/lint/forbidden_libobs_usage.py)。
	Change rebind(const char *uuid)
	{
		Change change;
		change.old_target = strong_ref();
		set_uuid(uuid);
		change.new_target = strong_ref();
		return change;
	}

	// 親がアクティブか表示中か (選び直した差分を子に伝えるべきか)。
	static bool parent_is_active(obs_source_t *parent)
	{
		return obs_source_active(parent) || obs_source_showing(parent);
	}

	// enum_active_sources と enum_all_sources は同じ実装でよい (選んだソースは常に 1 つ)。
	void enumerate(obs_source_t *parent, obs_source_enum_proc_t enum_callback, void *param)
	{
		obs_source_t *target = strong_ref();
		if (!target)
			return;
		enum_callback(parent, target, param);
		obs_source_release(target);
	}

private:
	obs_weak_source_t *weak_ = nullptr;
	// weak_ を守る (rebind は UI のスレッドから、strong_ref と enumerate は描画のスレッドなどから、同時に触るため)。
	// ほかのロックを取らず、このポインタ 1 つだけを守る。
	std::mutex mutex_;
};

} // namespace cordmux::sources

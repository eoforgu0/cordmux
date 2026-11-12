// SPDX-License-Identifier: Apache-2.0

// 変更の後、少し待ってから 1 回だけ保存する (DeferredSave)。表示タブの値を動かしている間は、保存を待たせ、止めた
// 1 秒後に 1 回だけ settings.json に書く。OBS を閉じるときは flush() で、待っている保存を今行う (書き損ねて値を失わない)。

#pragma once

#include <QObject>

#include <functional>

class QTimer;

namespace cordmux::dock {

class DeferredSave : public QObject {
public:
	// delay_ms は最後の touch() から保存するまでの時間、save は保存する関数。
	DeferredSave(int delay_ms, std::function<void()> save, QObject *parent = nullptr);
	~DeferredSave() override;

	DeferredSave(const DeferredSave &) = delete;
	DeferredSave &operator=(const DeferredSave &) = delete;

	// 変更があったことを伝える。待ち時間を数え直す。
	void touch();
	// 待っている保存があれば、今行う。なければ何もしない。
	void flush();
	// 保存を待っているか。
	bool pending() const { return pending_; }

private:
	QTimer *timer_ = nullptr;
	std::function<void()> save_;
	bool pending_ = false;
};

} // namespace cordmux::dock

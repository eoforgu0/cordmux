// SPDX-License-Identifier: Apache-2.0

// sources/frame_sink_core.h (合成のソースの子にフレームを渡す、空ける手順) を、OBS の関数の代わりの偽の Port で通すテストと、
// sources/frame_origin.h (子に渡したフレームがダミーかの記録と、子のテクスチャの元) のテスト。空ける要求、フレーム、tick の順で、空けるのが tick の
// 中だけで起きること、要求の後に届いたフレームが空けるのを取り消すことを確認する。

#include "doctest.h"

#include "sources/frame_origin.h"
#include "sources/frame_sink_core.h"

#include <cstdint>
#include <string>
#include <vector>

using cordmux::sources::BasicFrameSink;
using cordmux::sources::FrameOriginLog;
using cordmux::sources::TileTextureOrigin;

namespace {

struct FakeSource {
	int id = 0;
};
struct FakeFrame {
	int number = 0;
};

// 偽の OBS。呼ばれた順に記録する ("frame N"、"null"、"unbuffered 0/1")。時刻は now で決める。
struct FakePort {
	using Source = FakeSource;
	using Frame = FakeFrame;
	static std::vector<std::string> &events()
	{
		static std::vector<std::string> list;
		return list;
	}
	static std::uint64_t &now()
	{
		static std::uint64_t value = 1'000'000'000ULL;
		return value;
	}
	static void output_frame(FakeSource * /*source*/, const FakeFrame *frame)
	{
		events().push_back("frame " + std::to_string(frame->number));
	}
	static void output_null(FakeSource * /*source*/) { events().push_back("null"); }
	static void set_unbuffered(FakeSource * /*source*/, bool unbuffered)
	{
		events().push_back(unbuffered ? "unbuffered 1" : "unbuffered 0");
	}
	static std::uint64_t now_ns() { return now(); }
};

using Sink = BasicFrameSink<FakePort>;

// 子に 1 つフレームを渡した後の状態にして、記録を空にする (最初のフレームの方式の切り替えの NULL を数えない)。
void start_with_frame(Sink &sink, FakeSource &source)
{
	FakePort::events().clear();
	FakePort::now() = 1'000'000'000ULL;
	sink.source = &source;
	FakeFrame first{1};
	sink.output(&first, true);
	FakePort::events().clear();
}

} // namespace

TEST_CASE("すぐ空ける要求は、その場では空けず、次の tick で空ける")
{
	FakeSource source;
	Sink sink;
	start_with_frame(sink, source);
	sink.clear_now();
	CHECK(FakePort::events().empty());
	sink.tick();
	CHECK(FakePort::events() == std::vector<std::string>{"null"});
	// 空けた後の tick は何もしない。
	sink.tick();
	CHECK(FakePort::events().size() == 1);
}

TEST_CASE("要求、フレーム、tick の順では、届いたフレームが要求を取り消し、tick は空けない")
{
	FakeSource source;
	Sink sink;
	start_with_frame(sink, source);
	sink.clear_now();
	FakeFrame next{2};
	sink.output(&next, true);
	sink.tick();
	CHECK(FakePort::events() == std::vector<std::string>{"frame 2"});
}

TEST_CASE("フレーム、要求、tick の順では、tick で空ける")
{
	FakeSource source;
	Sink sink;
	start_with_frame(sink, source);
	FakeFrame next{2};
	sink.output(&next, true);
	sink.clear_now();
	sink.tick();
	CHECK(FakePort::events() == std::vector<std::string>{"frame 2", "null"});
}

TEST_CASE("保ってから空ける予約は、期限の前の tick では空けず、期限の後の tick で空ける。すぐ空ける要求は予約より先に効く")
{
	FakeSource source;
	Sink sink;
	start_with_frame(sink, source);
	sink.hold_then_clear(2'000'000'000ULL);
	FakePort::now() += 1'000'000'000ULL;
	sink.tick();
	CHECK(FakePort::events().empty());
	FakePort::now() += 1'000'000'000ULL;
	sink.tick();
	CHECK(FakePort::events() == std::vector<std::string>{"null"});

	start_with_frame(sink, source);
	sink.clear_now();
	sink.hold_then_clear(2'000'000'000ULL);
	sink.tick();
	CHECK(FakePort::events() == std::vector<std::string>{"null"});
}

TEST_CASE("空いている子に空ける要求を置いても、tick は何もしない")
{
	FakeSource source;
	Sink sink;
	start_with_frame(sink, source);
	sink.clear_now();
	sink.tick();
	FakePort::events().clear();
	sink.clear_now();
	sink.tick();
	CHECK(FakePort::events().empty());
}

TEST_CASE("方式の切り替えだけは、フレームを渡す直前に NULL を渡す (tick の外。同じロックの中ですぐ次のフレームを渡す)")
{
	FakeSource source;
	Sink sink;
	start_with_frame(sink, source);
	FakeFrame next{2};
	sink.output(&next, false);
	CHECK(FakePort::events() == std::vector<std::string>{"null", "unbuffered 0", "frame 2"});
}

TEST_CASE("渡し先を外した後は、要求も tick も何もしない")
{
	FakeSource source;
	Sink sink;
	start_with_frame(sink, source);
	sink.clear_now();
	sink.detach();
	sink.tick();
	FakeFrame next{2};
	sink.output(&next, true);
	CHECK(FakePort::events().empty());
}

// ---- 子に渡したフレームがダミーかの記録と、子のテクスチャの元 ----

TEST_CASE("渡したフレームのタイムスタンプで、ダミーかを引ける。記録にない (古すぎる) タイムスタンプは引けない")
{
	FrameOriginLog log;
	log.note_output(100, false);
	log.note_output(116, true);
	bool dummy = true;
	REQUIRE(log.lookup(100, &dummy));
	CHECK_FALSE(dummy);
	REQUIRE(log.lookup(116, &dummy));
	CHECK(dummy);
	CHECK_FALSE(log.lookup(7, &dummy));
	for (std::uint64_t ts = 200; ts < 200 + FrameOriginLog::kCapacity; ++ts)
		log.note_output(ts, false);
	CHECK_FALSE(log.lookup(116, &dummy)); // 押し出された
}

TEST_CASE("同じタイムスタンプが 2 つあれば、新しく渡したほうで決める")
{
	FrameOriginLog log;
	log.note_output(42, false);
	log.note_output(42, true);
	bool dummy = false;
	REQUIRE(log.lookup(42, &dummy));
	CHECK(dummy);
}

TEST_CASE("tick で選ばれたフレームは、合成のソースが子を描いたときにテクスチャの元になる")
{
	TileTextureOrigin origin;
	CHECK_FALSE(origin.texture_dummy());
	origin.on_tick();
	origin.on_selected(true);
	// 選ばれただけでは、まだテクスチャに入っていない。
	CHECK_FALSE(origin.texture_dummy());
	origin.on_drawn();
	CHECK(origin.texture_dummy());
}

TEST_CASE("選ばれたのに描かれなかったフレームは、次の tick で捨てられ、テクスチャの元にならない")
{
	TileTextureOrigin origin;
	origin.on_tick();
	origin.on_selected(false);
	origin.on_drawn(); // 実際の人のフレームが入った
	origin.on_tick();
	origin.on_selected(true); // ダミーが選ばれたが、このフレームでは合成のソースが描かれなかった
	origin.on_tick();         // 次の tick で何も選ばれない
	origin.on_drawn();        // 描いても、テクスチャは前の実際の人のフレームのまま
	CHECK_FALSE(origin.texture_dummy());
}

TEST_CASE("同じフレームの 2 回目の描画 (プレビュー) は、テクスチャの元を変えない")
{
	TileTextureOrigin origin;
	origin.on_tick();
	origin.on_selected(true);
	origin.on_drawn();
	CHECK(origin.texture_dummy());
	// 次の tick で実際の人のフレームが選ばれるまでは、何度描いてもダミーのまま。
	origin.on_drawn();
	CHECK(origin.texture_dummy());
	origin.on_tick();
	origin.on_selected(false);
	origin.on_drawn();
	CHECK_FALSE(origin.texture_dummy());
	origin.on_drawn();
	CHECK_FALSE(origin.texture_dummy());
}

# SPDX-License-Identifier: Apache-2.0

# src-obs/sources のソースファイルを製品のターゲットに追加する。src-obs/CMakeLists.txt が
# include(sources/sources.cmake) で読み込む。ソースは libobs に依存し、libobs のリンクは src-obs/CMakeLists.txt が
# 行う。Qt6::Gui を使うのは、名前ラベルの描画 (decor_paint.h と decor_paint.cpp) と、その画像を扱う decor_registry.cpp と
# composite_decor.cpp と dummy_feed.cpp で、Qt6::Gui の追加の仕方は decor_paint.cmake にまとめてある (src-obs/tests/CMakeLists.txt の
# decor_tests も同じ関数を呼ぶ)。
# composite_layout.h と composite_roster.h と composite_input.h と composite_label_plan.h と composite_draw_plan.h と
# composite_paint_handoff.h と composite_texture_strips.h と dummy_tiles.h と、double_audio.h の純粋関数は libobs にも
# Qt にも依存しないので、テスト (align_tests) がヘッダーを直接読む。

include("${CMAKE_CURRENT_LIST_DIR}/decor_paint.cmake")
cordmux_add_decor_paint(${CMAKE_PROJECT_NAME})

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    sources/scene_names.h
    sources/scene_names.cpp
    sources/screen_feed.h
    sources/screen_feed.cpp
    sources/target_ref.h
    sources/frame_sink.h
    sources/composite_input.h
    sources/composite_source.h
    sources/composite_source.cpp
    sources/composite_decor.h
    sources/composite_decor.cpp
    sources/composite_background.h
    sources/composite_label_plan.h
    sources/composite_paint_handoff.h
    sources/composite_texture_strips.h
    sources/decor_registry.h
    sources/decor_registry.cpp
    sources/decor_params.h
    sources/self_frame_plan.h
    sources/self_frame_source.h
    sources/self_frame_source.cpp
    sources/self_speaking_judge.h
    sources/self_speaking_source.h
    sources/self_speaking_source.cpp
    sources/screen_end.h
    sources/composite_layout.h
    sources/composite_roster.h
    sources/label_border.h
    sources/dummy_tiles.h
    sources/dummy_feed.h
    sources/dummy_feed.cpp
    sources/sources.h
    sources/sources.cpp
    sources/cordmux_removal.h
    sources/cordmux_removal.cpp
    sources/double_audio.h
    sources/double_audio.cpp
)
